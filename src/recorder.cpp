// Video recorder: GDI frames + microphone -> Media Foundation sink writer -> MP4 (H.264 video, AAC or MP3 audio).
// Two worker threads (video frames on a media clock that stops during pauses; microphone buffers of 50 ms) write into
// one IMFSinkWriter. Everything the encoder needs ships with Windows, so no codec packs are required.
#include "c2s.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <strmif.h>
#include <codecapi.h>
#include <mmsystem.h>
#include <wchar.h>
#include <stdio.h>

namespace {

// CODECAPI_AVEncCommonQualityVsSpeed / ...RateControlMode (codecapi.h), spelled out.
const GUID kQualityVsSpeed  = { 0x98332df8, 0x03cd, 0x476b, { 0x89, 0xfa, 0x3f, 0x9e, 0x44, 0x2d, 0xec, 0x9f } };
const GUID kIidCodecApi    = { 0x901db4c7, 0x31ce, 0x41a2, { 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda } };
const GUID kMaxKeyDist    = { 0x2987123a, 0xba93, 0x4704, { 0xb4, 0x89, 0xec, 0x1e, 0x5f, 0x25, 0x29, 0x2c } };   // CODECAPI_AVEncVideoMaxKeyframeDistance
const GUID kGopSize        = { 0x95f31b26, 0x95a4, 0x41aa, { 0x93, 0x03, 0x24, 0x6a, 0x7f, 0xc6, 0xee, 0xf1 } };
const GUID kRateControlMode = { 0x1c0608e9, 0x370c, 0x4710, { 0x8a, 0x58, 0xcb, 0x61, 0x81, 0xc4, 0x24, 0x23 } };

// Format GUIDs spelled out (not in every header set): the FOURCC / wave-format tag + the Media Foundation base GUID.
const GUID kVideoHevc = { 0x43564548, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };   // 'HEVC'
const GUID kVideoWmv3 = { 0x33564d57, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };   // 'WMV3'
const GUID kAudioWma8 = { 0x00000161, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };   // Windows Media Audio 8

const int  kAudioRate = 44100;
const int  kAudioBufs = 8;
const int  kAudioBufMs = 50;

LONGLONG Qpc100ns(const LARGE_INTEGER& freq)
{
    LARGE_INTEGER n; QueryPerformanceCounter(&n);
    return (LONGLONG)((double)n.QuadPart * 10000000.0 / (double)freq.QuadPart);
}

} // namespace

struct Recorder::Impl {
    RecParams p;
    IMFSinkWriter* sw = nullptr;
    DWORD vStream = 0, aStream = 0;
    bool hasAudio = false, mfStarted = false, comInit = false, started = false, finalized = false;
    CRITICAL_SECTION cs;                 // the sink writer calls and the media clock
    HANDLE vThread = nullptr, aThread = nullptr;
    volatile LONG stopFlag = 0, paused = 0, failed = 0, limit = 0;
    LARGE_INTEGER freq;
    LONGLONG wall0 = 0;                  // QPC (100 ns) at the start
    LONGLONG pausedTotal = 0, pauseStart = 0;
    volatile LONGLONG mediaNow = 0;      // last media time seen by the video thread, 100 ns
    Grabber grab;
    int w = 0, h = 0, fps = 25;
    LONGLONG lastVideoTs = -1;
    LONGLONG audioSamples = 0;
    HWAVEIN hwi = nullptr;
    WAVEHDR hdr[kAudioBufs];
    BYTE* abuf[kAudioBufs];
    HANDLE aEvent = nullptr;
    bool timePeriod = false;
    bool nullSink = false;               // self-test only (C2S_NULLSINK): frames are counted, nothing is encoded
    bool wantSpk = false;                // the sound of the speakers goes into the file too
    FrameSink* alt = nullptr;            // AVI / GIF: written by our own sink instead of the Media Foundation sink writer
    int fmt = VC_H264_BEST;
    volatile LONG vFrames = 0;
    bool writeFailLogged = false;
    LONGLONG lag = 0, maxLag = 0; unsigned statTick = 0;
    unsigned long grabFails = 0; LONGLONG grabFailStart = 0; bool haveFrame = false; LONGLONG lastGrabTry = 0;
    LONGLONG grabTime = 0, writeTime = 0, loadT0 = 0; ULONGLONG loadCpu0 = 0; unsigned long loadFrames = 0;   // where the time of the video thread goes   // consecutive failed screen reads        // frames the encoder has not taken yet (now / worst), from the sink writer statistics

    Impl() { InitializeCriticalSection(&cs); QueryPerformanceFrequency(&freq); memset(hdr, 0, sizeof(hdr)); memset(abuf, 0, sizeof(abuf)); }
    ~Impl() { DeleteCriticalSection(&cs); }

    LONGLONG MediaTime()                 // wall clock without the paused spans
    {
        LONGLONG now = Qpc100ns(freq) - wall0;
        EnterCriticalSection(&cs);
        LONGLONG r = paused ? pauseStart - pausedTotal : now - pausedTotal;
        LeaveCriticalSection(&cs);
        return r < 0 ? 0 : r;
    }

    bool WriteSample(DWORD stream, const void* data, DWORD bytes, LONGLONG ts, LONGLONG dur)
    {
        if (nullSink) { if (stream == vStream) InterlockedIncrement(&vFrames); return true; }
        if (alt) {
            EnterCriticalSection(&cs);
            bool r = stream == vStream ? alt->Video((const uint8_t*)data, ts) : alt->Audio(data, bytes);
            LeaveCriticalSection(&cs);
            if (r && stream == vStream) InterlockedIncrement(&vFrames);
            return r;
        }
        IMFMediaBuffer* b = nullptr; IMFSample* s = nullptr;
        bool ok = false;
        if (SUCCEEDED(MFCreateMemoryBuffer(bytes, &b))) {
            BYTE* dst = nullptr;
            if (SUCCEEDED(b->Lock(&dst, nullptr, nullptr))) {
                memcpy(dst, data, bytes);
                b->Unlock();
                b->SetCurrentLength(bytes);
                if (SUCCEEDED(MFCreateSample(&s))) {
                    s->AddBuffer(b);
                    s->SetSampleTime(ts);
                    s->SetSampleDuration(dur);
                    EnterCriticalSection(&cs);
                    HRESULT hrw = sw->WriteSample(stream, s);
                    ok = SUCCEEDED(hrw);
                    if (!ok && !writeFailLogged) { writeFailLogged = true; Log(L"recorder: WriteSample failed, stream %lu, 0x%08lX", (unsigned long)stream, (unsigned long)hrw); }
                    LeaveCriticalSection(&cs);
                }
            }
        }
        if (s) s->Release();
        if (b) b->Release();
        return ok;
    }

    void VideoLoop()
    {
        const LONGLONG interval = 10000000LL / fps;
        LONGLONG n = 0;
        const size_t bytes = (size_t)w * h * 4;
        while (!stopFlag) {
            if (paused) { Sleep(10); continue; }
            LONGLONG mt = MediaTime();
            mediaNow = mt;
            if (alt && alt->Full()) { limit = 1; break; }       // a file that cannot grow: finish it properly
            if (mt < n * interval) {
                LONGLONG waitMs = (n * interval - mt) / 10000;
                Sleep(waitMs > 1 ? (DWORD)(waitMs - 1) : 1);
                continue;
            }
            if (sw && ((++statTick & 15) == 0 || lag > fps * 2)) {          // an encoder that lags (software H.264 at a large size): do not pile frames up in memory
                MF_SINK_WRITER_STATISTICS st; memset(&st, 0, sizeof(st)); st.cb = sizeof(st);
                if (SUCCEEDED(sw->GetStatistics(vStream, &st))) {
                    lag = (LONGLONG)st.qwNumSamplesReceived - (LONGLONG)st.qwNumSamplesEncoded;
                    if (lag > maxLag) maxLag = lag;
                }
            }
            if (lag > fps * 2) { Sleep(10); continue; }
            RECT src = p.rc;
            bool again = false;                                    // the picture of the last frame goes in once more (nothing new can be read)
            if (p.hwnd) {
                if (!IsWindow(p.hwnd)) { limit = 1; break; }       // the recorded window was closed
                if (IsIconic(p.hwnd)) { if (!haveFrame) { Sleep(20); continue; } again = true; }     // minimized: the last picture stands
                RECT r;
                if (!again && WindowVisibleRect(p.hwnd, &r) && r.right - r.left > 1 && r.bottom - r.top > 1) src = r;
            }
            if (!again && grabFails && haveFrame && mt - lastGrabTry < 2500000LL) again = true;       // while the screen is lost it is tried four times a second
            if (!again) lastGrabTry = mt;
            LONGLONG tg0 = Qpc100ns(freq);
            SetLastError(0);
            if (!again && !grab.Grab(src, p.cfg.capMouse, p.cfg.highlight, true)) {
                // The screen cannot be read (e.g. the Remote Desktop client is minimized, error 5). The frames must not stop: a gap of the
                // time stamps would be filled by the encoder at once after it (a Remote Desktop gap of 150 s was ~3700 frames made in a
                // burst: 8 cores for 20 s and a file that grew by tens of megabytes). So the last picture goes in on the usual schedule.
                if (grabFails == 0) { grabFailStart = mt; Log(L"capture: the screen cannot be read (error %lu), the last picture is repeated", (unsigned long)GetLastError()); }
                ++grabFails;
                if (!haveFrame) { Sleep(grabFails < 8 ? 20 : grabFails < 40 ? 100 : 250); continue; }
                again = true;
            }
            if (!again && grabFails) { Log(L"capture: the screen is readable again after %.1f s (%lu failed attempts)", (mt - grabFailStart) / 1e7, grabFails); grabFails = 0; }
            if (!again) haveFrame = true;
            LONGLONG tg1 = Qpc100ns(freq);
            LONGLONG ts = mt;
            if (ts <= lastVideoTs) ts = lastVideoTs + 1;
            lastVideoTs = ts;
            if (!WriteSample(vStream, grab.Bits(), (DWORD)bytes, ts, interval)) { failed = 1; break; }
            LONGLONG tw1 = Qpc100ns(freq);
            grabTime += tg1 - tg0; writeTime += tw1 - tg1; ++loadFrames;
            if (tw1 - loadT0 >= 20000000LL) {                  // every 2 s: when the process is busy, say where the time goes
                FILETIME c, e, k, u; ULONGLONG cpu = 0;
                if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) cpu = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
                if (loadCpu0) {
                    double busy = (double)(cpu - loadCpu0) / (double)(tw1 - loadT0) * 100.0;      // % of one core, both are 100 ns units
                    if (busy > 40.0 && loadFrames)
                        Log(L"load: process %.0f %% of a core, per frame: screen read %.1f ms, encoder input %.1f ms, %lu frames in 2 s, encoder lag %lld", busy,
                            grabTime / 1e4 / loadFrames, writeTime / 1e4 / loadFrames, loadFrames, (long long)lag);
                }
                loadCpu0 = cpu; loadT0 = tw1; grabTime = writeTime = 0; loadFrames = 0;
            }
            n = mt / interval + 1;        // a late frame does not cause a burst of catch-up frames
        }
    }

    // The sound: the microphone (waveIn) and the speakers (WASAPI loopback) are queued, and the mix is written in step with
    // the media clock (the pauses are not in it): what a device has not delivered yet is silence, so a quiet speaker, which
    // delivers nothing, does not let the sound drift away from the picture.
    void AudioLoop()
    {
        HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        SpeakerCapture* spk = nullptr;
        if (wantSpk) {
            spk = new SpeakerCapture;
            if (spk->Open()) Log(L"sound: the speakers are captured");
            else { Log(L"sound: no loopback capture of the speakers here"); delete spk; spk = nullptr; }
        }
        Fifo16 micQ, spkQ;
        static const int kChunk = 4096;
        int16_t tmp[8192], a[kChunk], b[kChunk], mixed[kChunk];
        while (!stopFlag) {
            if (aEvent) WaitForSingleObject(aEvent, 20); else Sleep(20);
            if (hwi) {
                for (int i = 0; i < kAudioBufs; ++i) {
                    WAVEHDR& hd = hdr[i];
                    if (!(hd.dwFlags & WHDR_DONE)) continue;
                    if (hd.dwBytesRecorded) micQ.Push((const int16_t*)hd.lpData, (int)(hd.dwBytesRecorded / 2));
                    hd.dwFlags &= ~WHDR_DONE;
                    hd.dwBytesRecorded = 0;
                    if (!stopFlag) waveInAddBuffer(hwi, &hd, sizeof(WAVEHDR));
                }
            }
            if (spk) { int n; while ((n = spk->Pull(tmp, 8192)) > 0) spkQ.Push(tmp, n); }
            if (paused) { micQ.Clear(); spkQ.Clear(); continue; }
            LONGLONG shouldHave = MediaTime() * kAudioRate / 10000000LL;
            LONGLONG want = shouldHave - audioSamples;
            if (want < kAudioRate / 100) continue;                          // less than 10 ms: later
            if (want > kAudioRate / 2) want = kAudioRate / 2;
            micQ.TrimTo((int)want + kAudioRate * 15 / 100);                 // a source that runs ahead is cut, not queued for ever
            spkQ.TrimTo((int)want + kAudioRate * 15 / 100);
            while (want > 0 && !stopFlag) {
                int n = want > kChunk ? kChunk : (int)want;
                micQ.Pop(a, n); spkQ.Pop(b, n);
                AudioMix(a, b, n, mixed);
                LONGLONG ts = audioSamples * 10000000LL / kAudioRate;
                LONGLONG dur = (LONGLONG)n * 10000000LL / kAudioRate;
                if (!WriteSample(aStream, mixed, n * 2, ts, dur)) { failed = 1; break; }
                audioSamples += n; want -= n;
            }
            if (failed) break;
        }
        delete spk;
        if (SUCCEEDED(hrCom)) CoUninitialize();
    }

    static DWORD WINAPI VThread(LPVOID a)
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);       // WIC (AVI / GIF) wants COM on the thread
        ((Impl*)a)->VideoLoop();
        if (SUCCEEDED(hr)) CoUninitialize();
        return 0;
    }
    static DWORD WINAPI AThread(LPVOID a) { ((Impl*)a)->AudioLoop(); return 0; }

    void CloseAudio()
    {
        if (hwi) {
            waveInStop(hwi);
            waveInReset(hwi);
            for (int i = 0; i < kAudioBufs; ++i)
                if (hdr[i].dwFlags & WHDR_PREPARED) waveInUnprepareHeader(hwi, &hdr[i], sizeof(WAVEHDR));
            waveInClose(hwi);
            hwi = nullptr;
        }
        for (int i = 0; i < kAudioBufs; ++i) { delete[] abuf[i]; abuf[i] = nullptr; }
        if (aEvent) { CloseHandle(aEvent); aEvent = nullptr; }
    }

    bool OpenAudio()
    {
        aEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        WAVEFORMATEX wf; memset(&wf, 0, sizeof(wf));
        wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 1; wf.nSamplesPerSec = kAudioRate; wf.wBitsPerSample = 16;
        wf.nBlockAlign = 2; wf.nAvgBytesPerSec = kAudioRate * 2;
        if (!aEvent || waveInOpen(&hwi, WAVE_MAPPER, &wf, (DWORD_PTR)aEvent, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
            hwi = nullptr; return false;
        }
        DWORD sz = kAudioRate * kAudioBufMs / 1000 * 2;
        for (int i = 0; i < kAudioBufs; ++i) {
            abuf[i] = new BYTE[sz];
            hdr[i].lpData = (LPSTR)abuf[i]; hdr[i].dwBufferLength = sz;
            if (waveInPrepareHeader(hwi, &hdr[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR ||
                waveInAddBuffer(hwi, &hdr[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) { CloseAudio(); return false; }
        }
        return true;
    }
};

// ---- helpers: media types ----------------------------------------------------------------------------------------
static IMFMediaType* VideoType(const GUID& sub, int w, int h, int fps, int bitrate)
{
    IMFMediaType* t = nullptr;
    if (FAILED(MFCreateMediaType(&t))) return nullptr;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, sub);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(t, MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(t, MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (bitrate) { t->SetUINT32(MF_MT_AVG_BITRATE, bitrate); if (sub == MFVideoFormat_H264) t->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main); }
    return t;
}

static IMFMediaType* AudioType(const GUID& sub, int bytesPerSec, bool compressed)
{
    IMFMediaType* t = nullptr;
    if (FAILED(MFCreateMediaType(&t))) return nullptr;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, sub);
    t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioRate);
    t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
    t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytesPerSec);
    t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, compressed ? 1 : 2);
    if (compressed && sub == MFAudioFormat_AAC) {
        t->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        t->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    }
    return t;
}

// An MP3 output type for 44.1 kHz mono, the available bit rate closest to the wanted one. The type is taken from the list Windows gives for
// its MP3 encoder: a hand-made one lacks the MPEGLAYER3 fields (MF_MT_USER_DATA) and Windows 10 refuses it (MF_E_INVALIDMEDIATYPE, measured).
static IMFMediaType* Mp3Type(int wantBytesPerSec)
{
    IMFCollection* col = nullptr;
    if (FAILED(MFTranscodeGetAudioOutputAvailableTypes(MFAudioFormat_MP3, MFT_ENUM_FLAG_ALL, nullptr, &col)) || !col) return nullptr;
    DWORD n = 0; col->GetElementCount(&n);
    IMFMediaType* best = nullptr; int bestDiff = 0x7fffffff;
    for (DWORD i = 0; i < n; ++i) {
        IUnknown* u = nullptr;
        if (FAILED(col->GetElement(i, &u)) || !u) continue;
        IMFMediaType* t = nullptr;
        if (SUCCEEDED(u->QueryInterface(IID_PPV_ARGS(&t))) && t) {
            UINT32 rate = 0, ch = 0, bps = 0;
            t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate); t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch); t->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bps);
            int diff = (int)bps - wantBytesPerSec; if (diff < 0) diff = -diff;
            if (rate == (UINT32)kAudioRate && ch == 1 && diff < bestDiff) {
                if (best) best->Release();
                best = t; t->AddRef(); bestDiff = diff;
            }
            t->Release();
        }
        u->Release();
    }
    col->Release();
    return best;
}

// A Windows Media Audio output type for 44.1 kHz mono, the available bit rate closest to the wanted one.
static IMFMediaType* WmaType(int wantBytesPerSec)
{
    IMFCollection* col = nullptr;
    if (FAILED(MFTranscodeGetAudioOutputAvailableTypes(kAudioWma8, MFT_ENUM_FLAG_ALL, nullptr, &col)) || !col) return nullptr;
    DWORD n = 0; col->GetElementCount(&n);
    IMFMediaType* best = nullptr; int bestDiff = 0x7fffffff;
    for (DWORD i = 0; i < n; ++i) {
        IUnknown* u = nullptr;
        if (FAILED(col->GetElement(i, &u)) || !u) continue;
        IMFMediaType* t = nullptr;
        if (SUCCEEDED(u->QueryInterface(IID_PPV_ARGS(&t))) && t) {
            UINT32 rate = 0, ch = 0, bps = 0, bits = 16;
            t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate); t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
            t->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bps); t->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
            int diff = (int)bps - wantBytesPerSec; if (diff < 0) diff = -diff;
            if (rate == (UINT32)kAudioRate && ch == 1 && bits == 16 && diff < bestDiff) {
                if (best) best->Release();
                best = t; t->AddRef(); bestDiff = diff;
            }
            t->Release();
        }
        u->Release();
    }
    col->Release();
    return best;
}

// Whether this Windows can encode the audio of the given output type from the given input type. A stream cannot be taken out of
// a sink writer again, so the question is put to a throwaway writer (a small file beside the real one, same container).
static bool AudioProbe(const wchar_t* path, IMFMediaType* out, IMFMediaType* in)
{
    wchar_t probe[MAX_PATH + 16];
    wcsncpy(probe, path, MAX_PATH - 1); probe[MAX_PATH - 1] = 0;
    wchar_t* dot = wcsrchr(probe, L'.');
    const wchar_t* ext = dot ? dot : L".mp4";
    wchar_t head[MAX_PATH + 16]; wcsncpy(head, probe, MAX_PATH - 1); head[MAX_PATH - 1] = 0;
    if (dot) head[dot - probe] = 0;
    wchar_t full[MAX_PATH + 32]; _snwprintf(full, MAX_PATH + 32, L"%s-probe%s", head, ext);
    IMFSinkWriter* w = nullptr; DWORD idx = 0;
    bool ok = SUCCEEDED(MFCreateSinkWriterFromURL(full, nullptr, nullptr, &w)) && w &&
              SUCCEEDED(w->AddStream(out, &idx)) && SUCCEEDED(w->SetInputMediaType(idx, in, nullptr));
    if (w) w->Release();
    DeleteFileW(full);
    return ok;
}

// ---- Recorder --------------------------------------------------------------------------------------------------
Recorder::Recorder() : m(new Impl) {}
Recorder::~Recorder() { if (m->started && !m->finalized) Stop(); delete m; }

bool Recorder::Start(const RecParams& p, wchar_t* err, int errLen)
{
    Impl& d = *m;
    if (err && errLen) err[0] = 0;
    if (d.started) return false;
    d.p = p;
    int sw0 = p.rc.right - p.rc.left, sh0 = p.rc.bottom - p.rc.top;
    if (p.hwnd) { RECT r; if (WindowVisibleRect(p.hwnd, &r)) { sw0 = r.right - r.left; sh0 = r.bottom - r.top; d.p.rc = r; } }
    if (sw0 < 2 || sh0 < 2) { if (err) wcsncpy(err, T(S_ERR_REGION_SMALL), errLen - 1); return false; }
    int fmt = p.cfg.videoCodec < 0 || p.cfg.videoCodec >= VC_COUNT ? VC_H264_BEST : p.cfg.videoCodec;
    d.fmt = fmt;
    FitSizeForVideo(fmt, sw0, sh0, d.w, d.h);
    d.fps = p.cfg.fps < 5 ? 5 : p.cfg.fps > 60 ? 60 : p.cfg.fps;
    if (fmt == VC_GIF && d.fps > 15) d.fps = 15;       // a GIF is for slow pictures: 15 frames a second at most
    if (!d.grab.Init(d.w, d.h)) { if (err) wcsncpy(err, T(S_ERR_CAPTURE), errLen - 1); return false; }

    if (GetEnvironmentVariableW(L"C2S_NULLSINK", nullptr, 0) > 0) {
        d.nullSink = true;
        timeBeginPeriod(1); d.timePeriod = true;
        d.wall0 = Qpc100ns(d.freq);
        d.stopFlag = d.paused = d.failed = d.limit = 0;
        d.started = true;
        d.vThread = CreateThread(nullptr, 0, Impl::VThread, &d, 0, nullptr);
        return true;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    d.comInit = SUCCEEDED(hr);

    if (fmt == VC_AVI || fmt == VC_GIF) {             // our own sinks: no Media Foundation involved
        wchar_t why[160] = L"", warn[200] = L"";
        const bool wantAudio = fmt == VC_AVI && (p.cfg.mic || p.cfg.loopback);
        d.wantSpk = fmt == VC_AVI && p.cfg.loopback;
        if (fmt == VC_AVI && p.cfg.mic && !d.OpenAudio()) _snwprintf(warn, 200, L"%s", T(S_ERR_AUDIO));     // the microphone: a message when it is refused
        d.hasAudio = wantAudio;
        d.vStream = 0; d.aStream = 1;
        d.alt = fmt == VC_AVI ? CreateAviSink(p.path, d.w, d.h, d.fps, p.cfg.videoQuality, wantAudio, why, 160)
                              : CreateGifSink(p.path, d.w, d.h, d.fps, why, 160);
        if (!d.alt) {
            d.CloseAudio();
            if (err) _snwprintf(err, errLen, T(S_ERR_RECORDER), why[0] ? why : L"file");
            Log(L"recorder start failed (%s): %s", VideoFormatName(fmt), why);
            if (d.comInit) { CoUninitialize(); d.comInit = false; }
            return false;
        }
        timeBeginPeriod(1); d.timePeriod = true;
        d.wall0 = Qpc100ns(d.freq);
        d.stopFlag = d.paused = d.failed = d.limit = 0;
        d.started = true;
        if (d.hasAudio) { if (d.hwi) waveInStart(d.hwi); d.aThread = CreateThread(nullptr, 0, Impl::AThread, &d, 0, nullptr); }
        d.vThread = CreateThread(nullptr, 0, Impl::VThread, &d, 0, nullptr);
        if (err && warn[0]) { wcsncpy(err, warn, errLen - 1); err[errLen - 1] = 0; }
        return true;
    }

    hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) { if (err) _snwprintf(err, errLen, T(S_ERR_RECORDER), L"MFStartup"); Log(L"MFStartup failed 0x%08lX", (unsigned long)hr); return false; }
    d.mfStarted = true;

    auto fail = [&](const wchar_t* what, HRESULT code = S_OK) {
        wchar_t detail[100];
        if (code != S_OK) _snwprintf(detail, 100, L"%s (0x%08lX)", what, (unsigned long)code);
        else _snwprintf(detail, 100, L"%s", what);
        if (err) _snwprintf(err, errLen, T(S_ERR_RECORDER), detail);
        Log(L"recorder start failed: %s", detail);
        if (d.sw) { d.sw->Release(); d.sw = nullptr; }
        DeleteFileW(p.path);
        if (d.mfStarted) { MFShutdown(); d.mfStarted = false; }
        if (d.comInit) { CoUninitialize(); d.comInit = false; }
        return false;
    };

    // Encoder settings: speed/quality trade-off and variable bit rate.
    IMFAttributes* attrs = nullptr; IMFAttributes* enc = nullptr;
    MFCreateAttributes(&attrs, 4);
    MFCreateAttributes(&enc, 3);
    if (attrs) {
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        if (enc) {
            enc->SetUINT32(kQualityVsSpeed, fmt == VC_H264_FAST ? 25 : 85);
            enc->SetUINT32(kRateControlMode, eAVEncCommonRateControlMode_UnconstrainedVBR);
            if (fmt == VC_H264_BEST || fmt == VC_H264_FAST || fmt == VC_H265)
                { enc->SetUINT32(kMaxKeyDist, (UINT32)(d.fps * 20)); enc->SetUINT32(kGopSize, (UINT32)(d.fps * 20)); }       // a key frame every 20 s (the original: every 20-24 s): they are most of an H.264 file on a still screen
            attrs->SetUnknown(MF_SINK_WRITER_ENCODER_CONFIG, enc);
        }
    }
    hr = MFCreateSinkWriterFromURL(p.path, nullptr, attrs, &d.sw);
    if (enc) enc->Release();
    if (attrs) attrs->Release();
    if (FAILED(hr)) return fail(L"MFCreateSinkWriterFromURL", hr);

    // Video: bitrate from the size table, scaled by the frame rate and the quality choice.
    double qk = p.cfg.videoQuality == 0 ? 1.5 : p.cfg.videoQuality == 1 ? 1.0 : 0.6;
    double fk = d.fps / 25.0; if (fk < 0.5) fk = 0.5; if (fk > 2.0) fk = 2.0;
    if (fmt == VC_H264_BEST || fmt == VC_H264_FAST) qk *= 0.6;   // recordings of a screen are still sharp at well below the table rate
    else if (fmt == VC_H265) qk *= 0.4;                          // H.265 holds the same picture at about two thirds of the H.264 rate
    int bitrate = (int)(AutoBitrate(d.w, d.h) * qk * fk);
    if (bitrate < 200000) bitrate = 200000;
    if (bitrate > 20000000) bitrate = 20000000;
    const GUID& vsub = fmt == VC_H265 ? kVideoHevc : fmt == VC_WMV ? kVideoWmv3 : MFVideoFormat_H264;
    IMFMediaType* vo = VideoType(vsub, d.w, d.h, d.fps, bitrate);
    IMFMediaType* vi = VideoType(MFVideoFormat_RGB32, d.w, d.h, d.fps, 0);
    if (!vo || !vi) { if (vo) vo->Release(); if (vi) vi->Release(); return fail(L"media type"); }
    // Our pictures are bottom-up DIBs. The H.264 / H.265 path takes that for the default of an RGB32 input, the WMV encoder takes a top-down
    // picture when the stride is not told (measured: the file is upside down for another decoder): a negative stride says "bottom-up".
    if (fmt == VC_WMV) vi->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)(LONG)(-(LONG)d.w * 4));
    hr = d.sw->AddStream(vo, &d.vStream);
    if (SUCCEEDED(hr)) hr = d.sw->SetInputMediaType(d.vStream, vi, nullptr);
    vo->Release();
    if (SUCCEEDED(hr) && (fmt == VC_H264_BEST || fmt == VC_H264_FAST || fmt == VC_H265)) {
        // The key frame interval, straight on the encoder: the sink writer's config attributes are ignored (measured). The software H.264 encoder of
        // Windows 11 takes this value and reads it back but uses it only after the types are negotiated again (see below; the encoder of
        // Windows 10 and, presumably, the hardware ones take it at once).
        ICodecAPI* api = nullptr;
        HRESULT hrc = d.sw->GetServiceForStream(d.vStream, GUID_NULL, kIidCodecApi, (void**)&api);
        if (SUCCEEDED(hrc) && api) {
            VARIANT v; VariantInit(&v); v.vt = VT_UI4; v.ulVal = (ULONG)(d.fps * 20);
            HRESULT hrM = api->SetValue(&kMaxKeyDist, &v);
            hrc = api->SetValue(&kGopSize, &v);
            Log(L"video encoder: max key frame distance set: 0x%08lX", (unsigned long)hrM);
            api->Release();
            // The software encoder of Windows 11 reads the interval back but applies it only when its types are set again (measured on the
            // encoder alone and through the sink writer: set after the types - a key frame every second, the types set again on the encoder -
            // every 20 s, the file 4-5 times smaller). The sink writer does nothing on its own SetInputMediaType again: the types go straight
            // to the encoder, the second transform of the stream (the first is the colour converter).
            IMFSinkWriterEx* wx = nullptr;
            if (SUCCEEDED(d.sw->QueryInterface(__uuidof(IMFSinkWriterEx), (void**)&wx)) && wx) {
                int redone = 0;
                for (DWORD ti = 0; ti < 6; ++ti) {
                    IMFTransform* t = nullptr; GUID cg = GUID_NULL;
                    if (FAILED(wx->GetTransformForStream(d.vStream, ti, &cg, &t)) || !t) break;
                    if (IsEqualGUID(cg, MFT_CATEGORY_VIDEO_ENCODER)) {
                        IMFMediaType* to = nullptr; IMFMediaType* ti2 = nullptr;
                        t->GetOutputCurrentType(0, &to); t->GetInputCurrentType(0, &ti2);
                        HRESULT r1 = to ? t->SetOutputType(0, to, 0) : E_FAIL, r2 = ti2 ? t->SetInputType(0, ti2, 0) : E_FAIL;
                        Log(L"video encoder: types set again on the encoder: 0x%08lX / 0x%08lX", (unsigned long)r1, (unsigned long)r2);
                        if (to) to->Release();
                        if (ti2) ti2->Release();
                        ++redone;
                    }
                    t->Release();
                }
                if (!redone) Log(L"video encoder: the sink writer shows no encoder transform");
                wx->Release();
            }
        }
        Log(L"video encoder: key frame interval %d frames, set directly: 0x%08lX", d.fps * 20, (unsigned long)hrc);
    }
    if (SUCCEEDED(hr) && fmt == VC_WMV) {
        // The WMV encoder ignores the quality setting of the sink writer's config but takes it through ICodecAPI; its default (a high
        // complexity) costs 3-4 times the processor of 25 (measured, 1080p: 370-460 ms of CPU a frame against 120 ms, the file not bigger).
        ICodecAPI* api = nullptr;
        HRESULT hrc = d.sw->GetServiceForStream(d.vStream, GUID_NULL, kIidCodecApi, (void**)&api);
        if (SUCCEEDED(hrc) && api) {
            VARIANT v; VariantInit(&v); v.vt = VT_UI4; v.ulVal = 25;
            hrc = api->SetValue(&kQualityVsSpeed, &v);
            api->Release();
        }
        Log(L"video encoder: WMV quality vs speed 25 set: 0x%08lX", (unsigned long)hrc);
    }
    vi->Release();
    if (FAILED(hr)) return fail(fmt == VC_H265 ? L"H.265" : fmt == VC_WMV ? L"WMV" : L"H.264", hr);

    // Audio (optional): AAC or MP3 from a 16-bit mono microphone stream.
    wchar_t warn[200] = L"";
    if (p.cfg.mic || p.cfg.loopback) {
        bool ok = false;
        d.wantSpk = p.cfg.loopback;
        static const int aacBps[3] = { 24000, 16000, 12000 };
        static const int mp3Bps[3] = { 16000, 12000, 8000 };
        int q = p.cfg.audioQuality < 0 ? 0 : p.cfg.audioQuality > 2 ? 2 : p.cfg.audioQuality;
        if (fmt == VC_WMV) {                                         // Windows Media container: Windows Media Audio
            IMFMediaType* ao = WmaType(aacBps[q]);
            IMFMediaType* ai = AudioType(MFAudioFormat_PCM, kAudioRate * 2, false);
            if (ao && ai && SUCCEEDED(d.sw->AddStream(ao, &d.aStream)) && SUCCEEDED(d.sw->SetInputMediaType(d.aStream, ai, nullptr))) ok = true;
            if (ao) ao->Release();
            if (ai) ai->Release();
        }
        HRESULT hrAudio = S_OK;
        const bool mp3 = p.cfg.audioCodec == 1;                          // the codec chosen is the codec recorded, or nothing: no silent switch to the other one
        for (int attempt = 0; attempt < 1 && !ok && fmt != VC_WMV; ++attempt) {
            IMFMediaType* ao = mp3 ? Mp3Type(mp3Bps[q]) : AudioType(MFAudioFormat_AAC, aacBps[q], true);
            IMFMediaType* ai = AudioType(MFAudioFormat_PCM, kAudioRate * 2, false);
            HRESULT hrA = E_FAIL, hrI = E_FAIL;
            if (ao && ai && !AudioProbe(p.path, ao, ai)) { Log(L"recorder: this Windows cannot encode %s sound here", mp3 ? L"MP3" : L"AAC"); hrI = MF_E_INVALIDMEDIATYPE; }
            else if (ao && ai) {
                hrA = d.sw->AddStream(ao, &d.aStream);
                if (SUCCEEDED(hrA)) hrI = d.sw->SetInputMediaType(d.aStream, ai, nullptr);
                if (SUCCEEDED(hrA) && SUCCEEDED(hrI)) ok = true;
            }
            Log(L"recorder: audio %s: AddStream 0x%08lX (stream %lu), SetInputMediaType 0x%08lX, video stream %lu", mp3 ? L"MP3" : L"AAC", (unsigned long)hrA, (unsigned long)d.aStream, (unsigned long)hrI, (unsigned long)d.vStream);
            if (ao) ao->Release();
            if (ai) ai->Release();
            hrAudio = FAILED(hrA) ? hrA : hrI;
        }
        if (!ok) {                                                           // the sound asked for cannot be recorded: an error, not a silent file
            d.CloseAudio();
            return fail(fmt == VC_WMV ? L"WMA audio encoder" : mp3 ? L"MP3 audio encoder" : L"AAC audio encoder", hrAudio ? hrAudio : MF_E_INVALIDMEDIATYPE);
        }
        bool micOk = !p.cfg.mic || (ok && d.OpenAudio());                  // the microphone: a message when it is refused
        if (ok) d.hasAudio = true;                                          // the stream exists; the mix is silence where a source gives nothing
        if (!micOk) _snwprintf(warn, 200, L"%s", T(S_ERR_AUDIO));
    }

    hr = d.sw->BeginWriting();
    if (FAILED(hr)) { d.CloseAudio(); return fail(L"BeginWriting", hr); }

    timeBeginPeriod(1); d.timePeriod = true;
    d.wall0 = Qpc100ns(d.freq);
    d.stopFlag = d.paused = d.failed = d.limit = 0;
    d.started = true;
    if (d.hasAudio) {
        if (d.hwi) waveInStart(d.hwi);
        d.aThread = CreateThread(nullptr, 0, Impl::AThread, &d, 0, nullptr);
    }
    d.vThread = CreateThread(nullptr, 0, Impl::VThread, &d, 0, nullptr);
    if (err && warn[0]) { wcsncpy(err, warn, errLen - 1); err[errLen - 1] = 0; }
    return true;
}

void Recorder::SetPaused(bool pause)
{
    Impl& d = *m;
    if (!d.started) return;
    EnterCriticalSection(&d.cs);
    LONGLONG now = Qpc100ns(d.freq) - d.wall0;
    if (pause && !d.paused) { d.pauseStart = now; d.paused = 1; }
    else if (!pause && d.paused) { d.pausedTotal += now - d.pauseStart; d.paused = 0; }
    LeaveCriticalSection(&d.cs);
}

bool Recorder::Paused() const { return m->paused != 0; }
bool Recorder::Active() const { return m->started && !m->finalized; }
bool Recorder::Failed() const { return m->failed != 0; }
bool Recorder::LimitReached() const { return m->limit != 0; }
uint64_t Recorder::FramesWritten() const { return (uint64_t)m->vFrames; }
int  Recorder::Width() const { return m->w; }
int  Recorder::Height() const { return m->h; }

double Recorder::ElapsedSec() const
{
    Impl& d = *m;
    if (!d.started) return 0.0;
    if (d.finalized) return (double)d.mediaNow / 1e7;
    return (double)d.MediaTime() / 1e7;
}

uint64_t Recorder::FileBytes() const
{
    HANDLE h = CreateFileW(m->p.path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz; sz.QuadPart = 0;
    GetFileSizeEx(h, &sz);
    CloseHandle(h);
    return (uint64_t)sz.QuadPart;
}

bool Recorder::Stop()
{
    Impl& d = *m;
    if (!d.started || d.finalized) return false;
    d.mediaNow = d.MediaTime();
    d.stopFlag = 1;
    if (d.aEvent) SetEvent(d.aEvent);
    if (d.vThread) { WaitForSingleObject(d.vThread, 10000); CloseHandle(d.vThread); d.vThread = nullptr; }
    if (d.aThread) { WaitForSingleObject(d.aThread, 10000); CloseHandle(d.aThread); d.aThread = nullptr; }
    d.CloseAudio();
    bool ok = !d.failed;
    if (d.alt) { ok = d.alt->Close(ok) && ok; delete d.alt; d.alt = nullptr; }
    if (d.sw) {
        ULONGLONG before = FileBytes();
        DWORD t0 = GetTickCount();
        HRESULT hr = d.sw->Finalize();
        Log(L"recorder finalize: 0x%08lX, %lu ms, file %llu -> %llu bytes, worst encoder lag %lld frames (%d fps)", (unsigned long)hr, (unsigned long)(GetTickCount() - t0), (unsigned long long)before, (unsigned long long)FileBytes(), (long long)d.maxLag, d.fps);
        ok = ok && SUCCEEDED(hr);
        d.sw->Release(); d.sw = nullptr;
    }
    if (d.timePeriod) { timeEndPeriod(1); d.timePeriod = false; }
    if (d.mfStarted) { MFShutdown(); d.mfStarted = false; }
    if (d.comInit) { CoUninitialize(); d.comInit = false; }
    d.finalized = true;
    if (!ok) DeleteFileW(d.p.path);
    return ok;
}

// ---- the video formats -----------------------------------------------------------------------------------------------
const wchar_t* VideoFormatName(int vc)
{
    switch (vc) {
    case VC_H264_BEST: return T(S_VC_BEST);
    case VC_H264_FAST: return T(S_VC_FAST);
    case VC_H265: return L"H.265 / HEVC (MP4)";
    case VC_WMV: return L"WMV (Windows Media)";
    case VC_AVI: return L"AVI (Motion-JPEG)";
    case VC_GIF: return L"GIF (animated)";
    }
    return T(S_VC_BEST);
}

const wchar_t* VideoFormatExt(int vc)
{
    switch (vc) {
    case VC_WMV: return L"wmv";
    case VC_AVI: return L"avi";
    case VC_GIF: return L"gif";
    }
    return L"mp4";
}

// Is there a video encoder MFT for this output format (HEVC needs the Microsoft extension or a hardware encoder).
static bool HasVideoEncoder(const GUID& sub)
{
    HRESULT hrc = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    bool found = false;
    if (started) {
        MFT_REGISTER_TYPE_INFO out = { MFMediaType_Video, sub };
        IMFActivate** act = nullptr; UINT32 n = 0;
        if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                                nullptr, &out, &act, &n)) && act) {
            found = n > 0;
            for (UINT32 i = 0; i < n; ++i) act[i]->Release();
            CoTaskMemFree(act);
        }
        MFShutdown();
    }
    if (SUCCEEDED(hrc)) CoUninitialize();
    return found;
}

// Does the encoder really encode? Listing an HEVC encoder is not enough: on a machine whose encoder is a hardware one without the
// hardware it fails at the first frame. A few frames of a small picture go through a throwaway writer into a temporary file.
static bool EncoderWorks(const GUID& sub)
{
    HRESULT hrc = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
        wchar_t tmp[MAX_PATH], path[MAX_PATH + 40];
        GetTempPathW(MAX_PATH, tmp);
        _snwprintf(path, MAX_PATH + 40, L"%sc2s-probe-%lu.mp4", tmp, (unsigned long)GetCurrentProcessId());
        IMFAttributes* attrs = nullptr; IMFSinkWriter* w = nullptr;
        MFCreateAttributes(&attrs, 2);
        if (attrs) { attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE); attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE); }
        const int W = 320, H = 240, FPS = 25;
        IMFMediaType* vo = VideoType(sub, W, H, FPS, 800000);
        IMFMediaType* vi = VideoType(MFVideoFormat_RGB32, W, H, FPS, 0);
        DWORD st = 0;
        if (vo && vi && SUCCEEDED(MFCreateSinkWriterFromURL(path, nullptr, attrs, &w)) && w &&
            SUCCEEDED(w->AddStream(vo, &st)) && SUCCEEDED(w->SetInputMediaType(st, vi, nullptr)) && SUCCEEDED(w->BeginWriting())) {
            ok = true;
            for (int i = 0; i < 12 && ok; ++i) {
                IMFMediaBuffer* b = nullptr; IMFSample* smp = nullptr; BYTE* d = nullptr;
                const DWORD bytes = W * H * 4;
                if (FAILED(MFCreateMemoryBuffer(bytes, &b)) || FAILED(b->Lock(&d, nullptr, nullptr))) { ok = false; if (b) b->Release(); break; }
                for (DWORD k = 0; k < bytes; k += 4) { d[k] = (BYTE)(i * 20); d[k + 1] = (BYTE)(k >> 8); d[k + 2] = (BYTE)(255 - i * 10); d[k + 3] = 255; }
                b->Unlock(); b->SetCurrentLength(bytes);
                if (FAILED(MFCreateSample(&smp))) ok = false;
                else {
                    smp->AddBuffer(b); smp->SetSampleTime((LONGLONG)i * 10000000LL / FPS); smp->SetSampleDuration(10000000LL / FPS);
                    ok = SUCCEEDED(w->WriteSample(st, smp));
                    smp->Release();
                }
                b->Release();
            }
            if (ok) ok = SUCCEEDED(w->Finalize());
        }
        if (vo) vo->Release();
        if (vi) vi->Release();
        if (w) w->Release();
        if (attrs) attrs->Release();
        DeleteFileW(path);
        MFShutdown();
    }
    if (SUCCEEDED(hrc)) CoUninitialize();
    Log(L"video format probe: the encoder %s", ok ? L"works" : L"does not work");
    return ok;
}

// The HEVC probe encodes a few frames, which takes seconds on a slow machine. Its answer is kept in the registry beside the number
// of the Windows build it was made on: the options window reads it at once, the probe at start-up (RefreshFormatProbes) renews it.
static DWORD WindowsBuild()
{
    typedef LONG(WINAPI* RtlGetVersionFn)(OSVERSIONINFOW*);
    RtlGetVersionFn fn = (RtlGetVersionFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW v; memset(&v, 0, sizeof(v)); v.dwOSVersionInfoSize = sizeof(v);
    return fn && fn(&v) == 0 ? v.dwBuildNumber : 0;
}
static void ProbeKey(wchar_t* out, int n) { _snwprintf(out, n, L"%s\\Probe", SettingsKeyName()); out[n - 1] = 0; }
static int LoadHevcProbe()                                           // 0 nothing stored for this build, 1 works, 2 does not
{
    HKEY k; DWORD val = 0, build = 0, n = 4, n2 = 4, t = 0;
    int r = 0; wchar_t key[200]; ProbeKey(key, 200);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, L"Hevc", nullptr, &t, (BYTE*)&val, &n) == ERROR_SUCCESS && RegQueryValueExW(k, L"Build", nullptr, &t, (BYTE*)&build, &n2) == ERROR_SUCCESS && build == WindowsBuild() && build)
            r = val ? 1 : 2;
        RegCloseKey(k);
    }
    return r;
}
static void SaveHevcProbe(bool ok)
{
    HKEY k; wchar_t key[200]; ProbeKey(key, 200);
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        DWORD v = ok ? 1 : 0, b = WindowsBuild();
        RegSetValueExW(k, L"Hevc", 0, REG_DWORD, (const BYTE*)&v, 4);
        RegSetValueExW(k, L"Build", 0, REG_DWORD, (const BYTE*)&b, 4);
        RegCloseKey(k);
    }
}
static volatile LONG g_hevcState = 0;                                // 0 unknown, 1 works, 2 does not (read without a lock)

// Start-up, from a thread of its own: probe again whatever the registry said, and keep the answer for the next start.
void RefreshFormatProbes()
{
    if (!HasVideoEncoder(kVideoHevc)) { g_hevcState = 2; SaveHevcProbe(false); return; }
    bool ok = EncoderWorks(kVideoHevc);
    g_hevcState = ok ? 1 : 2;
    SaveHevcProbe(ok);
}

bool VideoFormatAvailable(int vc)
{
    if (vc == VC_H265) {
        if (g_hevcState == 0) { int r = LoadHevcProbe(); if (r) InterlockedCompareExchange(&g_hevcState, r, 0); }
        if (g_hevcState) return g_hevcState == 1;                      // never waits for a probe that is running
    }
    static int cache[VC_COUNT];                                      // 0 unknown, 1 yes, 2 no
    static SRWLOCK lock = SRWLOCK_INIT;                               // the program asks from a thread of its own at start-up (the HEVC probe takes a moment)
    if (vc < 0 || vc >= VC_COUNT) return false;
    AcquireSRWLockExclusive(&lock);
    struct Unlock { SRWLOCK* l; ~Unlock() { ReleaseSRWLockExclusive(l); } } unlock = { &lock };
    if (cache[vc] == 0) {
        bool ok = true;
        switch (vc) {
        case VC_H264_BEST: ok = true; break;                          // always offered (the encoder is part of Windows)
        case VC_H264_FAST: ok = false; break;                         // not offered any more: one H.264 profile is enough (old settings are moved to the first)
        case VC_H265: ok = HasVideoEncoder(kVideoHevc) && EncoderWorks(kVideoHevc); g_hevcState = ok ? 1 : 2; SaveHevcProbe(ok); break;
        case VC_WMV: ok = HasVideoEncoder(kVideoWmv3); break;
        case VC_AVI: ok = ImageFormatAvailable(IMG_JPG); break;
        case VC_GIF: ok = ImageFormatAvailable(IMG_GIF); break;
        }
        cache[vc] = ok ? 1 : 2;
    }
    return cache[vc] == 1;
}
