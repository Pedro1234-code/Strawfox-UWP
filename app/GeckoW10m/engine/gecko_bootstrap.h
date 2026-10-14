// gecko_bootstrap.h — bringing the real Gecko runtime up, as a C interface.
//
// The implementation has to be compiled by clang-cl: Gecko's mfbt headers use
// clang builtins that cl.exe does not have, and with exceptions off, because
// clang emits no C++ EH on 32-bit ARM Windows. The rest of the shell is the
// other way round -- cl.exe, exceptions on, because C++/WinRT needs both. A
// plain C boundary is what lets the two halves meet.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Where log lines from the Gecko side go. Lines are plain ASCII. Set this
// before calling anything else; without it the diagnostics are lost.
typedef void (*gecko_w10m_gecko_log_fn)(const char* line);
void gecko_w10m_gecko_set_logger(gecko_w10m_gecko_log_fn fn);

// Starts the Gecko runtime and does not return until it shuts down, so call it
// on a thread of its own. XRE_main owns its thread for the life of the
// process.
//
//   installDir  the package's install folder, which is also the GRE directory
//   profileDir  a writable profile directory, normally under LocalState
//
// Returns the XRE_main exit code, or a negative value if the runtime never got
// that far.
/* width/height are the size in physical pixels of the area the shell can
 * show. Gecko has no window of its own here, so this is what its headless
 * screen is told it has; pass 0 to leave Gecko's own default alone. */
int gecko_w10m_gecko_run(const wchar_t* installDir, const wchar_t* profileDir,
                         const wchar_t* downloadDirectory, int width,
                         int height, double scale);

#ifdef __cplusplus
}
#endif
