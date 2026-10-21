#include "dynlib.h"

#include <dlfcn.h>
#include <glib.h>

#include <string>
#include <vector>

namespace
{

#if defined(__x86_64__)
constexpr const char* kTriplet = "x86_64-linux-gnu";
#elif defined(__aarch64__)
constexpr const char* kTriplet = "aarch64-linux-gnu";
#elif defined(__arm__)
constexpr const char* kTriplet = "arm-linux-gnueabihf";
#else
constexpr const char* kTriplet = "";
#endif

std::vector<std::string> search_path()
{
    std::vector<std::string> dirs;
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
    for (const char* base : {"/usr/lib", "/lib"})
        if (*kTriplet)
            dirs.push_back(std::string(base) + "/" + kTriplet);
    for (const char* d : {"/usr/lib64", "/lib64", "/usr/lib", "/lib", "/usr/local/lib"})
        dirs.push_back(d);
    return dirs;
}

}  // namespace

bool library_installed(const char* soname)
{
    static const std::vector<std::string> dirs = search_path();
    for (const std::string& d : dirs)
        if (g_file_test((d + "/" + soname).c_str(), G_FILE_TEST_EXISTS))
            return true;
    return false;
}
