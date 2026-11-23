#include "raw_linearity.h"

#include "image.h"

#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <cerrno>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <vector>

namespace linearity
{
namespace
{

std::mutex lock;  // remember() comes from the GPU's readbacks

const char* const kColours[3] = {"red", "green", "blue"};

std::string group_of(const std::string& camera, const std::string& format)
{
    // A group's name can't hold brackets or line breaks.
    std::string g = camera + " (" + format + ")";
    for (char& c : g)
        if (c == '[' || c == ']' || c == '\n' || c == '\r')
            c = ' ';
    return g;
}

std::vector<double> kept(GKeyFile* file, const std::string& group, const char* key)
{
    gsize n = 0;
    gdouble* v = g_key_file_get_double_list(file, group.c_str(), key, &n, nullptr);
    std::vector<double> out;
    for (gsize i = 0; v && i < n; ++i)
        if (v[i] > 0 && v[i] <= 1)
            out.push_back(v[i]);
    g_free(v);
    return out;
}

// The file locked against other processes for a read-modify-write, so a
// second remember() waits for the first's save rather than both loading the
// old file and the later save losing the earlier's group. The lock is the
// system's, on a lock file beside it (flock; on Windows the file open
// unshared), so a process that dies holding it lets go with it: there's no
// telling a crashed holder's lock by its age, and no taking one over, which
// raced (two finding the same old lock, the second removing the first's new
// one). The holder removes the file before letting go; one that locked a file
// since removed tries the one at the path now.
class FileLock
{
public:
    explicit FileLock(const std::string& file) : path_(file + ".lock")
    {
        constexpr int kTries = 400;  // 10 ms apart: 4 s
#ifdef _WIN32
        gunichar2* wide = g_utf8_to_utf16(path_.c_str(), -1, nullptr, nullptr, nullptr);
        if (!wide)
            return;
        for (int i = 0; i < kTries && !held_; ++i)
        {
            // Unshared, so a second open fails while it's held; deleted as the
            // last handle closes, a dead process's included.
            handle_ = CreateFileW(reinterpret_cast<const wchar_t*>(wide), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (handle_ != INVALID_HANDLE_VALUE)
            {
                held_ = true;
                break;
            }
            const DWORD e = GetLastError();
            if (e != ERROR_SHARING_VIOLATION && e != ERROR_ACCESS_DENIED)  // (denied: being deleted as its holder let go)
                break;
            g_usleep(10000);
        }
        g_free(wide);
#else
        for (int i = 0; i < kTries && !held_; ++i)
        {
            const int fd = g_open(path_.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
            if (fd < 0)
                return;
            if (flock(fd, LOCK_EX | LOCK_NB) == 0)
            {
                struct stat locked, now;
                if (fstat(fd, &locked) == 0 && ::stat(path_.c_str(), &now) == 0 && locked.st_dev == now.st_dev && locked.st_ino == now.st_ino)
                {
                    fd_ = fd;
                    held_ = true;
                    break;
                }
                close(fd);  // its holder removed it as we opened it
                continue;
            }
            const int e = errno;
            close(fd);
            if (e != EWOULDBLOCK && e != EINTR)
                return;
            g_usleep(10000);
        }
#endif
    }
    ~FileLock()
    {
        if (!held_)
            return;
#ifdef _WIN32
        CloseHandle(handle_);
#else
        g_remove(path_.c_str());  // before letting go: what waits on this file then finds it gone
        close(fd_);
#endif
    }
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    bool held() const { return held_; }

private:
    std::string path_;
    bool held_ = false;
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

// IFRIT_LINEARITY_HOLD (for tests): once the file's read under the lock, say
// so with <it>.held and hold the lock until <it> exists (up to 20 s).
void hold_for_test()
{
    const char* hold = g_getenv("IFRIT_LINEARITY_HOLD");
    if (!hold)
        return;
    const std::string held = std::string(hold) + ".held";
    g_file_set_contents(held.c_str(), "", 0, nullptr);
    for (int i = 0; i < 2000 && !g_file_test(hold, G_FILE_TEST_EXISTS); ++i)
        g_usleep(10000);
}

}  // namespace

std::string path()
{
    gchar* p = g_build_filename(g_get_user_config_dir(), "ifritview", "linearity.conf", nullptr);
    std::string s = p;
    g_free(p);
    return s;
}

std::string format_of(const RawMosaic& m)
{
    char s[96];
    if (m.ycc.block_h)
        std::snprintf(s, sizeof s, "YCbCr 4:2:%d %dx%d, white %.0f", m.ycc.block_h == 2 ? 0 : 2, m.width, m.height, m.white);
    else
        std::snprintf(s, sizeof s, "%s, white %.0f",
                      m.channels == 3 ? "linear" : m.grey ? "monochrome" : m.period == 6 ? "X-Trans" : "Bayer", m.white);
    return s;
}

void recall(const std::string& camera, const std::string& format, float limit[3])
{
    limit[0] = limit[1] = limit[2] = 0;
    if (camera.empty())
        return;
    std::lock_guard<std::mutex> hold(lock);
    GKeyFile* file = g_key_file_new();
    if (g_key_file_load_from_file(file, path().c_str(), G_KEY_FILE_NONE, nullptr))
    {
        const std::string group = group_of(camera, format);
        for (int c = 0; c < 3; ++c)
        {
            std::vector<double> v = kept(file, group, kColours[c]);
            if (v.empty())
                continue;
            std::sort(v.begin(), v.end());
            const size_t n = v.size();
            limit[c] = float(n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2);
        }
    }
    g_key_file_free(file);
}

void remember(const std::string& camera, const std::string& format, const float limit[3], const bool sure[3])
{
    if (camera.empty() || !(sure[0] || sure[1] || sure[2]))
        return;
    std::lock_guard<std::mutex> hold(lock);
    const std::string file_path = path();
    gchar* dir = g_path_get_dirname(file_path.c_str());
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    // Another process may be remembering too: read, change and save under
    // the file's lock (the save itself is atomic: a temporary renamed in).
    FileLock locked(file_path);
    if (!locked.held())
        return;  // held for seconds by another: this measurement goes unremembered
    GKeyFile* file = g_key_file_new();
    g_key_file_load_from_file(file, file_path.c_str(), G_KEY_FILE_KEEP_COMMENTS, nullptr);
    hold_for_test();
    const std::string group = group_of(camera, format);
    for (int c = 0; c < 3; ++c)
    {
        if (!sure[c] || !(limit[c] > 0 && limit[c] <= 1))
            continue;
        std::vector<double> v = kept(file, group, kColours[c]);
        v.push_back(limit[c]);
        if (v.size() > size_t(kLinearityKept))
            v.erase(v.begin(), v.end() - kLinearityKept);
        g_key_file_set_double_list(file, group.c_str(), kColours[c], v.data(), v.size());
    }
    g_key_file_save_to_file(file, file_path.c_str(), nullptr);
    g_key_file_free(file);
}

}  // namespace linearity
