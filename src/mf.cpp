#include "mf.h"
#include "platform.h"

#include <cstring>
#include <mutex>

namespace mf
{

namespace
{

struct Api
{
    HMODULE plat = nullptr, readwrite = nullptr;
    HRESULT(WINAPI* startup)(ULONG, DWORD) = nullptr;
    HRESULT(WINAPI* create_attributes)(IMFAttributes**, UINT32) = nullptr;
    HRESULT(WINAPI* create_media_type)(IMFMediaType**) = nullptr;
    HRESULT(WINAPI* create_source_reader)(LPCWSTR, IMFAttributes*, IMFSourceReader**) = nullptr;
    bool started = false;
};

template <class F>
void resolve(HMODULE m, const char* name, F& fn)
{
    if (m)
        fn = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name)));
}

// Loaded and started on first use, from whichever thread gets there first.
Api& api()
{
    static Api a;
    static std::once_flag once;
    std::call_once(once,
                   []
                   {
                       a.plat = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                       a.readwrite = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                       resolve(a.plat, "MFStartup", a.startup);
                       resolve(a.plat, "MFCreateAttributes", a.create_attributes);
                       resolve(a.plat, "MFCreateMediaType", a.create_media_type);
                       resolve(a.readwrite, "MFCreateSourceReaderFromURL", a.create_source_reader);
                       a.started = a.startup && a.create_attributes && a.create_media_type && a.create_source_reader &&
                                   SUCCEEDED(a.startup(MF_VERSION, MFSTARTUP_FULL));
                   });
    return a;
}

}  // namespace

bool available()
{
    // A stat of the system directory, not a load.
    wchar_t dir[MAX_PATH];
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return false;
    const std::wstring path = std::wstring(dir) + L"\\mfplat.dll";
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

Thread::Thread()
{
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    uninitialize_ = SUCCEEDED(hr);  // RPC_E_CHANGED_MODE: the thread already has COM, which will do
    ok_ = (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) && api().started;
}

Thread::~Thread()
{
    if (uninitialize_)
        CoUninitialize();
}

HRESULT create_attributes(IMFAttributes** out, UINT32 size)
{
    return api().started ? api().create_attributes(out, size) : E_NOTIMPL;
}

HRESULT create_media_type(IMFMediaType** out)
{
    return api().started ? api().create_media_type(out) : E_NOTIMPL;
}

HRESULT create_source_reader(const std::wstring& url, IMFAttributes* attributes, IMFSourceReader** out)
{
    return api().started ? api().create_source_reader(url.c_str(), attributes, out) : E_NOTIMPL;
}

int upright_turn(IMFMediaType* native_type)
{
    // The tag says how far the stored frames are turned anticlockwise from
    // upright; turning them back is the same amount clockwise.
    UINT32 rotation = 0;
    if (!native_type || FAILED(native_type->GetUINT32(MF_MT_VIDEO_ROTATION, &rotation)))
        return 0;
    return int(rotation % 360) / 90 * 90;
}

void copy_upright(const uint8_t* src, int src_w, int src_h, ptrdiff_t stride, int turn, PixelBuffer& out, int& w, int& h)
{
    const bool sideways = turn == 90 || turn == 270;
    w = sideways ? src_h : src_w;
    h = sideways ? src_w : src_h;
    out.resize(size_t(w) * h * 4);
    auto* dst = reinterpret_cast<uint32_t*>(out.data());
    if (turn == 0)
    {
        for (int y = 0; y < src_h; ++y)
        {
            const auto* s = reinterpret_cast<const uint32_t*>(src + y * stride);
            uint32_t* d = dst + size_t(y) * w;
            for (int x = 0; x < src_w; ++x)
                d[x] = s[x] | 0xff000000u;
        }
        return;
    }
    // Source pixel (x, y) lands at:  90: (h-1-y, x)   180: (w-1-x, h-1-y)   270: (y, w-1-x)
    for (int y = 0; y < src_h; ++y)
    {
        const auto* s = reinterpret_cast<const uint32_t*>(src + y * stride);
        for (int x = 0; x < src_w; ++x)
        {
            int dx, dy;
            if (turn == 90)
                dx = w - 1 - y, dy = x;
            else if (turn == 180)
                dx = w - 1 - x, dy = h - 1 - y;
            else
                dx = y, dy = h - 1 - x;
            dst[size_t(dy) * w + dx] = s[x] | 0xff000000u;
        }
    }
}

}  // namespace mf
