// Frame sinks that do not go through Media Foundation: AVI (Motion-JPEG video + PCM sound) and animated GIF. Both are
// written with WIC (the JPEG / GIF encoders that ship with Windows), so no codec packs are needed either.
#include "c2s.h"
#include <wincodec.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

namespace {

typedef unsigned char u8;

void Why(wchar_t* why, int len, const wchar_t* text, HRESULT hr = S_OK)
{
    if (!why || len <= 0) return;
    if (hr != S_OK) _snwprintf(why, len, L"%s (0x%08lX)", text, (unsigned long)hr);
    else _snwprintf(why, len, L"%s", text);
    why[len - 1] = 0;
}

// The captured DIB is bottom-up; WIC wants rows top-down.
void FlipRows(const u8* dib, u8* out, int w, int h)
{
    size_t row = (size_t)w * 4;
    for (int y = 0; y < h; ++y) memcpy(out + (size_t)y * row, dib + (size_t)(h - 1 - y) * row, row);
}

// =================================================================================================================
// AVI: RIFF 'AVI ' { hdrl { avih, strl(video MJPG), strl(audio PCM) }, movi { 00dc..., 01wb... }, idx1 }
// The video is constant-rate: a frame that comes late leaves empty '00dc' chunks (the player repeats the picture), so the
// picture stays in step with the sound. A file is at most ~3.9 GB (the RIFF limit); Full() then stops the recording.
// =================================================================================================================
class AviSink : public FrameSink {
public:
    AviSink(int w, int h, int fps, double quality, bool audio) : m_w(w), m_h(h), m_fps(fps), m_q(quality), m_audio(audio) {}
    ~AviSink() override
    {
        if (m_file != INVALID_HANDLE_VALUE) CloseHandle(m_file);
        free(m_idx); free(m_flip); free(m_prev);
        if (m_wic) m_wic->Release();
    }

    bool Open(const wchar_t* path, wchar_t* why, int whyLen)
    {
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&m_wic);
        if (FAILED(hr)) { Why(why, whyLen, L"WIC", hr); return false; }
        m_flip = (u8*)malloc((size_t)m_w * m_h * 4);
        if (!m_flip) { Why(why, whyLen, L"memory"); return false; }
        m_interval = 10000000LL / m_fps;
        m_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_file == INVALID_HANDLE_VALUE) { Why(why, whyLen, L"CreateFile", HRESULT_FROM_WIN32(GetLastError())); return false; }
        if (!WriteHeader()) { Why(why, whyLen, L"write"); return false; }
        return true;
    }

    bool Video(const u8* dib, LONGLONG ts) override
    {
        if (m_full) return true;
        LONGLONG want = ts / m_interval;
        if (want < m_nextFrame) return true;                     // not later than the previous one: nothing to add
        LONGLONG gap = want - m_nextFrame;
        if (gap > 2000) gap = 2000;
        for (LONGLONG i = 0; i < gap; ++i) if (!Chunk('0', '0', 'd', 'c', nullptr, 0, 0)) return false;   // repeat the last picture
        // A picture that is the same as the last one is not encoded again: empty chunks repeat the last picture (a still screen then costs
        // nothing, not a full JPEG of 100-400 KB 25 times a second).
        const size_t bytes = (size_t)m_w * m_h * 4;
        if (m_prev && m_havePrev && !memcmp(dib, m_prev, bytes)) {
            if (!Chunk('0', '0', 'd', 'c', nullptr, 0, 0)) return false;
            m_nextFrame = want + 1;
            if (m_pos > 3900000000ULL) m_full = true;
            return true;
        }
        if (!m_prev) m_prev = (u8*)malloc(bytes);
        if (m_prev) { memcpy(m_prev, dib, bytes); m_havePrev = true; }
        FlipRows(dib, m_flip, m_w, m_h);
        IWICBitmap* bmp = nullptr;
        if (FAILED(m_wic->CreateBitmapFromMemory(m_w, m_h, GUID_WICPixelFormat32bppBGR, m_w * 4, (UINT)((size_t)m_w * m_h * 4), m_flip, &bmp))) return false;
        bool ok = EncodeJpeg(bmp);
        bmp->Release();
        m_nextFrame = want + 1;
        if (m_pos > 3900000000ULL) m_full = true;
        return ok;
    }

    bool Audio(const void* pcm, uint32_t bytes) override
    {
        if (!m_audio || m_full) return true;
        m_audioBytes += bytes;
        return Chunk('0', '1', 'w', 'b', pcm, bytes, 0);
    }

    bool Full() const override { return m_full; }

    bool Close(bool ok) override
    {
        if (m_file == INVALID_HANDLE_VALUE) return false;
        bool good = ok;
        const DWORD moviEnd = (DWORD)m_pos;
        if (good) {                                               // idx1
            good = PutFcc('i', 'd', 'x', '1') && Put32((DWORD)(m_nIdx * 16));
            for (size_t i = 0; good && i < m_nIdx; ++i)
                good = Put32(m_idx[i].id) && Put32(m_idx[i].flags) && Put32(m_idx[i].off) && Put32(m_idx[i].size);
        }
        if (good) {
            m_moviEnd = moviEnd;
            m_riffSize = (DWORD)m_pos - 8;
            m_final = true;
            good = WriteHeader();                                  // the same header again, now with the real numbers
        }
        CloseHandle(m_file); m_file = INVALID_HANDLE_VALUE;
        return good;
    }

private:
    struct Idx { DWORD id, flags, off, size; };

    bool Raw(const void* d, DWORD n)
    {
        DWORD wr = 0;
        if (n == 0) return true;
        if (!WriteFile(m_file, d, n, &wr, nullptr) || wr != n) return false;
        m_pos += n;
        return true;
    }
    bool Put32(DWORD v) { return Raw(&v, 4); }
    bool Put16(WORD v) { return Raw(&v, 2); }
    bool PutFcc(char a, char b, char c, char d) { char f[4] = { a, b, c, d }; return Raw(f, 4); }

    // One chunk of the 'movi' list (and its index entry).
    bool Chunk(char a, char b, char c, char d, const void* data, DWORD n, DWORD flags)
    {
        DWORD off = (DWORD)(m_pos - m_moviData);
        if (!PutFcc(a, b, c, d) || !Put32(n) || !Raw(data, n)) return false;
        if (n & 1) { char z = 0; if (!Raw(&z, 1)) return false; }
        if (m_nIdx == m_capIdx) {
            size_t cap = m_capIdx ? m_capIdx * 2 : 4096;
            Idx* p = (Idx*)realloc(m_idx, cap * sizeof(Idx));
            if (!p) return false;
            m_idx = p; m_capIdx = cap;
        }
        char f[4] = { a, b, c, d };
        memcpy(&m_idx[m_nIdx].id, f, 4);
        m_idx[m_nIdx].flags = flags; m_idx[m_nIdx].off = off; m_idx[m_nIdx].size = n;
        ++m_nIdx;
        if (n > m_maxChunk) m_maxChunk = n;
        return true;
    }

    bool EncodeJpeg(IWICBitmap* bmp)
    {
        IStream* stm = nullptr; IWICStream* ws = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr;
        IPropertyBag2* props = nullptr;
        bool ok = false;
        HGLOBAL hg = nullptr;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stm))) return false;
        if (SUCCEEDED(m_wic->CreateStream(&ws)) && SUCCEEDED(ws->InitializeFromIStream(stm)) &&
            SUCCEEDED(m_wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) &&
            SUCCEEDED(enc->Initialize(ws, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, &props))) {
            if (props) {
                PROPBAG2 opt; memset(&opt, 0, sizeof(opt)); opt.pstrName = (LPOLESTR)L"ImageQuality";
                VARIANT v; VariantInit(&v); v.vt = VT_R4; v.fltVal = (float)m_q;
                props->Write(1, &opt, &v);
                PROPBAG2 sub; memset(&sub, 0, sizeof(sub)); sub.pstrName = (LPOLESTR)L"JpegYCrCbSubsampling";      // 4:2:0: the colour at half the size, as any video
                VARIANT vs; VariantInit(&vs); vs.vt = VT_UI1; vs.bVal = 1;                                         // WICJpegYCrCbSubsampling420
                props->Write(1, &sub, &vs);
            }
            WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
            if (SUCCEEDED(fr->Initialize(props)) && SUCCEEDED(fr->SetSize(m_w, m_h)) && SUCCEEDED(fr->SetPixelFormat(&pf)) &&
                SUCCEEDED(fr->WriteSource(bmp, nullptr)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit())) {
                STATSTG ss; memset(&ss, 0, sizeof(ss));
                if (SUCCEEDED(stm->Stat(&ss, STATFLAG_NONAME)) && SUCCEEDED(GetHGlobalFromStream(stm, &hg)) && hg) {
                    void* p = GlobalLock(hg);
                    if (p) {
                        ok = Chunk('0', '0', 'd', 'c', p, (DWORD)ss.cbSize.QuadPart, 0x10 /*AVIIF_KEYFRAME*/);
                        GlobalUnlock(hg);
                    }
                }
            }
        }
        if (props) props->Release();
        if (fr) fr->Release();
        if (enc) enc->Release();
        if (ws) ws->Release();
        stm->Release();
        return ok;
    }

    // The fixed-size header; written at the start with empty counts and again at the end with the real ones.
    bool WriteHeader()
    {
        LARGE_INTEGER z; z.QuadPart = 0;
        if (!SetFilePointerEx(m_file, z, nullptr, FILE_BEGIN)) return false;
        m_pos = 0;
        const DWORD frames = (DWORD)m_nextFrame;
        const DWORD audioSamples = (DWORD)(m_audioBytes / 2);
        const DWORD strlV = 4 + (8 + 56) + (8 + 40), strlA = 4 + (8 + 56) + (8 + 16);
        const DWORD hdrl = 4 + (8 + 56) + (8 + strlV) + (m_audio ? 8 + strlA : 0);
        const DWORD moviList = m_final ? (m_moviEnd - m_moviListPos - 8) : 4;
        bool ok = PutFcc('R', 'I', 'F', 'F') && Put32(m_final ? m_riffSize : 0) && PutFcc('A', 'V', 'I', ' ');
        ok = ok && PutFcc('L', 'I', 'S', 'T') && Put32(hdrl) && PutFcc('h', 'd', 'r', 'l');
        // avih
        ok = ok && PutFcc('a', 'v', 'i', 'h') && Put32(56) && Put32((DWORD)(m_interval / 10)) /*us per frame*/ &&
             Put32((DWORD)((uint64_t)m_maxChunk * m_fps)) && Put32(0) && Put32(0x10 | 0x100) && Put32(frames) && Put32(0) &&
             Put32(m_audio ? 2 : 1) && Put32(m_maxChunk) && Put32(m_w) && Put32(m_h) && Put32(0) && Put32(0) && Put32(0) && Put32(0);
        // video stream
        ok = ok && PutFcc('L', 'I', 'S', 'T') && Put32(strlV) && PutFcc('s', 't', 'r', 'l');
        ok = ok && PutFcc('s', 't', 'r', 'h') && Put32(56) && PutFcc('v', 'i', 'd', 's') && PutFcc('M', 'J', 'P', 'G') && Put32(0) && Put16(0) && Put16(0) &&
             Put32(0) && Put32(1) && Put32(m_fps) && Put32(0) && Put32(frames) && Put32(m_maxChunk) && Put32(0xFFFFFFFF) && Put32(0) &&
             Put16(0) && Put16(0) && Put16((WORD)m_w) && Put16((WORD)m_h);
        ok = ok && PutFcc('s', 't', 'r', 'f') && Put32(40) && Put32(40) && Put32(m_w) && Put32(m_h) && Put16(1) && Put16(24) &&
             PutFcc('M', 'J', 'P', 'G') && Put32((DWORD)((size_t)m_w * m_h * 3)) && Put32(0) && Put32(0) && Put32(0) && Put32(0);
        if (m_audio) {
            ok = ok && PutFcc('L', 'I', 'S', 'T') && Put32(strlA) && PutFcc('s', 't', 'r', 'l');
            ok = ok && PutFcc('s', 't', 'r', 'h') && Put32(56) && PutFcc('a', 'u', 'd', 's') && Put32(0) && Put32(0) && Put16(0) && Put16(0) &&
                 Put32(0) && Put32(1) && Put32(44100) && Put32(0) && Put32(audioSamples) && Put32(0) && Put32(0xFFFFFFFF) && Put32(2) &&
                 Put16(0) && Put16(0) && Put16(0) && Put16(0);
            ok = ok && PutFcc('s', 't', 'r', 'f') && Put32(16) && Put16(1) /*PCM*/ && Put16(1) && Put32(44100) && Put32(88200) && Put16(2) && Put16(16);
        }
        m_moviListPos = (DWORD)m_pos;                              // the movi list starts here
        ok = ok && PutFcc('L', 'I', 'S', 'T') && Put32(moviList) && PutFcc('m', 'o', 'v', 'i');
        m_moviData = m_pos - 4;                                    // chunk offsets in idx1 count from the 'movi' fourcc
        return ok;
    }

    int m_w, m_h, m_fps;
    double m_q;
    bool m_audio;
    IWICImagingFactory* m_wic = nullptr;
    HANDLE m_file = INVALID_HANDLE_VALUE;
    u8* m_flip = nullptr;
    u8* m_prev = nullptr; bool m_havePrev = false;         // the last picture as it came, to tell an unchanged one
    uint64_t m_pos = 0;
    LONGLONG m_interval = 400000;
    LONGLONG m_nextFrame = 0;
    uint64_t m_audioBytes = 0;
    DWORD m_maxChunk = 0;
    DWORD m_moviListPos = 0, m_moviEnd = 0, m_riffSize = 0;
    uint64_t m_moviData = 0;
    bool m_full = false, m_final = false;
    Idx* m_idx = nullptr; size_t m_nIdx = 0, m_capIdx = 0;
};

// =================================================================================================================
// Animated GIF: every frame has its own 256-colour palette and a delay taken from the real time stamps.
// =================================================================================================================
class GifSink : public FrameSink {
public:
    GifSink(int w, int h, int fps) : m_w(w), m_h(h), m_interval(10000000LL / fps) {}
    ~GifSink() override
    {
        if (m_enc) m_enc->Release();
        if (m_st) m_st->Release();
        if (m_wic) m_wic->Release();
        free(m_prev); free(m_cur);
    }

    bool Open(const wchar_t* path, wchar_t* why, int whyLen)
    {
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&m_wic);
        if (FAILED(hr)) { Why(why, whyLen, L"WIC", hr); return false; }
        size_t n = (size_t)m_w * m_h * 4;
        m_prev = (u8*)malloc(n); m_cur = (u8*)malloc(n);
        if (!m_prev || !m_cur) { Why(why, whyLen, L"memory"); return false; }
        hr = m_wic->CreateStream(&m_st);
        if (SUCCEEDED(hr)) hr = m_st->InitializeFromFilename(path, GENERIC_WRITE);
        if (SUCCEEDED(hr)) hr = m_wic->CreateEncoder(GUID_ContainerFormatGif, nullptr, &m_enc);
        if (SUCCEEDED(hr)) hr = m_enc->Initialize(m_st, WICBitmapEncoderNoCache);
        if (FAILED(hr)) { Why(why, whyLen, L"GIF encoder", hr); return false; }
        // play forever (the Netscape looping extension); an encoder that does not know it just ignores that
        IWICMetadataQueryWriter* qw = nullptr;
        if (SUCCEEDED(m_enc->GetMetadataQueryWriter(&qw)) && qw) {
            static u8 app[11] = { 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0' };
            static u8 data[5] = { 3, 1, 0, 0, 0 };
            PROPVARIANT pv; PropVariantInit(&pv);
            pv.vt = VT_UI1 | VT_VECTOR; pv.caub.cElems = 11; pv.caub.pElems = app;
            qw->SetMetadataByName(L"/appext/Application", &pv);
            pv.caub.cElems = 5; pv.caub.pElems = data;
            qw->SetMetadataByName(L"/appext/Data", &pv);
            qw->Release();
        }
        return true;
    }

    bool Video(const u8* dib, LONGLONG ts) override
    {
        FlipRows(dib, m_cur, m_w, m_h);
        bool ok = true;
        if (m_have) {
            int cs = (int)((ts - m_prevTs + 50000) / 100000);       // 1/100 s
            if (cs < 2) cs = 2;
            if (cs > 6000) cs = 6000;
            ok = WriteFrame(m_prev, cs);
        }
        u8* t = m_prev; m_prev = m_cur; m_cur = t;
        m_prevTs = ts; m_have = true;
        return ok;
    }

    bool Audio(const void*, uint32_t) override { return true; }

    bool Close(bool ok) override
    {
        bool good = ok;
        if (m_have && good) { int cs = (int)(m_interval / 100000); good = WriteFrame(m_prev, cs < 2 ? 2 : cs); }
        if (good && m_frames > 0) good = SUCCEEDED(m_enc->Commit());
        else good = false;
        return good;
    }

private:
    // The palette comes from a picture a third of the size on each side: nine times fewer pixels to count, the same colours.
    bool PaletteFrom(IWICBitmap* bmp, IWICPalette* pal)
    {
        IWICBitmapScaler* sc = nullptr;
        bool ok = false;
        if (m_w >= 90 && m_h >= 90 && SUCCEEDED(m_wic->CreateBitmapScaler(&sc)) &&
            SUCCEEDED(sc->Initialize(bmp, m_w / 3, m_h / 3, WICBitmapInterpolationModeNearestNeighbor)))
            ok = SUCCEEDED(pal->InitializeFromBitmap(sc, 256, FALSE));
        if (sc) sc->Release();
        return ok || SUCCEEDED(pal->InitializeFromBitmap(bmp, 256, FALSE));
    }

    bool WriteFrame(const u8* topDown, int delayCs)
    {
        IWICBitmap* bmp = nullptr; IWICBitmapFrameEncode* fr = nullptr; IPropertyBag2* props = nullptr;
        IWICPalette* pal = nullptr; IWICFormatConverter* conv = nullptr; IWICMetadataQueryWriter* mw = nullptr;
        bool ok = false;
        if (SUCCEEDED(m_wic->CreateBitmapFromMemory(m_w, m_h, GUID_WICPixelFormat32bppBGR, m_w * 4, (UINT)((size_t)m_w * m_h * 4), (BYTE*)topDown, &bmp)) &&
            SUCCEEDED(m_wic->CreatePalette(&pal)) && PaletteFrom(bmp, pal) &&
            SUCCEEDED(m_wic->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(bmp, GUID_WICPixelFormat8bppIndexed, WICBitmapDitherTypeNone, pal, 0.0, WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(m_enc->CreateNewFrame(&fr, &props))) {
            WICPixelFormatGUID pf = GUID_WICPixelFormat8bppIndexed;
            if (SUCCEEDED(fr->Initialize(props)) && SUCCEEDED(fr->SetSize(m_w, m_h)) && SUCCEEDED(fr->SetPixelFormat(&pf)) && SUCCEEDED(fr->SetPalette(pal))) {
                if (SUCCEEDED(fr->GetMetadataQueryWriter(&mw)) && mw) {
                    PROPVARIANT pv; PropVariantInit(&pv);
                    pv.vt = VT_UI2; pv.uiVal = (USHORT)delayCs;
                    mw->SetMetadataByName(L"/grctlext/Delay", &pv);
                    pv.vt = VT_UI1; pv.bVal = 1;                      // leave the frame in place: the next one is drawn over it
                    mw->SetMetadataByName(L"/grctlext/Disposal", &pv);
                }
                ok = SUCCEEDED(fr->WriteSource(conv, nullptr)) && SUCCEEDED(fr->Commit());
                if (ok) ++m_frames;
            }
        }
        if (mw) mw->Release();
        if (conv) conv->Release();
        if (pal) pal->Release();
        if (props) props->Release();
        if (fr) fr->Release();
        if (bmp) bmp->Release();
        return ok;
    }

    int m_w, m_h;
    LONGLONG m_interval;
    IWICImagingFactory* m_wic = nullptr;
    IWICStream* m_st = nullptr;
    IWICBitmapEncoder* m_enc = nullptr;
    u8* m_prev = nullptr; u8* m_cur = nullptr;
    LONGLONG m_prevTs = 0;
    bool m_have = false;
    int m_frames = 0;
};

} // namespace

FrameSink* CreateAviSink(const wchar_t* path, int w, int h, int fps, int quality, bool audio, wchar_t* why, int whyLen)
{
    static const double q[3] = { 0.8, 0.65, 0.45 };      // high / medium / low (a screen with text stays readable far below the 0.9 of a photograph)
    AviSink* s = new AviSink(w, h, fps, q[quality < 0 ? 0 : quality > 2 ? 2 : quality], audio);
    if (!s->Open(path, why, whyLen)) { delete s; DeleteFileW(path); return nullptr; }
    return s;
}

FrameSink* CreateGifSink(const wchar_t* path, int w, int h, int fps, wchar_t* why, int whyLen)
{
    GifSink* s = new GifSink(w, h, fps);
    if (!s->Open(path, why, whyLen)) { delete s; DeleteFileW(path); return nullptr; }
    return s;
}
