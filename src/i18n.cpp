#include "i18n.h"

#include "config.h"

#include <rapport/sys.h>

#include <glib.h>

#include <system_error>

std::filesystem::path translations_folder(std::string_view domain)
{
    namespace fs = std::filesystem;
    std::vector<fs::path> candidates;
    const fs::path exe_dir = rap::sys::executable_path().parent_path();
    candidates.push_back(exe_dir / "i18n");  // Windows, installed or not
#ifdef IFRIT_DATA_DIR
    candidates.push_back(fs::path(IFRIT_DATA_DIR) / "i18n");
#endif
#ifdef IFRIT_SOURCE_DIR
    // A build: IfritView's beside the sources, rapport's in its submodule.
    candidates.push_back(domain == "rapport" ? fs::path(IFRIT_SOURCE_DIR) / "third_party" / "rapport" / "i18n"
                                             : fs::path(IFRIT_SOURCE_DIR) / "i18n");
#endif
    const std::string prefix = std::string(domain) + ".";
    std::error_code ec;
    for (const fs::path& dir : candidates)
        for (const fs::directory_entry& e : fs::directory_iterator(dir, ec))
        {
            const std::string name = rap::sys::path_to_utf8(e.path().filename());
            if (name.starts_with(prefix) && name.ends_with(".po"))
                return dir;
        }
    return {};
}

std::string saved_language()
{
    std::string tag;
    GKeyFile* kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, Config::path().c_str(), G_KEY_FILE_NONE, nullptr))
        if (gchar* s = g_key_file_get_string(kf, Config::kInterfaceGroup, "language", nullptr))
        {
            tag = s;
            g_free(s);
        }
    g_key_file_free(kf);
    return tag;
}

void start_translations(const std::string& chosen)
{
    // A choice is passed on to the platform too (the toolkit's own dialogs);
    // the system's languages it knows already.
    if (chosen.empty())
        rap::i18n::set_languages(rap::sys::ui_languages());
    else
        rap::i18n::set_languages({chosen}, true);
    for (const char* domain : {"rapport", "ifritview"})
        if (const std::filesystem::path dir = translations_folder(domain); !dir.empty())
            rap::i18n::load(domain, dir);
}
