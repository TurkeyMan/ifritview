#!/bin/sh
# Regenerates i18n/ifritview.pot, the template translators start from, from
# the strings marked in the sources: _() translated where it is, N_() marked
# and translated where it's shown, C_() with a context, P_() with a plural.
# Needs gettext's xgettext. Then, for each translation:
#   msgmerge -U i18n/ifritview.<tag>.po i18n/ifritview.pot
# rapport's own strings are in third_party/rapport/i18n.
set -e
cd "$(dirname "$0")/.."
find src -name '*.cpp' -o -name '*.h' | sort | xgettext --from-code=UTF-8 \
    --language=C++ --files-from=- --output=i18n/ifritview.pot \
    --package-name=IfritView --msgid-bugs-address=https://github.com/TurkeyMan/ifritview/issues \
    --add-comments=TRANSLATORS: --sort-by-file \
    --keyword= --keyword=_ --keyword=N_ --keyword=C_:1c,2 --keyword=P_:1,2
