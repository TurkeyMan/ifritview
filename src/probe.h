#pragma once

// --probe: decode each file and report what was found, no window. Returns
// the process exit code (1 if any failed).
int probe(int argc, char** argv);
