#include "platform.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace platform
{

FILE* fopen(const std::string& path, const char* mode)
{
#ifdef _WIN32
    return g_fopen(path.c_str(), mode);  // UTF-8 to wide
#else
    return ::fopen(path.c_str(), mode);  // (g_fopen is a macro for fopen here)
#endif
}

FILE* fmemopen(const uint8_t* data, size_t size)
{
#ifdef _WIN32
    (void)data;
    (void)size;
    return nullptr;
#else
    return ::fmemopen(const_cast<uint8_t*>(data), size, "rb");
#endif
}

int64_t file_size(const std::string& path)
{
    GStatBuf st;
    return g_stat(path.c_str(), &st) == 0 ? int64_t(st.st_size) : -1;
}

void lower_thread_priority()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
    setpriority(PRIO_PROCESS, pid_t(syscall(SYS_gettid)), 10);
#endif
}

size_t available_memory()
{
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof status;
    return GlobalMemoryStatusEx(&status) ? size_t(status.ullAvailPhys) : 0;
#else
    size_t available = 0;
    if (FILE* f = std::fopen("/proc/meminfo", "r"))
    {
        char line[256];
        while (std::fgets(line, sizeof line, f))
        {
            unsigned long long kb = 0;
            if (std::sscanf(line, "MemAvailable: %llu kB", &kb) == 1)
            {
                available = size_t(kb) * 1024;
                break;
            }
        }
        std::fclose(f);
    }
    return available;
#endif
}

std::string executable_path()
{
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(sizeof buf / sizeof buf[0]));
    if (n == 0)
        return "ifritview.exe";
    gchar* utf8 = g_utf16_to_utf8(reinterpret_cast<const gunichar2*>(buf), glong(n), nullptr, nullptr, nullptr);
    std::string s = utf8 ? utf8 : "ifritview.exe";
    g_free(utf8);
    return s;
#else
    gchar* exe = g_file_read_link("/proc/self/exe", nullptr);
    std::string s = exe ? exe : "ifritview";
    g_free(exe);
    return s;
#endif
}

namespace
{

// Where the loader would look, in its order.
std::vector<std::string> library_dirs()
{
    std::vector<std::string> dirs;
    gchar* exe_dir = g_path_get_dirname(executable_path().c_str());
    dirs.push_back(exe_dir);  // Windows looks beside the .exe first; a bundle puts DLLs there
    g_free(exe_dir);
#ifdef _WIN32
    if (const char* path = g_getenv("PATH"))
    {
        gchar** parts = g_strsplit(path, ";", -1);
        for (gchar** p = parts; *p; ++p)
            if (**p)
                dirs.push_back(*p);
        g_strfreev(parts);
    }
#else
    // The loader's own list for this executable: LD_LIBRARY_PATH, the
    // binary's RPATH/RUNPATH, the system defaults.
    if (void* self = dlopen(nullptr, RTLD_LAZY))
    {
        Dl_serinfo size;
        if (dlinfo(self, RTLD_DI_SERINFOSIZE, &size) == 0)
        {
            std::vector<char> buf(size.dls_size);
            auto* info = reinterpret_cast<Dl_serinfo*>(buf.data());
            *info = size;
            if (dlinfo(self, RTLD_DI_SERINFO, info) == 0)
                for (unsigned i = 0; i < info->dls_cnt; ++i)
                    dirs.push_back(info->dls_serpath[i].dls_name);
        }
        dlclose(self);
    }
    // Directories the loader reaches only through ld.so.cache.
#if defined(__x86_64__)
    const char* triplet = "x86_64-linux-gnu";
#elif defined(__aarch64__)
    const char* triplet = "aarch64-linux-gnu";
#else
    const char* triplet = nullptr;
#endif
    if (triplet)
        for (const char* base : {"/usr/lib", "/lib"})
            dirs.push_back(std::string(base) + "/" + triplet);
    for (const char* d : {"/usr/lib64", "/lib64", "/usr/lib", "/lib", "/usr/local/lib"})
        dirs.push_back(d);
#endif
    return dirs;
}

}  // namespace

bool library_installed(const char* name)
{
    static const std::vector<std::string> dirs = library_dirs();
    for (const std::string& d : dirs)
    {
        gchar* path = g_build_filename(d.c_str(), name, nullptr);
        const bool found = g_file_test(path, G_FILE_TEST_EXISTS);
        g_free(path);
        if (found)
            return true;
    }
    return false;
}

void* open_library(const char* name, std::string* error)
{
#ifdef _WIN32
    void* lib = reinterpret_cast<void*>(LoadLibraryA(name));
    if (!lib && error)
        *error = std::string(name) + ": not found (error " + std::to_string(GetLastError()) + ")";
    return lib;
#else
    void* lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (!lib && error)
        *error = dlerror();
    return lib;
#endif
}

void* library_symbol(void* library, const char* symbol)
{
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), symbol));
#else
    return dlsym(library, symbol);
#endif
}

void* allocate_pixels(size_t bytes)
{
#ifdef _WIN32
    return std::malloc(bytes ? bytes : 1);
#else
    constexpr size_t huge = size_t(2) << 20;
    if (bytes >= (size_t(4) << 20))
    {
        void* p = nullptr;
        const size_t rounded = (bytes + huge - 1) & ~(huge - 1);
        if (posix_memalign(&p, huge, rounded) != 0)
            return nullptr;
        madvise(p, rounded, MADV_HUGEPAGE);
        return p;
    }
    return std::malloc(bytes ? bytes : 1);
#endif
}

void free_pixels(void* p)
{
    std::free(p);
}

namespace
{
MainDispatch& main_dispatch()
{
    static MainDispatch d;
    return d;
}
}  // namespace

void set_main_dispatch(MainDispatch dispatch)
{
    main_dispatch() = std::move(dispatch);
}

void post_main(std::function<void()> fn)
{
    if (main_dispatch())
    {
        main_dispatch()(std::move(fn));
        return;
    }
    g_idle_add_full(
        G_PRIORITY_DEFAULT_IDLE,
        +[](gpointer p) -> gboolean
        {
            (*static_cast<std::function<void()>*>(p))();
            return G_SOURCE_REMOVE;
        },
        new std::function<void()>(std::move(fn)), +[](gpointer p) { delete static_cast<std::function<void()>*>(p); });
}

}  // namespace platform
