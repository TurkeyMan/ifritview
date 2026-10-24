#pragma once
// IfritView's text in the user's language: rapport's i18n (gettext .po
// files, read as they ship), the "ifritview" domain beside rapport's own.
//   _("Save as…")             translated here
//   N_("Quality")             marked, kept as written in a table, and
//                             translated where it's shown (tr_ifrit)
//   P_("{0} page", "{0} pages", n)   the plural for n
// i18n/ifritview.pot is made from these marks by tools/update-pot.sh.

#include <rapport/i18n.h>

#include <filesystem>
#include <string>
#include <string_view>

#define _(s) rap::i18n::tr("ifritview", s)
#define N_(s) s
#define C_(context, s) rap::i18n::tr_ctx("ifritview", context, s)
#define P_(one, many, n) rap::i18n::tr_n("ifritview", one, many, n)

// A string marked with N_, translated where it's shown.
inline const char* tr_ifrit(const char* s)
{
    return s && *s ? rap::i18n::tr("ifritview", s) : s;
}

// Before the toolkit starts: the languages to use (`chosen`, a tag such as
// "de", or "" for the system's), and the translations for them, IfritView's
// and rapport's.
void start_translations(const std::string& chosen);

// The language setting saved in the settings file, read on its own and at
// once (the rest of the settings are read on a thread, after the toolkit
// has started, which needs the language before).
std::string saved_language();

// Where `domain`'s translations are: beside the exe on Windows, in the
// data folder once installed, or in the source tree for a build.
std::filesystem::path translations_folder(std::string_view domain);
