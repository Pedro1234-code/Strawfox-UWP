// GeckoRuntimeHost.h — starts the Gecko runtime and keeps it off the UI thread.
#pragma once

#include <string>

namespace gecko_w10m::engine {

// Starts Gecko on a thread of its own and returns immediately. XRE_main owns
// its thread until shutdown, so it can never share one with XAML.
//
// width/height are the size in physical pixels of the area the shell can show.
// Gecko is headless here, so this is the only thing that decides how big the
// window it opens will be. Zero leaves Gecko's own default alone.
bool StartGeckoRuntime(const std::wstring& localStatePath, int width,
                       int height, double scale);

}  // namespace gecko_w10m::engine
