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
    LONGLONG limit100 = 0;
    LONGLONG lastVideoTs = -1;
    LONGLONG audioSamples = 0;
    HWAVEIN hwi = nullptr;
    WAVEHDR hdr[kAudioBufs];
    BYTE* abuf[kAudioBufs];
    HANDLE aEvent = nullptr;
    bool timePeriod = false;
    bool nullSink = false;               // self-test only (C2S_NULLSINK): frames are counted, nothing is encoded
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
            if (limit100 && mt >= limit100) { limit = 1; break; }
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
            if (!grab.Grab(src, p.cfg.capMouse, p.cfg.highlight)) { Sleep(5); continue; }
            LONGLONG ts = mt;
            if (ts <= lastVideoTs) ts = lastVideoTs + 1;
            lastVideoTs = ts;
            if (!WriteSample(vStream, grab.Bits(), (DWORD)bytes, ts, interval)) { failed = 1; break; }
            n = mt / interval + 1;        // a late frame does not cause a burst of catch-up frames
        }
    }

    void AudioLoop()
    {
        while (!stopFlag) {
            WaitForSingleObject(aEvent, 100);
            for (int i = 0; i < kAudioBufs; ++i) {
                WAVEHDR& hd = hdr[i];
                if (!(hd.dwFlags & WHDR_DONE)) continue;
                DWORD bytes = hd.dwBytesRecorded;
                if (bytes && !paused && !stopFlag) {
                    LONGLONG ts = audioSamples * 10000000LL / kAudioRate;
                    LONGLONG dur = (LONGLONG)(bytes / 2) * 10000000LL / kAudioRate;
                    if (!WriteSample(aStream, hd.lpData, bytes, ts, dur)) { failed = 1; return; }
                    audioSamples += bytes / 2;
                }
                hd.dwFlags &= ~WHDR_DONE;
                hd.dwBytesRecorded = 0;
                if (!stopFlag) waveInAddBuffer(hwi, &hd, sizeof(WAVEHDR));
            }
        }
    }

    static DWORD WINAPI VThread(LPVOID a) { ((Impl*)a)->VideoLoop(); return 0; }
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
    if (bitrate) { t->SetUINT32(MF_MT_AVG_BITRATE, bitrate); t->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main); }
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
    FitSize(sw0, sh0, d.w, d.h);
    d.fps = p.cfg.fps < 5 ? 5 : p.cfg.fps > 60 ? 60 : p.cfg.fps;
    if (!d.grab.Init(d.w, d.h)) { if (err) wcsncpy(err, T(S_ERR_CAPTURE), errLen - 1); return false; }
    int total = p.cfg.durH * 3600 + p.cfg.durM * 60 + p.cfg.durS;
    d.limit100 = (LONGLONG)total * 10000000LL;

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
            enc->SetUINT32(kQualityVsSpeed, p.cfg.videoCodec == 0 ? 85 : 25);
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
    IMFMediaType* vo = VideoType(MFVideoFormat_H264, d.w, d.h, d.fps, bitrate);
    IMFMediaType* vi = VideoType(MFVideoFormat_RGB32, d.w, d.h, d.fps, 0);
    if (!vo || !vi) { if (vo) vo->Release(); if (vi) vi->Release(); return fail(L"media type"); }
    hr = d.sw->AddStream(vo, &d.vStream);
    if (SUCCEEDED(hr)) hr = d.sw->SetInputMediaType(d.vStream, vi, nullptr);
    vo->Release(); vi->Release();
    if (FAILED(hr)) return fail(L"H.264", hr);

    // Audio (optional): AAC or MP3 from a 16-bit mono microphone stream.
    wchar_t warn[200] = L"";
    if (p.cfg.mic) {
        bool ok = false;
        static const int aacBps[3] = { 24000, 16000, 12000 };
        static const int mp3Bps[3] = { 16000, 12000, 8000 };
        int q = p.cfg.audioQuality < 0 ? 0 : p.cfg.audioQuality > 2 ? 2 : p.cfg.audioQuality;
        for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
            bool mp3 = attempt == 0 ? p.cfg.audioCodec == 1 : p.cfg.audioCodec != 1;   // second try: the other codec
            IMFMediaType* ao = AudioType(mp3 ? MFAudioFormat_MP3 : MFAudioFormat_AAC, mp3 ? mp3Bps[q] : aacBps[q], true);
            IMFMediaType* ai = AudioType(MFAudioFormat_PCM, kAudioRate * 2, false);
            if (ao && ai && SUCCEEDED(d.sw->AddStream(ao, &d.aStream)) && SUCCEEDED(d.sw->SetInputMediaType(d.aStream, ai, nullptr)))
                ok = true;
            if (ao) ao->Release();
            if (ai) ai->Release();
        }
        if (ok && d.OpenAudio()) d.hasAudio = true;
        else _snwprintf(warn, 200, L"%s", T(S_ERR_AUDIO));
        if (ok && !d.hasAudio) { /* the audio stream stays unused; the writer finalizes fine without samples */ }
    }

    hr = d.sw->BeginWriting();
    if (FAILED(hr)) { d.CloseAudio(); return fail(L"BeginWriting", hr); }

    timeBeginPeriod(1); d.timePeriod = true;
    d.wall0 = Qpc100ns(d.freq);
    d.stopFlag = d.paused = d.failed = d.limit = 0;
    d.started = true;
    if (d.hasAudio) {
        waveInStart(d.hwi);
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
