#pragma once

#include <string>
#include <vector>

// Desktop integration: registering as the default viewer. Everything here
// is per-user (mimeapps.list and ~/.local/share), so no privileges needed.

// Make sure a ifritview.desktop entry is visible to GIO: the installed one
// if present, otherwise a per-user copy pointing at this executable.
bool ensure_desktop_entry(std::string& error);

// Set IfritView as the default handler for the MIME types behind these
// extensions. Returns the number of types set; `skipped` lists extensions
// whose type the system doesn't know.
int set_default_for_extensions(const std::vector<std::string>& extensions, std::vector<std::string>& skipped, std::string& error);
