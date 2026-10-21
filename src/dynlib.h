#pragma once

// Whether a shared library could be dlopened, judged by its file being in
// the loader's search path (LD_LIBRARY_PATH, then the standard directories).
// Costs a few stats, not a load, so it can gate what the folder lists while
// the library itself is only opened when a file needs it.
bool library_installed(const char* soname);
