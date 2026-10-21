// Windows: register IfritView as a handler for the listed extensions, per
// user (HKCU, no elevation). Windows 10 and later don't let an application
// make itself the default; it can only offer itself, so this registers a
// ProgID, adds it to each extension's "Open with" list, and opens Settings'
// Default apps page where the user picks it.
#include "integration.h"

#include "platform.h"

#include <glib.h>
#include <shlobj.h>
#include <windows.h>

#include <set>

namespace
{

const wchar_t* kProgId = L"IfritView.Image";

std::wstring wide(const std::string& s)
{
    gunichar2* w = g_utf8_to_utf16(s.c_str(), -1, nullptr, nullptr, nullptr);
    std::wstring out = w ? reinterpret_cast<wchar_t*>(w) : L"";
    g_free(w);
    return out;
}

bool set_value(const std::wstring& key, const wchar_t* name, const std::wstring& value, std::string& error)
{
    HKEY h = nullptr;
    LONG r = RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr);
    if (r == ERROR_SUCCESS)
    {
        r = RegSetValueExW(h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), DWORD((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(h);
    }
    if (r != ERROR_SUCCESS && error.empty())
        error = "registry write failed (" + std::to_string(r) + ")";
    return r == ERROR_SUCCESS;
}

bool is_video(const std::string& ext)
{
    for (const char* v : {"mp4", "m4v", "mov", "mts", "m2ts", "mkv", "webm", "avi", "3gp"})
        if (ext == v)
            return true;
    return false;
}

}  // namespace

bool ensure_desktop_entry(std::string& error)
{
    const std::wstring exe = wide(platform::executable_path());
    const std::wstring base = std::wstring(L"Software\\Classes\\") + kProgId;
    bool ok = set_value(base, nullptr, L"IfritView image", error);
    ok = set_value(base + L"\\DefaultIcon", nullptr, L"\"" + exe + L"\",0", error) && ok;
    ok = set_value(base + L"\\shell\\open\\command", nullptr, L"\"" + exe + L"\" \"%1\"", error) && ok;
    // Listed under "Open with" and in Default apps.
    const std::wstring app = L"Software\\Classes\\Applications\\ifritview.exe";
    ok = set_value(app + L"\\shell\\open\\command", nullptr, L"\"" + exe + L"\" \"%1\"", error) && ok;
    ok = set_value(app, L"FriendlyAppName", L"IfritView", error) && ok;
    return ok;
}

int set_default_for_extensions(const std::vector<std::string>& extensions, bool videos, bool documents, std::vector<std::string>& skipped, std::string& error)
{
    (void)skipped;
    if (!ensure_desktop_entry(error))
        return 0;
    int count = 0;
    std::set<std::string> done;
    for (const std::string& ext : extensions)
    {
        if (!done.insert(ext).second)
            continue;
        if (is_video(ext) ? !videos : (ext == "pdf" && !documents))
            continue;
        const std::wstring key = L"Software\\Classes\\." + wide(ext) + L"\\OpenWithProgids";
        HKEY h = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) == ERROR_SUCCESS)
        {
            if (RegSetValueExW(h, kProgId, 0, REG_NONE, nullptr, 0) == ERROR_SUCCESS)
                ++count;
            RegCloseKey(h);
        }
        set_value(L"Software\\Classes\\Applications\\ifritview.exe\\SupportedTypes", wide("." + ext).c_str(), L"", error);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    // The choice itself is the user's: open Settings > Default apps.
    ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL);
    return count;
}
