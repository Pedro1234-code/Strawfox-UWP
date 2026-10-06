// MainPage.cpp
#include "pch.h"
#include "MainPage.h"

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.System.Diagnostics.h>
#include <winrt/Windows.System.Profile.h>

#include <atomic>
#include <thread>

#include "client/Log.h"
#include "client/DownloadBroker.h"
#include "engine/CrashProbe.h"
#include "engine/GeckoRuntimeHost.h"
#include "engine/OverlayProbe.h"
#include "client/SearchEngines.h"

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Input;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace winrt::Windows::UI::Text;
using namespace winrt::Windows::UI::ViewManagement;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Metadata;
using namespace winrt::Windows::Storage;

namespace gecko_w10m {
namespace {

using winrt::Windows::UI::Color;
using winrt::Windows::UI::ColorHelper;

// A deliberately high-contrast scheme. The first build used near-white chrome
// with grey text on a white page, which on a phone screen in daylight is
// simply unreadable.
Color ChromeBg() { return ColorHelper::FromArgb(255, 32, 32, 36); }
Color ChromeFg() { return ColorHelper::FromArgb(255, 245, 245, 245); }
Color ContentBg() { return ColorHelper::FromArgb(255, 255, 255, 255); }
Color ContentFg() { return ColorHelper::FromArgb(255, 24, 24, 28); }
Color Accent() { return ColorHelper::FromArgb(255, 224, 108, 31); }
Color LogBg() { return ColorHelper::FromArgb(242, 16, 16, 18); }
Color LogFg() { return ColorHelper::FromArgb(255, 190, 235, 190); }

SolidColorBrush Brush(Color c) {
  SolidColorBrush b;
  b.Color(c);
  return b;
}

bool IsXbox() {
  static const bool value = [] {
    try {
      return winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo()
                 .DeviceFamily() == L"Windows.Xbox";
    } catch (...) {
      return false;
    }
  }();
  return value;
}

}  // namespace

MainPage::MainPage() {
  using client::BrowserPreferences;
  using client::Log;

  auto localState = ApplicationData::Current().LocalFolder().Path();
  Log::Init(std::wstring(localState));
  Log::Write(L"boot: MainPage constructor entered");
  {
    auto v = winrt::Windows::ApplicationModel::Package::Current().Id().Version();
    std::wstring line = L"shell starting, version " + std::to_wstring(v.Major) +
                        L"." + std::to_wstring(v.Minor) + L"." +
                        std::to_wstring(v.Build) + L"." +
                        std::to_wstring(v.Revision);
    // The engine's own version, read where it is written down: the
    // application.ini the shell hands to XRE_main.
    try {
      const std::wstring ini =
          std::wstring(winrt::Windows::ApplicationModel::Package::Current()
                           .InstalledLocation()
                           .Path()) +
          L"\\browser\\application.ini";
      if (FILE* f = _wfopen(ini.c_str(), L"r")) {
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), f)) {
          if (strncmp(buffer, "Version=", 8) == 0) {
            std::string value(buffer + 8);
            while (!value.empty() &&
                   (value.back() == '\n' || value.back() == '\r')) {
              value.pop_back();
            }
            line += L", Gecko " +
                    std::wstring(value.begin(), value.end());
            break;
          }
        }
        fclose(f);
      }
    } catch (...) {
    }
    Log::Write(line);
  }
  Log::Write(L"LocalState", std::wstring(localState));
  client::DownloadBroker::Initialize(std::wstring(localState));
  Log::Write(Log::Verbose()
                 ? L"logs: verbose -- the engine's logging and the probes are on"
                 : L"logs: quiet (Settings > About > Verbose logs for debugging "
                   L"turns the diagnostics on at the next launch)");
  std::thread([] { Log::Mirror(); }).detach();

  // The number every memory question on this platform comes down to. A phone
  // does not give an app its RAM, it gives it a ceiling set by the device's
  // class, and past it the app is simply gone. Nobody has read the ceiling on
  // any of these phones yet, so it goes in the log first, then every half
  // minute with how much of it is in use, and at once whenever the system
  // says it is moving.
  try {
    using winrt::Windows::System::MemoryManager;
    const auto mb = [](uint64_t bytes) {
      return std::to_wstring(bytes / (1024 * 1024)) + L" MB";
    };
    Log::Write(L"mem: the app may use " + mb(MemoryManager::AppMemoryUsageLimit()) +
               L", using " + mb(MemoryManager::AppMemoryUsage()) + L", level " +
               std::to_wstring(static_cast<int>(MemoryManager::AppMemoryUsageLevel())));
    MemoryManager::AppMemoryUsageLimitChanging([mb](auto&&, auto&& args) {
      Log::Write(L"mem: the limit is changing from " + mb(args.OldLimit()) +
                 L" to " + mb(args.NewLimit()));
    });
    MemoryManager::AppMemoryUsageIncreased([mb](auto&&, auto&&) {
      const auto level = MemoryManager::AppMemoryUsageLevel();
      Log::Write(L"mem: the system says usage went UP a level -- now " +
                 std::to_wstring(static_cast<int>(level)) +
                 L" at " + mb(MemoryManager::AppMemoryUsage()) + L" of " +
                 mb(MemoryManager::AppMemoryUsageLimit()));
      // Low is 0, Medium 1, High 2, OverLimit 3. From Medium on, the engine
      // is told to let go of what it can -- but not every time. On a 1 GB
      // phone Medium is half the limit, the system repeats it several times a
      // second, and each one is a full GC, a cycle collection and a cache
      // purge on a CPU that is already 90% busy: the cure was the illness.
      // Medium goes through once in thirty seconds, High and over once in
      // five.
      static std::atomic<unsigned long long> lastSent{0};
      const unsigned long long now = ::GetTickCount64();
      // Past the limit (OverLimit, 3) the app is about to be ended, so that
      // one always goes through.
      const unsigned long long gap = static_cast<int>(level) >= 3   ? 0ull
                                     : static_cast<int>(level) >= 2 ? 5000ull
                                                                    : 30000ull;
      const unsigned long long last = lastSent.load();
      if (static_cast<int>(level) >= 1 && (!last || now - last >= gap)) {
        lastSent.store(now);
        if (HMODULE xul = ::GetModuleHandleW(L"xul.dll")) {
          using PressureFn = void(__cdecl*)(int32_t);
          if (auto fn = reinterpret_cast<PressureFn>(
                  ::GetProcAddress(xul, "gecko_w10m_memory_pressure"))) {
            fn(static_cast<int32_t>(level));
          }
        }
      }
    });
    MemoryManager::AppMemoryUsageDecreased([mb](auto&&, auto&&) {
      Log::Write(L"mem: usage went down a level -- now " +
                 std::to_wstring(static_cast<int>(MemoryManager::AppMemoryUsageLevel())) +
                 L" at " + mb(MemoryManager::AppMemoryUsage()));
    });
    // Every ten seconds for the first two minutes, then every thirty: the
    // app's own usage, and beside it what the whole phone is doing -- CPU
    // busy since the last sample, memory free. A launch that took seventy
    // seconds with the engine thread barely using any CPU was waiting on
    // something outside the process, and this is the only view of outside
    // an app gets.
    std::thread([mb] {
      using namespace winrt::Windows::System::Diagnostics;
      const ULONGLONG started = ::GetTickCount64();
      long long lastBusy = 0, lastIdle = 0;
      unsigned samples = 0;
      while (true) {
        ::Sleep(::GetTickCount64() - started < 120000 ? 10000 : 30000);
        try {
          std::wstring line = L"mem: using " + mb(MemoryManager::AppMemoryUsage()) +
                              L" of " + mb(MemoryManager::AppMemoryUsageLimit()) +
                              L", level " +
                              std::to_wstring(static_cast<int>(MemoryManager::AppMemoryUsageLevel()));
          try {
            auto sys = SystemDiagnosticInfo::GetForCurrentSystem();
            auto cpu = sys.CpuUsage().GetReport();
            const long long busy = cpu.KernelTime().count() + cpu.UserTime().count();
            const long long idle = cpu.IdleTime().count();
            if (lastBusy || lastIdle) {
              const long long dBusy = busy - lastBusy, dIdle = idle - lastIdle;
              if (dBusy + dIdle > 0) {
                line += L"; phone cpu " + std::to_wstring(dBusy * 100 / (dBusy + dIdle)) +
                        L"% busy";
              }
            }
            lastBusy = busy;
            lastIdle = idle;
            auto m = sys.MemoryUsage().GetReport();
            line += L", phone memory " + mb(m.AvailableSizeInBytes()) + L" free of " +
                    mb(m.TotalPhysicalSizeInBytes()) + L", committed " +
                    mb(m.CommittedSizeInBytes());
          } catch (...) {
            line += L"; phone stats unavailable";
          }
          Log::Write(line);
          // Every other sample -- every minute once past the first two --
          // what that memory is made of.
          if (++samples % 2 == 0 && Log::Verbose()) {
            gecko_w10m::engine::LogMemoryMap();
          }
        } catch (...) {
        }
      }
    }).detach();
  } catch (...) {
    Log::Write(L"mem: MemoryManager is not available here");
  }

  // Before the engine, before the UI: a fault reported only once Gecko is
  // running looks like Gecko's fault, and there was no way to tell that from
  // something this device does on every launch.
  engine::InstallProcessProbes(std::wstring(localState));
  engine::StartHeartbeat();
  // The second device stays off now. It answered its three questions -- a
  // device beside XAML's is harmless, so is taking six hundred megabytes from
  // it, and so is pushing work through it beside the compositor -- and leaving
  // it on would put its own textures in the address space that is now the
  // thing being measured.
  if (Log::Verbose()) {
    engine::MakeSecondD3DDevice();  // a witness device: the heartbeat asks it whether the GPU was reset
  }

  // The compositor telling us it has lost its surfaces is the one warning a
  // GPU reset gives an application. If the phone's driver is being knocked
  // over by the engine using D3D11 alongside XAML, this is where it would say
  // so, and it has never been asked.
  Media::CompositionTarget::SurfaceContentsLost([](auto&&, auto&&) {
    client::Log::Write(L"FATAL: the compositor lost its surfaces");
  });

  bool jit = BrowserPreferences::Shared().IsJitEnabled();
  Log::Write(L"jit preference", jit ? L"enabled" : L"disabled");

  runtime_ = engine::Runtime::Create(std::wstring(localState), jit, 96);
  Log::Write(L"runtime", runtime_ ? L"created" : L"FAILED to create");

  tabManager_ = std::make_unique<client::TabManager>(runtime_);

  // Gecko is headless and has no idea how large the phone is, so it has to be
  // told: the size of the window in physical pixels, which is what the buffer
  // the shell reads back will be. XAML measures in view pixels, and on a phone
  // those are nothing like the same thing.
  double raw = 1.0;
  if (ApiInformation::IsPropertyPresent(
          L"Windows.Graphics.Display.DisplayInformation",
          L"RawPixelsPerViewPixel")) {
    raw = winrt::Windows::Graphics::Display::DisplayInformation::
        GetForCurrentView()
            .RawPixelsPerViewPixel();
  }
  auto bounds = Window::Current().Bounds();
  const int pixelWidth = static_cast<int>(bounds.Width * raw + 0.5);
  const int pixelHeight = static_cast<int>(bounds.Height * raw + 0.5);
  Log::Write(L"view: asking the engine for " + std::to_wstring(pixelWidth) +
             L"x" + std::to_wstring(pixelHeight) + L" physical pixels");
  // The ceiling probe asks for surfaces the size of the screen, because that
  // is the size of the ones WebRender asks for.
  engine::SetProbeSurfaceSize(pixelWidth, pixelHeight);

  // The desktop Firefox chrome has a narrowest width it will accept, and it
  // wins: ask for anything narrower and the window comes back at the minimum.
  // Measured at 504 CSS pixels on this build -- the engine was asked for 1440
  // device pixels and returned 1765, which is 504 at 3.5 device pixels per CSS
  // pixel. At the scale Windows reports, a 1440-pixel screen is 411 CSS pixels
  // wide, so the window could not be as narrow as the phone and every frame
  // carried a quarter more columns than the screen can show, rasterised in
  // software and then copied.
  //
  // So the scale is whatever Windows says, or whatever makes the screen wide
  // enough, whichever is smaller. 540 rather than 504 leaves room for rounding
  // and for a locale whose toolbar needs a little more.
  //
  // This is a second scale, not a correction to the first. `raw` says how many
  // physical pixels Windows puts in a view pixel and is what turns a XAML size
  // or a keyboard height into frame pixels; `cssScale` says how big Gecko
  // should draw. Writing the second over the first, which the last build did,
  // makes the shell measure the keyboard in the wrong unit: it reported 669
  // physical pixels covered where the keyboard really takes 878, and the
  // window moved up by too little.
  constexpr double kNarrowestChromeCss = 540.0;
  double cssScale = raw;
  if (pixelWidth > 0 && cssScale > pixelWidth / kNarrowestChromeCss) {
    cssScale = pixelWidth / kNarrowestChromeCss;
    Log::Write(L"view: scale " + std::to_wstring(raw) + L" would leave " +
               std::to_wstring(static_cast<int>(pixelWidth / raw)) +
               L" CSS pixels, too narrow for the chrome; drawing at " +
                std::to_wstring(cssScale));
  }
  // Xbox presents UWP at a TV-oriented logical scale. Keep the framebuffer at
  // the display's native size, but expose more CSS pixels to Firefox so its
  // desktop chrome and pages are not oversized. This is the same 70% desktop
  // scale used by factoryos-10x-shell; only Gecko's CSS scale changes, not
  // pointer coordinates, swap-chain size or the physical DPI used by APZ.
  constexpr double kXboxDesktopScale = 0.70;
  if (IsXbox()) {
    cssScale *= kXboxDesktopScale;
    Log::Write(L"view: Xbox desktop scale 70%, Gecko CSS scale " +
               std::to_wstring(cssScale));
  }
  Log::WriteNum(L"cpu: cores visible to the process",
                static_cast<int>(std::thread::hardware_concurrency()));

  // The alternating no-panel experiment is over: it was there to ask whether
  // the panel itself hid the window, and the answer was no (the hide was a
  // clobbered uniform location in ANGLE, see 0.2.4.6). On the hardware path
  // a launch without a panel has nothing to present and nothing to copy, so
  // the splash never came down -- which is what every second launch did.
  engineView_ = std::make_unique<client::EngineView>(
      pixelWidth, pixelHeight, raw, /*withPanel*/ true,
      /*usePhysicalScreenRoom*/ IsXbox());

  BuildUi();
  WireEngine();
  // The diagnostics controls are not in the visible tree. Forwarding every
  // Gecko/ANGLE log line to XAML still queued one dispatcher callback per line
  // and made an unhandled XAML error recursively log and queue another one.
  // The file log remains available and is the diagnostics surface we use.

  tabManager_->AddTab(/*isPrivate*/ false, L"about:home");
  Log::WriteNum(L"tabs after first AddTab", tabManager_->Count());
  RebuildTabStrip();
  RefreshChrome();

  // The engine only reports anything in response to a load. Without this the
  // shell sat on a static placeholder and never told us whether the runtime,
  // the JIT probe or xul.dll had worked.
  Navigate(L"about:home");

  engineView_->OnFirstFrame([this]() {
    if (splash_) {
      splash_.Visibility(Visibility::Collapsed);
    }
  });
  engineView_->Start();

  // Last, so the window is up and the log is readable before Gecko gets its
  // chance to take the process down with it.
  engine::StartGeckoRuntime(std::wstring(localState), pixelWidth, pixelHeight,
                            cssScale);
}

void MainPage::PushDpi() {
  if (!engineView_) {
    return;
  }
  try {
    auto display =
        winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
    // RawDpiX is the panel's own density and is what a finger is measured
    // against; it reads zero on displays that never reported one, and then the
    // logical DPI -- 96 times the shell's scale -- is the closest thing there
    // is.
    double dpi = display.RawDpiX();
    if (!(dpi > 0)) {
      dpi = display.LogicalDpi();
    }
    engineView_->SetDpi(dpi);
  } catch (winrt::hresult_error const& error) {
    client::Log::Write(L"size: the display would not say how dense it is",
                       std::wstring(error.message()));
  }
}

void MainPage::ApplyVisibleBounds() {
  // The compositor's own state machine, read out of CoreUIComponents itself,
  // turns out to be about activation: state 5 is activated, 6 deactivated, and
  // the assertion that has been ending every run fires because something asks
  // it to deactivate out of a state that forbids it. And what follows in the
  // log is a process that freezes whole -- heartbeat thread included -- which
  // is what a phone does to an app it has decided is no longer in front.
  //
  // So these three are now on the record. None of them has ever been asked.
  //
  // Once, though. This runs again on every VisibleBoundsChanged -- every
  // rotation, and every time the bars come and go -- and each pass used to add
  // another copy of all three handlers.
  static bool handlersInstalled = false;
  if (!handlersInstalled) {
    handlersInstalled = true;
  Window::Current().VisibilityChanged(
      [this](auto&&, auto const& e) {
        if (e.Visible()) {
          client::Log::Write(L"window: visible");
          // Back from the background: the keyboard that was up when the app
          // left is gone, and nobody said so.
          if (engineView_) {
            engineView_->SyncKeyboardMargin();
          }
          return;
        }
        client::Log::Write(L"window: NOT visible any more");
        // Asked once, and only once, because this is the whole question now.
        //
        // In the hardware build nothing in the engine raises anything before
        // this -- no fault, no fail-fast, nothing -- and what follows is the
        // process freezing whole, which is a phone suspending an app it thinks
        // has gone away. So: does it think that because it was told, or
        // because it decided? If asking to be activated brings the window
        // back, the app is still the foreground app and something merely
        // dropped it; if the ask is refused or nothing follows, it has already
        // been taken off the front and the browser is being suspended, not
        // crashed -- which are two entirely different things to fix.
        static bool asked = false;
        if (asked) return;
        asked = true;
        try {
          Window::Current().Activate();
          client::Log::Write(L"window: asked to be activated again");
        } catch (winrt::hresult_error const& error) {
          client::Log::Write(L"window: the ask was refused",
                             std::wstring(error.message()));
        }
      });
  Window::Current().Activated(
      [](auto&&, auto const& e) {
        using winrt::Windows::UI::Core::CoreWindowActivationState;
        const auto which = e.WindowActivationState();
        client::Log::Write(
            which == CoreWindowActivationState::Deactivated
                ? L"window: deactivated"
                : (which == CoreWindowActivationState::PointerActivated
                       ? L"window: activated by a pointer"
                       : L"window: activated"));
      });

  }

  auto view = ApplicationView::GetForCurrentView();
  auto visible = view.VisibleBounds();
  auto window = Window::Current().Bounds();

  // The screen is the whole display. It is what the engine resizes its window
  // to when a page asks for fullscreen, and by then the two bars are gone, so
  // the whole display is exactly what the window gets.
  if (engineView_) {
    engineView_->SetScreen(window.Width, window.Height);
  }

  // In fullscreen, nothing is padded. The navigation bar is an overlay there:
  // it comes back on a swipe, sits over the picture and goes away again, and
  // the visible bounds move under it every time. Padding for that would resize
  // the browser window twice per swipe, and a page that is handed a resize
  // while it is playing fullscreen video rebuilds its player and falls out of
  // fullscreen -- which is the whole reason this is here.
  bool fullscreen = false;
  try {
    fullscreen = view.IsFullScreenMode();
  } catch (winrt::hresult_error const&) {
  }
  // VisibleBounds on Xbox reserves a small TV safe-area inset even though the
  // app is rendered edge-to-edge. Applying it as XAML padding left the Gecko
  // surface at 1913x1076 on a 1920x1080 display, exposing strips at the right
  // and bottom. Xbox should use the full CoreWindow just like fullscreen.
  if (fullscreen || IsXbox()) {
    root_.Padding(ThicknessHelper::FromUniformLength(0));
    return;
  }

  double left = visible.X - window.X;
  double top = visible.Y - window.Y;
  double right = (window.X + window.Width) - (visible.X + visible.Width);
  double bottom = (window.Y + window.Height) - (visible.Y + visible.Height);

  // Negative or absurd values mean the two rectangles are not comparable;
  // padding nothing is better than padding wrongly.
  auto sane = [](double v) { return (v > 0 && v < 400) ? v : 0.0; };
  root_.Padding(ThicknessHelper::FromLengths(sane(left), sane(top), sane(right),
                                             sane(bottom)));
}

void MainPage::BuildUi() {
  using client::BrowserPreferences;
  using client::ChromePosition;
  using client::Log;

  root_ = Grid();
  root_.Background(Brush(ChromeBg()));

  auto rTop = RowDefinition();
  rTop.Height(GridLengthHelper::Auto());
  auto rMid = RowDefinition();
  rMid.Height(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
  auto rBot = RowDefinition();
  rBot.Height(GridLengthHelper::Auto());
  root_.RowDefinitions().Append(rTop);
  root_.RowDefinitions().Append(rMid);
  root_.RowDefinitions().Append(rBot);

  // --- Tab strip (horizontal, scrollable) ---
  auto tabScroller = ScrollViewer();
  tabScroller.HorizontalScrollBarVisibility(ScrollBarVisibility::Auto);
  tabScroller.VerticalScrollBarVisibility(ScrollBarVisibility::Disabled);
  tabScroller.HorizontalScrollMode(ScrollMode::Auto);
  tabScroller.Background(Brush(ChromeBg()));
  tabStrip_ = StackPanel();
  tabStrip_.Orientation(Orientation::Horizontal);
  tabScroller.Content(tabStrip_);

  // --- Content host (engine SwapChainPanel would attach here) ---
  contentHost_ = Border();
  contentHost_.Background(Brush(ContentBg()));
  statusText_ = TextBlock();
  statusText_.Text(L"Gecko");
  statusText_.HorizontalAlignment(HorizontalAlignment::Center);
  statusText_.VerticalAlignment(VerticalAlignment::Center);
  statusText_.TextWrapping(TextWrapping::Wrap);
  statusText_.TextAlignment(TextAlignment::Center);
  statusText_.Margin(ThicknessHelper::FromUniformLength(16));
  statusText_.Foreground(Brush(ContentFg()));
  // The engine's frames go on top of the placeholder, so the text below shows
  // until there is something better to show and never afterwards.
  auto contentStack = Grid();
  contentStack.Children().Append(statusText_);
  // Order matters: the text sink goes underneath the picture. Above it, every
  // tap landed on a text control and Windows raised the keyboard for each one,
  // and nothing reached the engine at all.
  contentStack.Children().Append(engineView_->TextSink());
  // Back in the tree. Out of it the browser died on time, twice, with the
  // swap chain made and handed over and never composited -- so what the
  // compositor does with our swap chain is not what kills this process, and
  // there is no reason left to keep the picture off the screen.
  // The video layer first, so it lies under the browser's panel.
  if (engineView_->VideoPanel()) {
    contentStack.Children().Append(engineView_->VideoPanel());
  }
  if (engineView_->Panel()) {
    contentStack.Children().Append(engineView_->Panel());
  }
  contentStack.Children().Append(engineView_->Surface());

  // The browser's own logo, out of the browser's own package. It is the thing
  // the user is waiting for, so it is the right thing to wait in front of.
  splash_ = Grid();
  splash_.Background(Brush(ContentBg()));
  {
    Image logo;
    logo.Width(128);
    logo.Height(128);
    logo.HorizontalAlignment(HorizontalAlignment::Center);
    logo.VerticalAlignment(VerticalAlignment::Center);
    logo.Source(Media::Imaging::BitmapImage(Uri(
        L"ms-appx:///browser/chrome/browser/content/branding/about-logo@2x.png")));
    splash_.Children().Append(logo);
  }
  contentStack.Children().Append(splash_);
  contentHost_.Child(contentStack);

  // --- Diagnostics overlay, hidden until asked for ---
  logPanel_ = Border();
  logPanel_.Background(Brush(LogBg()));
  logPanel_.Visibility(Visibility::Collapsed);
  logScroller_ = ScrollViewer();
  logScroller_.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
  logText_ = TextBlock();
  logText_.Foreground(Brush(LogFg()));
  logText_.FontFamily(FontFamily(L"Consolas"));
  logText_.FontSize(11);
  logText_.TextWrapping(TextWrapping::Wrap);
  logText_.Margin(ThicknessHelper::FromUniformLength(8));
  logScroller_.Content(logText_);
  logPanel_.Child(logScroller_);

  // --- Chrome (address bar + nav buttons + progress) ---
  auto chrome = Grid();
  chrome.Padding(ThicknessHelper::FromLengths(6, 4, 6, 4));
  chrome.Background(Brush(ChromeBg()));

  progress_ = ProgressBar();
  progress_.Minimum(0);
  progress_.Maximum(1);
  progress_.Value(0);
  progress_.Height(3);
  progress_.Foreground(Brush(Accent()));
  progress_.VerticalAlignment(VerticalAlignment::Top);

  auto row = StackPanel();
  row.Orientation(Orientation::Horizontal);

  auto makeButton = [](std::wstring_view glyph) {
    Button b;
    FontIcon icon;
    icon.Glyph(winrt::hstring(glyph));
    icon.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
    icon.FontSize(16);
    icon.Foreground(Brush(ChromeFg()));
    b.Content(icon);
    b.Margin(ThicknessHelper::FromLengths(1, 0, 1, 0));
    b.Padding(ThicknessHelper::FromLengths(6, 6, 6, 6));
    b.MinWidth(38);
    b.Background(Brush(ColorHelper::FromArgb(255, 52, 52, 58)));
    b.Foreground(Brush(ChromeFg()));
    b.BorderThickness(ThicknessHelper::FromUniformLength(0));
    return b;
  };

  backButton_ = makeButton(L"\uE72B");     // Back
  forwardButton_ = makeButton(L"\uE72A");  // Forward
  reloadButton_ = makeButton(L"\uE72C");   // Refresh
  newTabButton_ = makeButton(L"\uE710");   // Add
  logButton_ = makeButton(L"\uE7BA");      // Warning/diagnostics

  addressBar_ = TextBox();
  addressBar_.PlaceholderText(L"Search or enter address");
  addressBar_.InputScope([] {
    InputScope s;
    InputScopeName n;
    n.NameValue(InputScopeNameValue::Url);
    s.Names().Append(n);
    return s;
  }());
  addressBar_.Margin(ThicknessHelper::FromLengths(4, 0, 4, 0));
  addressBar_.MinWidth(140);
  addressBar_.FontSize(14);

  row.Children().Append(backButton_);
  row.Children().Append(forwardButton_);
  row.Children().Append(reloadButton_);
  row.Children().Append(addressBar_);
  row.Children().Append(newTabButton_);
  row.Children().Append(logButton_);

  auto chromeStack = StackPanel();
  chromeStack.Children().Append(progress_);
  chromeStack.Children().Append(row);
  chrome.Children().Append(chromeStack);

  // --- Placement depends on the address-bar position preference ---
  bool barOnTop =
      BrowserPreferences::Shared().AddressBarPosition() == ChromePosition::Top;

  // Only the engine's own picture goes on screen. Firefox has a tab strip, an
  // address bar and a menu of its own, and now that they are visible and can be
  // touched, a second set underneath them is not a second opinion -- it is two
  // address bars, one of which does nothing. The controls above are still built
  // because the engine facade reports through them; they are simply not shown.
  //
  // The log panel goes with them. It was the only way to read anything off the
  // device before the engine could draw, and that has not been true for a while
  // -- the log is a file, and that is how it has actually been read all along.
  Grid::SetRow(contentHost_, 1);
  root_.Children().Append(contentHost_);

  // The engine's window is the size of this, measured whenever it changes.
  // Not the size of the picture inside it: a stretched Image reports what it
  // drew, which is the frame, which would make the frame decide its own size.
  engineView_->WatchRoom(contentHost_);


  // The status bar exists only on mobile; tint it to match so the chrome does
  // not look like it is floating under a foreign strip.
  if (ApiInformation::IsTypePresent(L"Windows.UI.ViewManagement.StatusBar")) {
    auto status = StatusBar::GetForCurrentView();
    status.BackgroundColor(ChromeBg());
    status.BackgroundOpacity(1.0);
    status.ForegroundColor(ChromeFg());
  }

  // The navigation client carries a property named
  // CoreWindowResizeManager_ShouldWaitForLayoutCompletion, and a phone's
  // resize manager is a protocol: the shell changes the window and waits for
  // the app to say its layout is complete. An app that never says so, at a
  // moment the shell thinks the window changed, is an app the shell stops
  // waiting for. Nothing here has ever reported a size or bounds change, so
  // whether one happens at first paint is unknown. Every one is on the record
  // now, with the numbers.
  {
    auto core = Window::Current().CoreWindow();
    core.SizeChanged([](auto const&, auto const& e) {
      auto size = e.Size();
      client::Log::Write(L"size: CoreWindow.SizeChanged " +
                         std::to_wstring(static_cast<int>(size.Width)) + L"x" +
                         std::to_wstring(static_cast<int>(size.Height)));
      client::Log::FlushFromFault();
    });
    core.ResizeStarted([](auto const&, auto const&) {
      client::Log::Write(L"size: CoreWindow.ResizeStarted -- the shell is "
                         L"waiting for layout to complete");
      client::Log::FlushFromFault();
    });
    core.ResizeCompleted([](auto const&, auto const&) {
      client::Log::Write(L"size: CoreWindow.ResizeCompleted");
      client::Log::FlushFromFault();
    });
    Window::Current().SizeChanged([this](auto const&, auto const& e) {
      if (engineView_) {
        engineView_->SetScreen(e.Size().Width, e.Size().Height);
      }
      auto size = e.Size();
      client::Log::Write(L"size: Window.SizeChanged " +
                         std::to_wstring(static_cast<int>(size.Width)) + L"x" +
                         std::to_wstring(static_cast<int>(size.Height)));
    });
    auto display = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
    display.OrientationChanged([](auto const&, auto const&) {
      client::Log::Write(L"size: DisplayInformation.OrientationChanged");
    });
    display.DpiChanged([this](auto const&, auto const&) {
      client::Log::Write(L"size: DisplayInformation.DpiChanged");
      PushDpi();
    });
  }
  PushDpi();

  // On a phone an unhandled Back is a navigation away from the app: the shell
  // hides the view and suspends it a few seconds later -- which is, step for
  // step, what has been observed. Nobody presses anything, but the strings in
  // the navigation client name Windows.Phone.UI.Input.HardwareButtons and
  // BackPressedEventArgs, and a Back that arrives from nowhere would look
  // exactly like this. So it is logged, and handled, and if the hide stops
  // that is the answer.
  {
    using winrt::Windows::UI::Core::SystemNavigationManager;
    auto navigation = SystemNavigationManager::GetForCurrentView();
    navigation.BackRequested([](auto const&, auto const& e) {
      client::Log::Write(L"back: BackRequested arrived -- handling it so the "
                         L"shell does not navigate away");
      client::Log::FlushFromFault();
      e.Handled(true);
    });
    // HardwareButtons.BackPressed lives in a phone extension SDK this build
    // does not have; BackRequested is the same button by the newer road.
  }

  auto view = ApplicationView::GetForCurrentView();
  // Consolidated is the view being removed from the switcher -- the shell's
  // word for "gone", as opposed to merely behind something.
  view.Consolidated([](auto const&, auto const&) {
    client::Log::Write(L"window: the view was CONSOLIDATED -- removed from "
                       L"the task switcher");
    client::Log::FlushFromFault();
  });
  Window::Current().CoreWindow().Closed([](auto const&, auto const&) {
    client::Log::Write(L"window: the CoreWindow was CLOSED");
    client::Log::FlushFromFault();
  });
  Window::Current().Closed([](auto const&, auto const&) {
    client::Log::Write(L"window: the XAML window was closed");
    client::Log::FlushFromFault();
  });
  // Xbox's UseVisible rectangle is the TV safe area (1913x1076 on a
  // 1920x1080 output). It constrains the entire XAML tree, so clearing the
  // root padding alone cannot recover the missing edge pixels.
  view.SetDesiredBoundsMode(IsXbox() ? ApplicationViewBoundsMode::UseCoreWindow
                                     : ApplicationViewBoundsMode::UseVisible);
  view.VisibleBoundsChanged([this](auto&&, auto&&) {
    auto b = ApplicationView::GetForCurrentView().VisibleBounds();
    client::Log::Write(L"size: ApplicationView.VisibleBoundsChanged " +
                       std::to_wstring(static_cast<int>(b.X)) + L"," +
                       std::to_wstring(static_cast<int>(b.Y)) + L" " +
                       std::to_wstring(static_cast<int>(b.Width)) + L"x" +
                       std::to_wstring(static_cast<int>(b.Height)));
    ApplyVisibleBounds();
  });
  ApplyVisibleBounds();

  // --- Events ---
  backButton_.Click([this](auto&&, auto&&) {
    client::Log::Write(L"ui: back");
    if (auto* t = tabManager_->SelectedTab(); t && t->session)
      t->session->GoBack();
  });
  forwardButton_.Click([this](auto&&, auto&&) {
    client::Log::Write(L"ui: forward");
    if (auto* t = tabManager_->SelectedTab(); t && t->session)
      t->session->GoForward();
  });
  reloadButton_.Click([this](auto&&, auto&&) {
    client::Log::Write(L"ui: reload");
    if (auto* t = tabManager_->SelectedTab(); t && t->session)
      t->session->Reload();
  });
  newTabButton_.Click([this](auto&&, auto&&) {
    client::Log::Write(L"ui: new tab");
    tabManager_->AddTab(false, L"about:home");
    RebuildTabStrip();
    RefreshChrome();
  });
  logButton_.Click([this](auto&&, auto&&) {
    bool showing = logPanel_.Visibility() == Visibility::Visible;
    logPanel_.Visibility(showing ? Visibility::Collapsed : Visibility::Visible);
    if (!showing) {
      std::wstring all;
      for (auto const& line : client::Log::Recent()) {
        all += line;
        all += L"\n";
      }
      all += L"\nlog file: " + client::Log::Path();
      logText_.Text(winrt::hstring(all));
      logScroller_.UpdateLayout();
      logScroller_.ChangeView(nullptr, logScroller_.ScrollableHeight(), nullptr);
    }
  });
  addressBar_.KeyDown([this](auto&&, KeyRoutedEventArgs const& e) {
    if (e.Key() == winrt::Windows::System::VirtualKey::Enter) {
      Navigate(std::wstring(addressBar_.Text()));
    }
  });

  Log::Write(L"ui built");
}

void MainPage::WireEngine() {
  tabManager_->OnChanged([this] { RefreshChrome(); });
}

void MainPage::WireLog() {
  client::Log::OnLine(nullptr);
}

void MainPage::AppendLogLine(std::wstring line) {
  if (logPanel_.Visibility() != Visibility::Visible) return;
  logText_.Text(logText_.Text() + winrt::hstring(line + L"\n"));
  logScroller_.ChangeView(nullptr, logScroller_.ScrollableHeight(), nullptr);
}

void MainPage::OpenExternalUrl(std::wstring_view url) {
  client::Log::Write(L"open: activation with " + std::wstring(url));
  if (engineView_) {
    engineView_->OpenUrl(url);
  }
}

void MainPage::EnableMouse() {
  if (engineView_) engineView_->EnableMouse();
}

void MainPage::Navigate(std::wstring_view entry) {
  using client::Log;

  auto* tab = tabManager_->SelectedTab();
  if (!tab) {
    Log::Write(L"navigate: no selected tab, creating one");
    tabManager_->AddTab(false, L"");
    tab = tabManager_->SelectedTab();
  }
  if (!tab) {
    Log::Write(L"navigate: still no tab, giving up");
    return;
  }

  auto url = client::SearchEngines::ResolveEntry(entry);
  tab->url = url;
  Log::Write(L"navigate", url);

  if (!tab->session) {
    Log::Write(L"navigate: tab has no engine session (runtime create failed?)");
    statusText_.Text(L"No engine session.\nOpen the log for details.");
    RefreshChrome();
    return;
  }

  engine::SessionObserver obs;
  auto dispatcher = root_.Dispatcher();
  obs.LocationChanged = [this, dispatcher](std::wstring u) {
    client::Log::Write(L"engine: location", u);
    dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
                        [this, u] { addressBar_.Text(winrt::hstring(u)); });
  };
  obs.TitleChanged = [this, dispatcher](std::wstring t) {
    client::Log::Write(L"engine: title", t);
    dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
                        [this, t] { statusText_.Text(winrt::hstring(t)); });
  };
  obs.ProgressChanged = [this, dispatcher](float p) {
    dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
                        [this, p] { progress_.Value(p); });
  };
  obs.CanGoBackChanged = [](bool) {};
  obs.CanGoForwardChanged = [](bool) {};
  tab->session->Observe(std::move(obs));
  tab->session->LoadUri(url);
  RefreshChrome();
}

void MainPage::RefreshChrome() {
  auto* tab = tabManager_->SelectedTab();
  if (!tab) {
    addressBar_.Text(L"");
    backButton_.IsEnabled(false);
    forwardButton_.IsEnabled(false);
    return;
  }
  addressBar_.Text(winrt::hstring(tab->url));
  bool back = tab->session && tab->session->CanGoBack();
  bool fwd = tab->session && tab->session->CanGoForward();
  backButton_.IsEnabled(back);
  forwardButton_.IsEnabled(fwd);
}

void MainPage::RebuildTabStrip() {
  tabStrip_.Children().Clear();
  for (int i = 0; i < tabManager_->Count(); ++i) {
    auto* tab = tabManager_->At(i);
    Button b;
    TextBlock label;
    label.Text(winrt::hstring(tab->title.empty() ? L"New Tab" : tab->title));
    label.Foreground(Brush(ChromeFg()));
    label.FontSize(13);
    b.Content(label);
    b.Background(Brush(ColorHelper::FromArgb(
        255, i == tabManager_->SelectedIndex() ? 70 : 44,
        i == tabManager_->SelectedIndex() ? 70 : 44,
        i == tabManager_->SelectedIndex() ? 78 : 50)));
    b.BorderThickness(ThicknessHelper::FromUniformLength(0));
    b.Margin(ThicknessHelper::FromLengths(2, 4, 2, 4));
    b.Padding(ThicknessHelper::FromLengths(10, 4, 10, 4));
    int index = i;
    b.Click([this, index](auto&&, auto&&) {
      tabManager_->SelectTab(index);
      RefreshChrome();
    });
    tabStrip_.Children().Append(b);
  }
}

}  // namespace gecko_w10m
