// Sound for the recordings: what goes to the speakers (WASAPI loopback of the default playback device) and the helpers that
// bring it to the format of the recorder (mono, 16 bit, 44.1 kHz) and mix it with the microphone.
#include "c2s.h"
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mfapi.h>
#include <mftransform.h>
#include <mferror.h>
#include <stdlib.h>
#include <math.h>

// The identifiers, spelled out (not every header set has them).
static const GUID kClsidMMDeviceEnumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID kIidMMDeviceEnumerator   = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID kIidAudioClient          = { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID kIidAudioCaptureClient   = { 0xC8ADBD64, 0xE71E, 0x48A0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };
static const GUID kSubtypePcm              = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const GUID kSubtypeIeeeFloat        = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

// Mono 44.1 kHz 16 bit from `frames` frames of float samples (interleaved, `channels` of them) at `rate`: the channels are
// averaged, the rate is changed by linear interpolation. The stream goes on between calls: `phase` and `prev` carry over.
int AudioToMono44k(const float* in, int frames, int channels, int rate, double* phase, float* prev, int16_t* out, int cap)
{
    if (frames <= 0 || channels <= 0 || rate <= 0) return 0;
    const double step = (double)rate / 44100.0;
    double pos = *phase;                                   // position in t[]: t[0] is the last sample of the previous block
    int n = 0;
    float p0 = *prev;
    float last = *prev;
    auto mono = [&](int k) -> float {                      // t[k] for k >= 1 is the k-th frame of this block
        if (k == 0) return p0;
        const float* f = in + (size_t)(k - 1) * channels;
        float s = 0; for (int c = 0; c < channels; ++c) s += f[c];
        return s / channels;
    };
    while (pos < frames && n < cap) {
        int i = (int)pos; double fr = pos - i;
        float v = (float)(mono(i) * (1.0 - fr) + mono(i + 1) * fr);
        if (v > 1.0f) v = 1.0f; if (v < -1.0f) v = -1.0f;
        out[n++] = (int16_t)(v * 32767.0f);
        pos += step;
    }
    last = mono(frames);
    *phase = pos - frames;
    if (*phase < 0) *phase = 0;
    *prev = last;
    return n;
}

// ---- the Windows audio resampler -----------------------------------------------------------------------------------------
static const GUID kClsidResampler = { 0xf447b69e, 0x1884, 0x4a7e, { 0x80, 0x55, 0x34, 0x6f, 0x74, 0xd6, 0xed, 0xb3 } };   // CLSID_CResamplerMediaObject
static const GUID kIidTransform   = { 0xbf94c121, 0x5b05, 0x4e6f, { 0x80, 0x00, 0xba, 0x59, 0x89, 0x61, 0x41, 0x4d } };   // IID_IMFTransform

struct AudioResampler::Impl {
    IMFTransform* mft = nullptr;
    bool started = false;
};

AudioResampler::AudioResampler() : m(new Impl) {}
AudioResampler::~AudioResampler()
{
    if (m->mft) { m->mft->Release(); m->mft = nullptr; }
    if (m->started) MFShutdown();
    delete m;
}

static IMFMediaType* FloatMonoType(int rate)
{
    IMFMediaType* t = nullptr;
    if (FAILED(MFCreateMediaType(&t))) return nullptr;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
    t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
    t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * 4);
    t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    return t;
}

bool AudioResampler::Init(int rate)
{
    if (m->mft || rate < 8000) return false;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;
    m->started = true;
    if (FAILED(CoCreateInstance(kClsidResampler, nullptr, CLSCTX_INPROC_SERVER, kIidTransform, (void**)&m->mft)) || !m->mft) return false;
    IMFMediaType* in = FloatMonoType(rate); IMFMediaType* out = FloatMonoType(44100);
    bool ok = in && out && SUCCEEDED(m->mft->SetInputType(0, in, 0)) && SUCCEEDED(m->mft->SetOutputType(0, out, 0));
    if (in) in->Release();
    if (out) out->Release();
    if (!ok) { m->mft->Release(); m->mft = nullptr; return false; }
    m->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

int AudioResampler::Process(const float* mono, int frames, int16_t* out, int cap)
{
    if (!m->mft || frames <= 0 || cap <= 0) return 0;
    int n = 0;
    auto drain = [&]() {
        while (n < cap) {
            const DWORD bytes = (DWORD)(cap - n) * 4;
            IMFMediaBuffer* b = nullptr; IMFSample* s = nullptr;
            if (FAILED(MFCreateMemoryBuffer(bytes, &b))) return;
            if (FAILED(MFCreateSample(&s))) { b->Release(); return; }
            s->AddBuffer(b);
            MFT_OUTPUT_DATA_BUFFER ob; memset(&ob, 0, sizeof(ob)); ob.pSample = s;
            DWORD status = 0;
            HRESULT hr = m->mft->ProcessOutput(0, 1, &ob, &status);
            if (SUCCEEDED(hr)) {
                BYTE* d = nullptr; DWORD len = 0;
                if (SUCCEEDED(b->Lock(&d, nullptr, &len))) {
                    const float* f = (const float*)d;
                    for (DWORD i = 0; i < len / 4 && n < cap; ++i) {
                        float v = f[i]; if (v > 1.0f) v = 1.0f; if (v < -1.0f) v = -1.0f;
                        out[n++] = (int16_t)(v * 32767.0f);
                    }
                    b->Unlock();
                }
            }
            if (ob.pEvents) ob.pEvents->Release();
            s->Release(); b->Release();
            if (FAILED(hr)) return;                         // MF_E_TRANSFORM_NEED_MORE_INPUT: this block is done
        }
    };
    IMFMediaBuffer* b = nullptr; IMFSample* s = nullptr;
    if (FAILED(MFCreateMemoryBuffer((DWORD)frames * 4, &b))) return 0;
    BYTE* d = nullptr;
    if (SUCCEEDED(b->Lock(&d, nullptr, nullptr))) { memcpy(d, mono, (size_t)frames * 4); b->Unlock(); b->SetCurrentLength((DWORD)frames * 4); }
    if (SUCCEEDED(MFCreateSample(&s))) {
        s->AddBuffer(b);
        HRESULT hr = m->mft->ProcessInput(0, s, 0);
        if (hr == MF_E_NOTACCEPTING) { drain(); hr = m->mft->ProcessInput(0, s, 0); }
        if (SUCCEEDED(hr)) drain();
        s->Release();
    }
    b->Release();
    return n;
}

void AudioMix(const int16_t* a, const int16_t* b, int n, int16_t* out)
{
    for (int i = 0; i < n; ++i) {
        int v = (int)a[i] + (int)b[i];
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

// ---- a first-in first-out queue of samples --------------------------------------------------------------------------
Fifo16::Fifo16() : m_buf(nullptr), m_cap(0), m_head(0), m_tail(0) {}
Fifo16::~Fifo16() { free(m_buf); }
int Fifo16::Size() const { return (int)(m_tail - m_head); }
void Fifo16::Clear() { m_head = m_tail = 0; }
void Fifo16::Push(const int16_t* s, int n)
{
    if (n <= 0) return;
    if (m_tail + n > m_cap) {
        if (m_head > 0) { memmove(m_buf, m_buf + m_head, (m_tail - m_head) * sizeof(int16_t)); m_tail -= m_head; m_head = 0; }
        if (m_tail + n > m_cap) {
            size_t cap = m_cap ? m_cap : 65536;
            while (cap < m_tail + n) cap *= 2;
            int16_t* b = (int16_t*)realloc(m_buf, cap * sizeof(int16_t));
            if (!b) return;
            m_buf = b; m_cap = cap;
        }
    }
    memcpy(m_buf + m_tail, s, n * sizeof(int16_t));
    m_tail += n;
}
void Fifo16::Pop(int16_t* out, int n)                      // missing samples are silence
{
    int have = Size() < n ? Size() : n;
    if (have > 0) memcpy(out, m_buf + m_head, have * sizeof(int16_t));
    for (int i = have; i < n; ++i) out[i] = 0;
    m_head += have;
    if (m_head == m_tail) m_head = m_tail = 0;
}
void Fifo16::TrimTo(int keep)                              // what is older than the last `keep` samples is dropped
{
    if (Size() > keep) m_head = m_tail - keep;
}

// ---- the speakers ----------------------------------------------------------------------------------------------------
struct SpeakerCapture::Impl {
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* cap = nullptr;
    WAVEFORMATEX* fmt = nullptr;
    bool isFloat = false, isPcm16 = false;
    int channels = 0, rate = 0;
    double phase = 0; float prev = 0;
    float* tmp = nullptr; int tmpCap = 0;
    AudioResampler* rs = nullptr;        // the Windows resampler when the device does not run at 44.1 kHz and it is there
    float* mono = nullptr; int monoCap = 0;
};

SpeakerCapture::SpeakerCapture() : m(new Impl) {}
SpeakerCapture::~SpeakerCapture() { Close(); delete m; }
bool SpeakerCapture::IsOpen() const { return m->cap != nullptr; }

bool SpeakerCapture::Open()
{
    Impl& d = *m;
    if (FAILED(CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL, kIidMMDeviceEnumerator, (void**)&d.en)) || !d.en) return false;
    if (FAILED(d.en->GetDefaultAudioEndpoint(eRender, eConsole, &d.dev)) || !d.dev) { Close(); return false; }
    if (FAILED(d.dev->Activate(kIidAudioClient, CLSCTX_ALL, nullptr, (void**)&d.client)) || !d.client) { Close(); return false; }
    if (FAILED(d.client->GetMixFormat(&d.fmt)) || !d.fmt) { Close(); return false; }
    d.channels = d.fmt->nChannels; d.rate = (int)d.fmt->nSamplesPerSec;
    if (d.fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && d.fmt->wBitsPerSample == 32) d.isFloat = true;
    else if (d.fmt->wFormatTag == WAVE_FORMAT_PCM && d.fmt->wBitsPerSample == 16) d.isPcm16 = true;
    else if (d.fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && d.fmt->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE* x = (const WAVEFORMATEXTENSIBLE*)d.fmt;
        if (x->SubFormat == kSubtypeIeeeFloat && d.fmt->wBitsPerSample == 32) d.isFloat = true;
        else if (x->SubFormat == kSubtypePcm && d.fmt->wBitsPerSample == 16) d.isPcm16 = true;
    }
    if ((!d.isFloat && !d.isPcm16) || d.channels < 1 || d.rate < 8000) { Close(); return false; }
    // loopback: what the device plays is captured from the playback endpoint itself
    if (FAILED(d.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, d.fmt, nullptr))) { Close(); return false; }
    if (FAILED(d.client->GetService(kIidAudioCaptureClient, (void**)&d.cap)) || !d.cap) { Close(); return false; }
    if (FAILED(d.client->Start())) { Close(); return false; }
    if (d.rate != 44100) {
        d.rs = new AudioResampler;
        if (!d.rs->Init(d.rate)) { delete d.rs; d.rs = nullptr; }
    }
    return true;
}

void SpeakerCapture::Close()
{
    Impl& d = *m;
    if (d.client) d.client->Stop();
    if (d.cap) { d.cap->Release(); d.cap = nullptr; }
    if (d.client) { d.client->Release(); d.client = nullptr; }
    if (d.fmt) { CoTaskMemFree(d.fmt); d.fmt = nullptr; }
    if (d.dev) { d.dev->Release(); d.dev = nullptr; }
    if (d.en) { d.en->Release(); d.en = nullptr; }
    free(d.tmp); d.tmp = nullptr; d.tmpCap = 0;
    free(d.mono); d.mono = nullptr; d.monoCap = 0;
    delete d.rs; d.rs = nullptr;
}

// Everything the device has played since the last call, as mono 44.1 kHz 16 bit; at most `cap` samples. While nothing plays
// the device delivers no packets (or silent ones): the caller fills the time with silence.
int SpeakerCapture::Pull(int16_t* out, int cap)
{
    Impl& d = *m;
    if (!d.cap) return 0;
    int total = 0;
    UINT32 next = 0;
    while (total < cap - 4096 && SUCCEEDED(d.cap->GetNextPacketSize(&next)) && next > 0) {
        BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
        if (FAILED(d.cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
        if (frames > 0) {
            if (d.tmpCap < (int)frames * d.channels) {
                free(d.tmp); d.tmpCap = (int)frames * d.channels; d.tmp = (float*)malloc((size_t)d.tmpCap * sizeof(float));
            }
            if (d.tmp) {
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) memset(d.tmp, 0, (size_t)frames * d.channels * sizeof(float));
                else if (d.isFloat) memcpy(d.tmp, data, (size_t)frames * d.channels * sizeof(float));
                else { const int16_t* s = (const int16_t*)data; for (UINT32 i = 0; i < frames * d.channels; ++i) d.tmp[i] = s[i] / 32768.0f; }
                if (d.rs) {                                                 // the channels averaged, the rate changed by the Windows resampler
                    if (d.monoCap < (int)frames) { free(d.mono); d.monoCap = (int)frames; d.mono = (float*)malloc((size_t)d.monoCap * sizeof(float)); }
                    if (d.mono) {
                        for (UINT32 i = 0; i < frames; ++i) { float sum = 0; for (int c = 0; c < d.channels; ++c) sum += d.tmp[(size_t)i * d.channels + c]; d.mono[i] = sum / d.channels; }
                        total += d.rs->Process(d.mono, (int)frames, out + total, cap - total);
                    }
                } else
                    total += AudioToMono44k(d.tmp, (int)frames, d.channels, d.rate, &d.phase, &d.prev, out + total, cap - total);
            }
        }
        d.cap->ReleaseBuffer(frames);
    }
    return total;
}
