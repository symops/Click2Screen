// Video recorder: GDI frames + microphone -> Media Foundation sink writer -> MP4 (H.264 video, AAC or MP3 audio).
// Two worker threads (video frames on a media clock that stops during pauses; microphone buffers of 50 ms) write into
// one IMFSinkWriter. Everything the encoder needs ships with Windows, so no codec packs are required.
#include "c2s.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <mmsystem.h>
#include <wchar.h>
#include <stdio.h>

namespace {

// CODECAPI_AVEncCommonQualityVsSpeed / ...RateControlMode (codecapi.h), spelled out.
const GUID kQualityVsSpeed  = { 0x98332df8, 0x03cd, 0x476b, { 0x89, 0xfa, 0x3f, 0x9e, 0x44, 0x2d, 0xec, 0x9f } };
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
                    ok = SUCCEEDED(sw->WriteSample(stream, s));
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
            RECT src = p.rc;
            if (p.hwnd) {
                if (!IsWindow(p.hwnd)) { limit = 1; break; }       // the recorded window was closed
                if (IsIconic(p.hwnd)) { Sleep(20); continue; }
                RECT r;
                if (WindowVisibleRect(p.hwnd, &r) && r.right - r.left > 1 && r.bottom - r.top > 1) src = r;
            }
            if (!grab.Grab(src, p.cfg.capMouse, p.cfg.highlight, true)) { Sleep(5); continue; }
            LONGLONG ts = mt;
            if (ts <= lastVideoTs) ts = lastVideoTs + 1;
            lastVideoTs = ts;
            if (!WriteSample(vStream, grab.Bits(), (DWORD)bytes, ts, interval)) { failed = 1; break; }
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
    MFCreateAttributes(&enc, 2);
    if (attrs) {
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        if (enc) {
            enc->SetUINT32(kQualityVsSpeed, fmt == VC_H264_FAST ? 25 : 85);
            enc->SetUINT32(kRateControlMode, eAVEncCommonRateControlMode_UnconstrainedVBR);
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
    int bitrate = (int)(AutoBitrate(d.w, d.h) * qk * fk);
    if (bitrate < 200000) bitrate = 200000;
    if (bitrate > 20000000) bitrate = 20000000;
    const GUID& vsub = fmt == VC_H265 ? kVideoHevc : fmt == VC_WMV ? kVideoWmv3 : MFVideoFormat_H264;
    IMFMediaType* vo = VideoType(vsub, d.w, d.h, d.fps, bitrate);
    IMFMediaType* vi = VideoType(MFVideoFormat_RGB32, d.w, d.h, d.fps, 0);
    if (!vo || !vi) { if (vo) vo->Release(); if (vi) vi->Release(); return fail(L"media type"); }
    hr = d.sw->AddStream(vo, &d.vStream);
    if (SUCCEEDED(hr)) hr = d.sw->SetInputMediaType(d.vStream, vi, nullptr);
    vo->Release(); vi->Release();
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
        for (int attempt = 0; attempt < 2 && !ok && fmt != VC_WMV; ++attempt) {
            bool mp3 = attempt == 0 ? p.cfg.audioCodec == 1 : p.cfg.audioCodec != 1;   // second try: the other codec
            IMFMediaType* ao = AudioType(mp3 ? MFAudioFormat_MP3 : MFAudioFormat_AAC, mp3 ? mp3Bps[q] : aacBps[q], true);
            IMFMediaType* ai = AudioType(MFAudioFormat_PCM, kAudioRate * 2, false);
            if (ao && ai && SUCCEEDED(d.sw->AddStream(ao, &d.aStream)) && SUCCEEDED(d.sw->SetInputMediaType(d.aStream, ai, nullptr)))
                ok = true;
            if (ao) ao->Release();
            if (ai) ai->Release();
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
        HRESULT hr = d.sw->Finalize();
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

bool VideoFormatAvailable(int vc)
{
    static int cache[VC_COUNT];                                      // 0 unknown, 1 yes, 2 no
    if (vc < 0 || vc >= VC_COUNT) return false;
    if (cache[vc] == 0) {
        bool ok = true;
        switch (vc) {
        case VC_H264_BEST: case VC_H264_FAST: ok = true; break;      // always offered (the encoder is part of Windows)
        case VC_H265: ok = HasVideoEncoder(kVideoHevc); break;
        case VC_WMV: ok = HasVideoEncoder(kVideoWmv3); break;
        case VC_AVI: ok = ImageFormatAvailable(IMG_JPG); break;
        case VC_GIF: ok = ImageFormatAvailable(IMG_GIF); break;
        }
        cache[vc] = ok ? 1 : 2;
    }
    return cache[vc] == 1;
}
