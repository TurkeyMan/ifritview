#include "integration.h"

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <set>

namespace
{

const char* kDesktopId = "ifritview.desktop";

std::string executable_path()
{
    gchar* exe = g_file_read_link("/proc/self/exe", nullptr);
    std::string path = exe ? exe : "ifritview";
    g_free(exe);
    return path;
}

bool copy_file(const std::string& from, const std::string& to)
{
    gchar* data = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(from.c_str(), &data, &len, nullptr))
        return false;
    const bool ok = g_file_set_contents(to.c_str(), data, gssize(len), nullptr);
    g_free(data);
    return ok;
}

}  // namespace

bool ensure_desktop_entry(std::string& error)
{
    if (GDesktopAppInfo* existing = g_desktop_app_info_new(kDesktopId))
    {
        g_object_unref(existing);
        return true;
    }
    // Not installed: write a per-user entry that launches this very binary,
    // with the icon beside it so the entry shows properly in menus.
    gchar* apps_dir = g_build_filename(g_get_user_data_dir(), "applications", nullptr);
    gchar* icon_dir = g_build_filename(g_get_user_data_dir(), "icons", "hicolor", "scalable", "apps", nullptr);
    g_mkdir_with_parents(apps_dir, 0755);
    g_mkdir_with_parents(icon_dir, 0755);
    gchar* icon_path = g_build_filename(icon_dir, "ifritview.svg", nullptr);
    copy_file(IFRIT_SOURCE_DIR "/data/ifritview.svg", icon_path);

    GKeyFile* kf = g_key_file_new();
    const std::string source = IFRIT_SOURCE_DIR "/data/ifritview.desktop";
    g_key_file_load_from_file(kf, source.c_str(), G_KEY_FILE_NONE, nullptr);
    g_key_file_set_string(kf, "Desktop Entry", "Type", "Application");
    g_key_file_set_string(kf, "Desktop Entry", "Name", "IfritView");
    g_key_file_set_string(kf, "Desktop Entry", "Exec", (executable_path() + " %f").c_str());
    g_key_file_set_string(kf, "Desktop Entry", "Icon", g_file_test(icon_path, G_FILE_TEST_EXISTS) ? icon_path : "image-x-generic");
    gchar* target = g_build_filename(apps_dir, kDesktopId, nullptr);
    GError* err = nullptr;
    const bool ok = g_key_file_save_to_file(kf, target, &err);
    if (!ok)
    {
        error = err ? err->message : "could not write desktop entry";
        if (err)
            g_error_free(err);
    }
    g_key_file_free(kf);
    g_free(target);
    g_free(icon_path);
    g_free(icon_dir);
    g_free(apps_dir);
    if (!ok)
        return false;

    // Let GIO notice the new file; update-desktop-database is optional.
    gchar* argv[] = {const_cast<gchar*>("update-desktop-database"), const_cast<gchar*>("-q"), nullptr};
    g_spawn_sync(nullptr, argv, nullptr, GSpawnFlags(G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL), nullptr, nullptr, nullptr,
                 nullptr, nullptr, nullptr);
    return true;
}

int set_default_for_extensions(const std::vector<std::string>& extensions, bool images_only, std::vector<std::string>& skipped, std::string& error)
{
    if (!ensure_desktop_entry(error))
        return 0;
    GDesktopAppInfo* app = g_desktop_app_info_new(kDesktopId);
    if (!app)
    {
        error = "desktop entry not found even after creating it";
        return 0;
    }
    std::set<std::string> done;
    int count = 0;
    for (const std::string& ext : extensions)
    {
        gboolean uncertain = FALSE;
        gchar* type = g_content_type_guess(("x." + ext).c_str(), nullptr, 0, &uncertain);
        std::string mime = type ? type : "";
        g_free(type);
        if (mime.empty() || mime == "application/octet-stream" || g_content_type_is_unknown(mime.c_str()))
        {
            skipped.push_back(ext);
            continue;
        }
        if (images_only && mime.rfind("image/", 0) != 0)
            continue;
        if (!done.insert(mime).second)
            continue;
        GError* err = nullptr;
        if (g_app_info_set_as_default_for_type(G_APP_INFO(app), mime.c_str(), &err))
        {
            ++count;
        }
        else
        {
            if (error.empty() && err)
                error = err->message;
            if (err)
                g_error_free(err);
        }
    }
    g_object_unref(app);
    return count;
}
