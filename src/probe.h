#pragma once

// --probe: decode each file and report what was found, no window. Returns
// the process exit code (1 if any failed).
int probe(int argc, char** argv);

// --convert in out [key=value...]: decode `in` and save it as `out`, in the
// format its extension names, with that format's save settings (as the
// Save As dialog offers them, e.g. format=3 dither=0). No window. It's
// --process in save:out,key=value,... by another name.
int convert(int argc, char** argv);
