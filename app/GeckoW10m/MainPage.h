// MainPage.h — the browser shell UI, built programmatically (no XAML markup).
// Maps the essential surface of the iOS BrowserViewController: address bar,
// back / forward / reload, progress, tab strip, and the engine content host.
#pragma once

#include <memory>
#include "client/BrowserPreferences.h"
#include "client/EngineView.h"
#include "client/TabManager.h"
#include "engine/GeckoEngine.h"

namespace gecko_w10m {

class MainPage {
 public:
  MainPage();

  // The root visual to set as Window content.
  winrt::Windows::UI::Xaml::UIElement Root() const { return root_; }

  // A URL from outside the app: another app launching us for a link, or this
  // being the system's browser. Opens in the running browser as a new tab.
  void OpenExternalUrl(std::wstring_view url);
  void EnableMouse();

 private:
  void BuildUi();
  void WireEngine();
  void WireLog();

  // Windows 10 Mobile draws the status bar and the (retractable) navigation
  // bar over the app window. Keep the chrome inside the visible area.
  void ApplyVisibleBounds();
  // Tells the engine how dense the display is, so APZ measures a finger
  // against this screen rather than a 96 dpi monitor.
  void PushDpi();

  void Navigate(std::wstring_view entry);
  void RefreshChrome();
  void RebuildTabStrip();
  void AppendLogLine(std::wstring line);

  // Engine
  std::shared_ptr<engine::Runtime> runtime_;
  std::unique_ptr<client::TabManager> tabManager_;
  std::unique_ptr<client::EngineView> engineView_;

  // Views
  winrt::Windows::UI::Xaml::Controls::Grid root_{nullptr};
  winrt::Windows::UI::Xaml::Controls::TextBox addressBar_{nullptr};
  winrt::Windows::UI::Xaml::Controls::Button backButton_{nullptr};
  winrt::Windows::UI::Xaml::Controls::Button forwardButton_{nullptr};
  winrt::Windows::UI::Xaml::Controls::Button reloadButton_{nullptr};
  winrt::Windows::UI::Xaml::Controls::Button newTabButton_{nullptr};
  winrt::Windows::UI::Xaml::Controls::Button logButton_{nullptr};
  winrt::Windows::UI::Xaml::Controls::ProgressBar progress_{nullptr};
  winrt::Windows::UI::Xaml::Controls::Border contentHost_{nullptr};
  // Shown until the engine has drawn something. Starting Gecko takes the
  // better part of half a minute on this hardware, and a blank rectangle
  // for that long is indistinguishable from a browser that did not start.
  winrt::Windows::UI::Xaml::Controls::Grid splash_{nullptr};
  winrt::Windows::UI::Xaml::Controls::TextBlock statusText_{nullptr};
  winrt::Windows::UI::Xaml::Controls::StackPanel tabStrip_{nullptr};

  // Diagnostics overlay: on a phone this is usually the only way to read
  // what the engine did.
  winrt::Windows::UI::Xaml::Controls::Border logPanel_{nullptr};
  winrt::Windows::UI::Xaml::Controls::TextBlock logText_{nullptr};
  winrt::Windows::UI::Xaml::Controls::ScrollViewer logScroller_{nullptr};
};

}  // namespace gecko_w10m
