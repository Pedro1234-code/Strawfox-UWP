// GeckoEngine.h — C++/WinRT-friendly wrapper over the flat gecko_capi ABI.
// Mirrors the iOS GeckoRuntime / GeckoSession Swift types.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include "gecko_capi.h"

namespace gecko_w10m::engine {

// Callbacks the shell subscribes to. std::function so a XAML page can capture
// its dispatcher and marshal back to the UI thread.
struct SessionObserver {
  std::function<void(std::wstring)> LocationChanged;
  std::function<void(std::wstring)> TitleChanged;
  std::function<void(float)>        ProgressChanged;
  std::function<void(bool)>         CanGoBackChanged;
  std::function<void(bool)>         CanGoForwardChanged;
};

class Session {
 public:
  Session(gecko_session* raw);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  void LoadUri(std::wstring_view uri);
  void Reload();
  void Stop();
  void GoBack();
  void GoForward();
  bool CanGoBack() const;
  bool CanGoForward() const;

  // pointerType: IDXGISwapChain*. Gecko composites into it.
  void SetSurface(void* swapChain, int width, int height, float scale);
  void Resize(int width, int height, float scale);

  void Observe(SessionObserver observer);

  gecko_session* raw() const { return raw_; }

 private:
  gecko_session* raw_;
  std::unique_ptr<SessionObserver> observer_;
  gecko_session_delegate delegate_{};
  void InstallDelegate();
};

class Runtime {
 public:
  // profileDir: a real path inside the app's LocalState (e.g. the app's
  // LocalState folder). jitEnabled requires the codeGeneration capability.
  static std::shared_ptr<Runtime> Create(std::wstring_view profileDir,
                                          bool jitEnabled, int dpi);
  ~Runtime();

  std::shared_ptr<Session> CreateSession(bool isPrivate);

  // Call once per UI frame to advance the single-process Gecko event loop.
  int Pump();

  gecko_runtime* raw() const { return raw_; }

 private:
  explicit Runtime(gecko_runtime* raw);
  gecko_runtime* raw_;
};

}  // namespace gecko_w10m::engine
