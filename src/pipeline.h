#pragma once
// The command line's image work, no window: a file read, steps done to it in
// order (turns, flips, crops, exposure), and saves along the way, with the
// same decoders, encoders, metadata handling and lossless JPEG transform as
// the app.

// --process IN STEP...: see process_help(). The process exit code: 0 done,
// 1 a file couldn't be read or written, 2 the command was wrong.
int process(int argc, char** argv);

// --formats: what opens, and what saves with which settings.
int formats_report();

// The full --help text: every command, the steps, settings and metadata
// choices, with examples (written to be learnt from).
const char* help_text();
