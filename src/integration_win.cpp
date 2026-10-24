// Windows: register IfritView as a handler for the listed extensions, per
// user (HKCU, no elevation). Windows 10 and later don't let an application
// make itself the default; it can only offer itself, so this registers a
// ProgID, adds it to each extension's "Open with" list, registers the app's
// capabilities (which gives it its own page in Default apps, listing its
// types, with a "Set default" for all of them on Windows 11), and opens that
// page, where the user makes the choice.
#include "integration.h"

#include "platform.h"

#include <glib.h>
#include <windows.h>
#include <shellapi.h>  // ShellExecuteW (not in windows.h when it's lean)
#include <shlobj.h>

#include <set>

namespace
{

const wchar_t* kProgId = L"IfritView.Image";
const wchar_t* kCapabilities = L"Software\\IfritView\\Capabilities";

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

void refresh_registration()
{
    // The ProgID's command names the executable that registered.
    const std::wstring key = std::wstring(L"Software\\Classes\\") + kProgId + L"\\shell\\open\\command";
    wchar_t value[2048];
    DWORD size = sizeof value;
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS)
        return;  // never registered
    const std::wstring expected = L"\"" + platform::widen(platform::executable_path()) + L"\" \"%1\"";
    if (_wcsicmp(value, expected.c_str()) == 0)
        return;
    std::string error;
    ensure_desktop_entry(error);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

bool ensure_desktop_entry(std::string& error)
{
    const std::wstring exe = platform::widen(platform::executable_path());
    const std::wstring base = std::wstring(L"Software\\Classes\\") + kProgId;
    bool ok = set_value(base, nullptr, L"IfritView image", error);
    ok = set_value(base + L"\\DefaultIcon", nullptr, L"\"" + exe + L"\",0", error) && ok;
    ok = set_value(base + L"\\shell\\open\\command", nullptr, L"\"" + exe + L"\" \"%1\"", error) && ok;
    // Listed under "Open with" and in Default apps.
    const std::wstring app = L"Software\\Classes\\Applications\\ifritview.exe";
    ok = set_value(app + L"\\shell\\open\\command", nullptr, L"\"" + exe + L"\" \"%1\"", error) && ok;
    ok = set_value(app, L"FriendlyAppName", L"IfritView", error) && ok;
    // An app of its own in Settings' Default apps.
    const std::wstring caps = kCapabilities;
    ok = set_value(caps, L"ApplicationName", L"IfritView", error) && ok;
    ok = set_value(caps, L"ApplicationDescription", L"A fast image viewer", error) && ok;
    ok = set_value(caps, L"ApplicationIcon", L"\"" + exe + L"\",0", error) && ok;
    ok = set_value(L"Software\\RegisteredApplications", L"IfritView", caps, error) && ok;
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
        const std::wstring key = L"Software\\Classes\\." + platform::widen(ext) + L"\\OpenWithProgids";
        HKEY h = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) == ERROR_SUCCESS)
        {
            if (RegSetValueExW(h, kProgId, 0, REG_NONE, nullptr, 0) == ERROR_SUCCESS)
                ++count;
            RegCloseKey(h);
        }
        set_value(L"Software\\Classes\\Applications\\ifritview.exe\\SupportedTypes", platform::widen("." + ext).c_str(), L"", error);
        set_value(std::wstring(kCapabilities) + L"\\FileAssociations", platform::widen("." + ext).c_str(), kProgId, error);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    // The choice itself is the user's: Settings' Default apps, at IfritView's
    // own page (Windows 11; older ones open the list of apps).
    ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps?registeredAppUser=IfritView", nullptr, nullptr, SW_SHOWNORMAL);
    return count;
}
