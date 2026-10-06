// App.cpp — UWP application entry point (C++/WinRT, no XAML markup compiler).
#include "pch.h"

#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Windows.Storage.h>
#include <appmodel.h>

#include <thread>
#include <vector>
#include "MainPage.h"
#include "client/Log.h"

using namespace winrt;
using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::ApplicationModel::Activation;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::UI::Xaml;

namespace gecko_w10m {

// A code-only C++/WinRT XAML app must still implement IXamlMetadataProvider; the
// framework QIs the Application for it during initialization. We author no
// custom XAML types, so the implementations return empties.
struct App : ApplicationT<App, winrt::Windows::UI::Xaml::Markup::IXamlMetadataProvider> {
  std::shared_ptr<MainPage> page_;

  App() {
    // The log first, before anything that could throw. On Windows 10 Mobile
    // 1511 the app closed at once and left no trace: the constructor below
    // asked for an event this OS does not have, the exception had nobody to
    // catch it, and the first log line was still a page away in MainPage.
    // Every step of the constructor is a line now, and each optional API is
    // its own try.
    try {
      client::Log::Init(std::wstring(
          winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()));
      client::Log::Write(L"boot: App constructor entered");
    } catch (...) {
    }
    // Warm the disk cache with the engine before the loader asks for it.
    // A cold launch on a phone was sixty seconds before the first chrome
    // manifest was even read, against two seconds warm: the loader brings a
    // hundred and forty megabytes of xul.dll in one page fault at a time,
    // and on eMMC a random four-kilobyte read costs what a sequential
    // megabyte does. Streaming the file once, in order, while the splash is
    // up puts it in the standby cache, and the faults then cost nothing.
    std::thread([] {
      try {
        const std::wstring root(Package::Current().InstalledLocation().Path());
        const wchar_t* names[] = {L"/xul.dll", L"/omni.ja",
                                  L"/browser/omni.ja", L"/gkcodecs.dll",
                                  L"/nss3.dll", L"/libGLESv2.dll"};
        const ULONGLONG started = ::GetTickCount64();
        unsigned long long total = 0;
        std::vector<char> buffer(1 << 20);
        for (const wchar_t* name : names) {
          CREATEFILE2_EXTENDED_PARAMETERS params{};
          params.dwSize = sizeof(params);
          params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
          params.dwFileFlags = FILE_FLAG_SEQUENTIAL_SCAN;
          HANDLE file = ::CreateFile2((root + name).c_str(), GENERIC_READ,
                                      FILE_SHARE_READ, OPEN_EXISTING, &params);
          if (file == INVALID_HANDLE_VALUE) {
            continue;
          }
          DWORD got = 0;
          while (::ReadFile(file, buffer.data(),
                            static_cast<DWORD>(buffer.size()), &got, nullptr) &&
                 got) {
            total += got;
          }
          ::CloseHandle(file);
        }
        client::Log::Write(L"prefetch: streamed " +
                           std::to_wstring(total / (1024 * 1024)) +
                           L" MB of the engine in " +
                           std::to_wstring(::GetTickCount64() - started) +
                           L" ms");
      } catch (...) {
        client::Log::Write(L"prefetch: could not read the package");
      }
    }).detach();
    // A XAML app does not die of a Win32 exception; it dies when an error
    // reaches the framework with nobody to answer for it. Those two events are
    // the only place that is visible, and neither leaves a trace in the Win32
    // handlers -- which is why an entire crash could look like a fault in
    // CoreUIComponents that the process then happily survived.
    try {
    UnhandledException([](auto const&, UnhandledExceptionEventArgs const& e) {
      client::Log::WriteFromFault(
          L"RECOVERED: XAML unhandled exception: " + std::wstring(e.Message()));
      client::Log::FlushFromFault();
      // XAML reports some unsupported/temporarily unavailable UWP operations
      // as E_FAIL after the browser chrome is already running. Letting those
      // escape tears down a healthy Gecko process; the affected operation has
      // already failed, so keep the shell alive and record it instead.
      e.Handled(true);
    });

    } catch (winrt::hresult_error const& e) {
      client::Log::Write(L"boot: UnhandledException unavailable", std::wstring(e.message()));
    }
    try {
    CoreApplication::UnhandledErrorDetected(
        [](auto const&, UnhandledErrorDetectedEventArgs const& e) {
          // Propagating it is what turns the error back into an exception this
          // process can report on; without it the framework reports nothing.
          try {
            e.UnhandledError().Propagate();
          } catch (winrt::hresult_error const& error) {
            client::Log::WriteFromFault(
                L"RECOVERED: WinRT error " +
                    std::to_wstring(static_cast<uint32_t>(error.code())) +
                    L": " + std::wstring(error.message()));
            client::Log::FlushFromFault();
          } catch (...) {
            client::Log::WriteFromFault(L"RECOVERED: WinRT error, no detail");
            client::Log::FlushFromFault();
          }
        });

    // The system gives a suspending app a few seconds before it is parked,
    // and on this platform parked is as far as most apps get before they are
    // ended -- there is no orderly shutdown after this, ever. So the seconds
    // are used: the engine is told, chrome script flushes what Firefox
    // normally saves on the shutdown that never comes here, and the deferral
    // is held for as long as the system allows before it is completed.
    } catch (winrt::hresult_error const& e) {
      client::Log::Write(L"boot: UnhandledErrorDetected unavailable", std::wstring(e.message()));
    }
    client::Log::Write(L"boot: error handlers installed");
    try {
    Suspending([](auto const&, SuspendingEventArgs const& args) {
      client::Log::Write(L"app: suspending -- telling the engine, holding "
                         L"the deferral");
      client::Log::FlushFromFault();
      auto deferral = args.SuspendingOperation().GetDeferral();
      std::thread([deferral] {
        if (HMODULE xul = ::GetModuleHandleW(L"xul.dll")) {
          using AppStateFn = void(__cdecl*)(int32_t);
          if (auto fn = reinterpret_cast<AppStateFn>(
                  ::GetProcAddress(xul, "gecko_w10m_app_state"))) {
            fn(0);
          }
        }
        // Time for prefs, the session file and the startup cache to reach
        // the disk. The system's deadline on a phone is about five seconds;
        // half is spent, the rest is margin.
        ::Sleep(2000);
        client::Log::Mirror();
        client::Log::Write(L"app: suspend deferral completed");
        client::Log::FlushFromFault();
        deferral.Complete();
      }).detach();
    });

    // Gecko runs a dozen threads of its own and shuts itself down by calling
    // TerminateProcess; there is no orderly way to park that and pick it up
    // again. When the system says the app is going, the process goes with it
    // rather than lingering with the engine still painting into a window
    // nobody will ever see.
    } catch (winrt::hresult_error const& e) {
      client::Log::Write(L"boot: Suspending unavailable", std::wstring(e.message()));
    }
    try {
    CoreApplication::Exiting([](auto const&, auto const&) {
      client::Log::Write(L"app: exiting");
      ::TerminateProcess(::GetCurrentProcess(), 0);
    });
    } catch (winrt::hresult_error const& e) {
      client::Log::Write(L"boot: Exiting unavailable", std::wstring(e.message()));
    }
    try {
    Resuming([](auto const&, auto const&) {
      client::Log::Write(L"app: resuming");
      if (HMODULE xul = ::GetModuleHandleW(L"xul.dll")) {
        using AppStateFn = void(__cdecl*)(int32_t);
        if (auto fn = reinterpret_cast<AppStateFn>(
                ::GetProcAddress(xul, "gecko_w10m_app_state"))) {
          fn(1);
        }
      }
    });

    // The module that hides the window turned out to be the phone's navigation
    // client -- the thing the shell talks to when it switches what is in
    // front. So the shell is switching away from us, and these are the ways an
    // app is told that. None has been on the record.
    } catch (winrt::hresult_error const& e) {
      client::Log::Write(L"boot: Resuming unavailable", std::wstring(e.message()));
    }
    client::Log::Write(L"boot: lifecycle handlers installed");
    // EnteredBackground and LeavingBackground arrived with Windows 10 1607
    // (ICoreApplication2). On 1511 asking for them throws E_NOINTERFACE,
    // which is what closed the app on the Lumia 650 before it drew anything.
    try {
      CoreApplication::EnteredBackground([](auto const&, auto const&) {
        client::Log::Write(L"app: ENTERED the background -- the shell put "
                           L"something else in front");
        client::Log::FlushFromFault();
      });
      CoreApplication::LeavingBackground([](auto const&, auto const&) {
        client::Log::Write(L"app: leaving the background");
      });
    } catch (winrt::hresult_error const& e) {
      client::Log::Write(L"boot: no background events on this OS",
                         std::wstring(e.message()));
    }
    client::Log::Write(L"boot: App constructor done");
    client::Log::FlushFromFault();
  }

  void OnLaunched(LaunchActivatedEventArgs const& args) {
    // The system splash: the shell's own full-screen surface with the package
    // image, held up until the app is ready. If it is still up thirty seconds
    // in, the window that reports itself visible has never actually been on
    // screen -- and its dismissal is a navigation, done by the very module
    // whose assertion this has been.
    try {
      client::Log::Write(L"splash: previous execution state",
                         std::to_wstring(static_cast<int>(args.PreviousExecutionState())));
      auto splash = args.SplashScreen();
      if (splash) {
        auto where = splash.ImageLocation();
        client::Log::Write(L"splash: the system splash is up, image at " +
                           std::to_wstring(static_cast<int>(where.X)) + L"," +
                           std::to_wstring(static_cast<int>(where.Y)) + L" " +
                           std::to_wstring(static_cast<int>(where.Width)) + L"x" +
                           std::to_wstring(static_cast<int>(where.Height)));
        splash.Dismissed([](auto const&, auto const&) {
          client::Log::Write(L"splash: the system splash was DISMISSED");
          client::Log::FlushFromFault();
        });
      } else {
        client::Log::Write(L"splash: no system splash object");
      }
    } catch (winrt::hresult_error const& error) {
      client::Log::Write(L"splash: could not ask about it",
                         std::wstring(error.message()));
    }
    EnsureContent();
    Window::Current().Activate();
    page_->EnableMouse();
  }

  void OnActivated(IActivatedEventArgs const& args) {
    // This is how the phone hands a browser a link: it activates the app that
    // is registered for http and https with the URL. Without this the app
    // merely opened, which is what "default browser" looked like before.
    std::wstring url;
    try {
      const auto kind = args.Kind();
      client::Log::Write(L"activation: kind " +
                         std::to_wstring(static_cast<int>(kind)));
      if (kind == ActivationKind::Protocol) {
        auto protocolArgs = args.as<ProtocolActivatedEventArgs>();
        if (auto uri = protocolArgs.Uri()) {
          url = uri.AbsoluteUri();
        }
      } else if (kind == ActivationKind::File) {
        auto fileArgs = args.as<FileActivatedEventArgs>();
        auto files = fileArgs.Files();
        if (files && files.Size() > 0) {
          if (auto file =
                  files.GetAt(0)
                      .try_as<winrt::Windows::Storage::IStorageItem>()) {
            // A local file is a URL like any other once it has a scheme.
            std::wstring path(file.Path());
            for (auto& ch : path) {
              if (ch == L'\\') ch = L'/';
            }
            url = L"file:///" + path;
          }
        }
      }
    } catch (winrt::hresult_error const& error) {
      client::Log::Write(L"activation: could not read it",
                         std::wstring(error.message()));
    }

    EnsureContent();
    if (!url.empty() && page_) {
      page_->OpenExternalUrl(url);
    }
    Window::Current().Activate();
    page_->EnableMouse();
  }

  void EnsureContent() {
    if (!page_) {
      page_ = std::make_shared<MainPage>();
    }
    if (!Window::Current().Content()) {
      Window::Current().Content(page_->Root());
    }
  }

  // IXamlMetadataProvider — empty (no custom XAML types).
  winrt::Windows::UI::Xaml::Markup::IXamlType GetXamlType(
      winrt::Windows::UI::Xaml::Interop::TypeName const&) {
    return nullptr;
  }
  winrt::Windows::UI::Xaml::Markup::IXamlType GetXamlType(
      winrt::hstring const&) {
    return nullptr;
  }
  winrt::com_array<winrt::Windows::UI::Xaml::Markup::XmlnsDefinition>
  GetXmlnsDefinitions() {
    return {};
  }
};

}  // namespace gecko_w10m

// The earliest possible mark, with nothing but Win32: a file in the app's
// TempState (what GetTempPath names inside the container), visible in the
// Device Portal's file explorer. A phone that has this file but no
// LocalState log died between here and the App constructor -- in XAML. A
// phone with neither died before wWinMain: the loader, the CRT, or a DLL's
// DllMain.
static void BootMark(const wchar_t* what) {
  // Not GetTempPath: inside the container that is AC\Temp, which the
  // Device Portal never shows. LocalState is
  // %LOCALAPPDATA%\Packages\<family>\LocalState, and both halves come
  // from Win32 alone.
  wchar_t local[MAX_PATH] = {};
  wchar_t family[128] = {};
  UINT32 familyLen = 128;
  if (!::GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) ||
      ::GetCurrentPackageFamilyName(&familyLen, family) != ERROR_SUCCESS) {
    return;
  }
  // Inside the container LOCALAPPDATA is the package's own ...\AC
  // directory; LocalState is its sibling.
  std::wstring base(local);
  if (base.size() > 3 && _wcsicmp(base.c_str() + base.size() - 3, L"\\AC") == 0) {
    base.resize(base.size() - 3);
  } else {
    base += L"\\Packages\\";
    base += family;
  }
  std::wstring file = base + L"\\LocalState\\gecko-boot.txt";
  CREATEFILE2_EXTENDED_PARAMETERS params{};
  params.dwSize = sizeof(params);
  params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
  HANDLE h = ::CreateFile2(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           OPEN_ALWAYS, &params);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  std::wstring line = std::wstring(what) + L"\r\n";
  DWORD written = 0;
  ::WriteFile(h, line.c_str(), static_cast<DWORD>(line.size() * sizeof(wchar_t)),
              &written, nullptr);
  ::FlushFileBuffers(h);
  ::CloseHandle(h);
}

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  BootMark(L"wWinMain reached");
  // Entry thread must be MTA (cf. C++/CX's [Platform::MTAThread] main).
  // Application::Start creates the ASTA UI thread itself; an uninitialized or
  // STA/ASTA entry thread makes CoreApplication throw RPC_E_WRONG_THREAD.
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  BootMark(L"apartment initialised");
  Application::Start([](auto&&) {
    BootMark(L"XAML started, making the App");
    winrt::make<gecko_w10m::App>();
    BootMark(L"App made");
  });
  return 0;
}
