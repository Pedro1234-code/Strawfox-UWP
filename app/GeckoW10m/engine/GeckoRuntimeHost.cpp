#include "pch.h"

#include "GeckoRuntimeHost.h"

#include <windows.h>

#include <string>

#include "../client/Log.h"
#include "CrashProbe.h"
#include "gecko_bootstrap.h"

namespace gecko_w10m::engine {
namespace {

using gecko_w10m::client::Log;

std::wstring Widen(const char* s) {
  if (!s) return std::wstring();
  int n = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
  if (n <= 1) return std::wstring();
  std::wstring out(static_cast<size_t>(n - 1), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
  return out;
}

void BridgeLog(const char* line) { Log::Write(L"gecko", Widen(line)); }

std::wstring InstallDirectory() {
  wchar_t buf[MAX_PATH] = {};
  DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return std::wstring();
  std::wstring path(buf, n);
  auto slash = path.find_last_of(L'\\');
  if (slash == std::wstring::npos) return std::wstring();
  path.resize(slash);
  return path;
}

struct ThreadArgs {
  std::wstring installDir;
  std::wstring profileDir;
  int width;
  int height;
  double scale;
};

DWORD WINAPI GeckoThread(LPVOID param) {
  auto* args = static_cast<ThreadArgs*>(param);

  int rc = gecko_w10m_gecko_run(args->installDir.c_str(),
                             args->profileDir.c_str(), args->width,
                             args->height, args->scale);
  Log::WriteNum(L"gecko: runtime exited with", rc);

  delete args;
  return 0;
}

}  // namespace

bool StartGeckoRuntime(const std::wstring& localStatePath, int width,
                       int height, double scale) {
  const std::wstring installDir = InstallDirectory();
  if (installDir.empty()) {
    Log::Write(L"gecko: could not determine the install directory");
    return false;
  }

  const std::wstring profileDir = localStatePath + L"\\profile";
  ::CreateDirectoryW(profileDir.c_str(), nullptr);

  // This UWP application is single-instance. If Windows terminated the last
  // process while it was suspended, Gecko had no orderly shutdown in which to
  // remove its desktop profile markers. Do not let those stale markers turn a
  // normal mobile lifecycle into a permanent startup-crash/safe-mode loop.
  ::DeleteFileW((profileDir + L"\\parent.lock").c_str());
  ::DeleteFileW((profileDir + L"\\.startup-incomplete").c_str());
  // Remove the obsolete host-side launch limiter from older packages. It is
  // never created again.
  ::DeleteFileW((localStatePath + L"\\gecko-attempts.txt").c_str());

  // Desktop Firefox restarts itself in Safe Mode after several interrupted
  // startups. The UWP host cannot perform that desktop-style relaunch, so the
  // restart request would simply close the app on every subsequent launch.
  // Keep crash accounting/logging, but do not enter that unrecoverable loop.
  _putenv_s("MOZ_DISABLE_AUTO_SAFE_MODE", "1");

  // The rest of the probes went in when the shell started; these need the
  // engine loaded, so they wait until now.
  InstallEngineProbes();

  gecko_w10m_gecko_set_logger(&BridgeLog);

  auto* args = new ThreadArgs{installDir, profileDir, width, height, scale};
  // 8 MB, reserved rather than committed. Gecko's main thread does deep work
  // and the executable's default of 1 MB is not what it expects.
  HANDLE thread = ::CreateThread(nullptr, 8 * 1024 * 1024, &GeckoThread, args,
                                 STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  if (!thread) {
    Log::WriteNum(L"gecko: CreateThread failed, err", ::GetLastError());
    delete args;
    return false;
  }
  // The sampler takes the handle: it is the only thing that will know where
  // the thread was if the process goes without a word. It suspends the UI
  // thread every millisecond once the engine is up, so only on request.
  if (Log::Verbose()) {
    StartLastLocationSampler(thread);
  } else {
    ::CloseHandle(thread);
  }
  return true;
}

}  // namespace gecko_w10m::engine
