// Videos in the series on Windows: the first frame (upright, per the rotation
// tag) is the image; the player takes over once it's on screen. Media
// Foundation reads the file itself, so this decoder is marked streaming and
// gets no preload.
#include "../mf.h"
#include "../platform.h"
#include "../video.h"
#include "decoder.h"

#include <mmsystem.h>  // MAKEFOURCC (not in windows.h when it's lean)
#include <propvarutil.h>

#include <cstdio>
#include <cstring>

namespace
{

std::string clock_text(double seconds)
{
    const int s = int(seconds + 0.5);
    char buf[32];
    if (s >= 3600)
        std::snprintf(buf, sizeof buf, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else
        std::snprintf(buf, sizeof buf, "%d:%02d", s / 60, s % 60);
    return buf;
}

// Media Foundation names codecs by GUID; their first word is the FOURCC
// (video) or the wave format tag (audio).
std::string video_codec(const GUID& subtype)
{
    switch (subtype.Data1)
    {
        case MAKEFOURCC('H', '2', '6', '4'): return "H.264";
        case MAKEFOURCC('H', 'E', 'V', 'C'): return "H.265 (HEVC)";
        case MAKEFOURCC('V', 'P', '8', '0'): return "VP8";
        case MAKEFOURCC('V', 'P', '9', '0'): return "VP9";
        case MAKEFOURCC('A', 'V', '0', '1'): return "AV1";
        case MAKEFOURCC('M', 'P', '4', 'V'): return "MPEG-4 Part 2";
        case MAKEFOURCC('M', 'P', 'G', '1'): return "MPEG-1";
        case MAKEFOURCC('M', 'P', 'G', '2'): return "MPEG-2";
        case MAKEFOURCC('M', 'J', 'P', 'G'): return "Motion JPEG";
        case MAKEFOURCC('W', 'M', 'V', '3'): return "WMV 9";
        case MAKEFOURCC('W', 'V', 'C', '1'): return "VC-1";
        case MAKEFOURCC('H', '2', '6', '3'): return "H.263";
    }
    char buf[8];
    std::memcpy(buf, &subtype.Data1, 4);
    buf[4] = 0;
    return buf;
}

std::string audio_codec(const GUID& subtype)
{
    switch (subtype.Data1)
    {
        case 0x1610: return "AAC";
        case 0x0055: return "MP3";
        case 0x0050: return "MPEG audio";
        case 0x2000: return "AC-3";
        case 0x2001: return "DTS";
        case 0x704F: return "Opus";
        case 0xF1AC: return "FLAC";
        case 0x6C61: return "ALAC";
        case 0x0001: return "PCM";
        case 0x0003: return "PCM (float)";
        case 0x0160:
        case 0x0161:
        case 0x0162: return "WMA";
        case 0x0163: return "WMA Lossless";
    }
    return "yes";
}

bool decode(const std::string& path, Image& out, std::string& error)
{
    mf::Thread thread;
    if (!thread.ok())
    {
        error = "Media Foundation unavailable";
        return false;
    }
    mf::Ptr<IMFAttributes> attributes;
    mf::create_attributes(attributes.put(), 1);
    if (attributes)
        attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);  // decodes to RGB32 for us
    mf::Ptr<IMFSourceReader> reader;
    if (FAILED(mf::create_source_reader(platform::widen(path), attributes.get(), reader.put())))
    {
        error = "Media Foundation can't open it";
        return false;
    }
    constexpr DWORD video = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), audio = DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    mf::Ptr<IMFMediaType> native;
    if (FAILED(reader->GetNativeMediaType(video, 0, native.put())))
    {
        error = "no video stream";
        return false;
    }
    GUID vsub{};
    native->GetGUID(MF_MT_SUBTYPE, &vsub);
    const int turn = mf::upright_turn(native.get());

    mf::Ptr<IMFMediaType> rgb;
    mf::create_media_type(rgb.put());
    if (!rgb)
        return false;
    rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(video, TRUE);
    if (FAILED(reader->SetCurrentMediaType(video, nullptr, rgb.get())))
    {
        error = "no decoder for " + video_codec(vsub);
        return false;
    }
    mf::Ptr<IMFMediaType> current;
    reader->GetCurrentMediaType(video, current.put());
    UINT32 fw = 0, fh = 0;
    if (!current || FAILED(MFGetAttributeSize(current.get(), MF_MT_FRAME_SIZE, &fw, &fh)) || !fw || !fh)
    {
        error = "no frame size";
        return false;
    }
    // Decoders round up to whole macroblocks (1080 -> 1088); the aperture is the picture.
    UINT32 left = 0, top = 0, aw = fw, ah = fh;
    MFVideoArea area{};
    if (SUCCEEDED(current->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof area, nullptr)))
    {
        left = UINT32(area.OffsetX.value), top = UINT32(area.OffsetY.value);
        aw = UINT32(area.Area.cx), ah = UINT32(area.Area.cy);
        if (!aw || !ah || left + aw > fw || top + ah > fh)
            left = top = 0, aw = fw, ah = fh;
    }
    INT32 stride = INT32(fw * 4);
    current->GetUINT32(MF_MT_DEFAULT_STRIDE, reinterpret_cast<UINT32*>(&stride));

    mf::Ptr<IMFSample> sample;
    for (int tries = 0; tries < 50 && !sample; ++tries)
    {
        DWORD flags = 0;
        if (FAILED(reader->ReadSample(video, 0, nullptr, &flags, nullptr, sample.put())) || (flags & MF_SOURCE_READERF_ENDOFSTREAM))
            break;
    }
    mf::Ptr<IMFMediaBuffer> buffer;
    if (!sample || FAILED(sample->ConvertToContiguousBuffer(buffer.put())))
    {
        error = "no video frame";
        return false;
    }
    BYTE* data = nullptr;
    DWORD length = 0;
    if (FAILED(buffer->Lock(&data, nullptr, &length)) || length < DWORD(std::abs(stride)) * fh)
    {
        error = "no video frame";
        return false;
    }
    // A negative stride is a bottom-up frame: its first row is at the end.
    const uint8_t* first = stride < 0 ? data + size_t(-stride) * (fh - 1) : data;
    PixelBuffer pixels;
    int w = 0, h = 0;
    mf::copy_upright(first + ptrdiff_t(top) * stride + left * 4, int(aw), int(ah), stride, turn, pixels, w, h);
    buffer->Unlock();
    out.allocate(w, h);
    std::memcpy(out.pixels.data(), pixels.data(), out.pixels.size());

    out.is_video = true;
    UINT32 num = 0, den = 0;
    if (SUCCEEDED(MFGetAttributeRatio(native.get(), MF_MT_FRAME_RATE, &num, &den)) && num && den)
    {
        char fps[32];
        std::snprintf(fps, sizeof fps, "%.3g fps", double(num) / den);
        out.add("Video", "Frame rate", fps);
    }
    PROPVARIANT var;
    PropVariantInit(&var);
    if (SUCCEEDED(reader->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var)) && var.vt == VT_UI8)
        out.duration = double(var.uhVal.QuadPart) / 1e7;
    PropVariantClear(&var);
    std::string acodec;
    mf::Ptr<IMFMediaType> atype;
    if (SUCCEEDED(reader->GetNativeMediaType(audio, 0, atype.put())))
    {
        GUID asub{};
        atype->GetGUID(MF_MT_SUBTYPE, &asub);
        acodec = audio_codec(asub);
        out.has_audio = true;
    }
    const std::string vcodec = video_codec(vsub);
    out.format = "Video, " + vcodec;
    out.add("Video", "Duration", out.duration > 0 ? clock_text(out.duration) : "unknown");
    out.add("Video", "Size", std::to_string(out.width) + " × " + std::to_string(out.height));
    out.add("Video", "Codec", vcodec);
    out.add("Video", "Audio", out.has_audio ? acodec : "none");
    out.pixel_format.notes.push_back(vcodec);
    return true;
}

}  // namespace

const Decoder& video_decoder()
{
    static const Decoder d{"Video", video_extensions(), nullptr, decode, nullptr, true};
    return d;
}
