// gecko_bootstrap.cpp — start the real Gecko runtime.
//
// xul.dll exposes exactly one external interface, mozilla::Bootstrap, obtained
// from the exported XRE_GetBootstrap. Its only full-application entry point is
// XRE_main, which is what firefox.exe itself calls: it reads application.ini,
// sets up the profile, starts XPCOM and runs the event loop until shutdown.
//
// This runs headless. The Windows widget backend creates desktop HWNDs, which
// a packaged app on a phone has no way to present -- it has a CoreWindow, not
// a desktop window station. Headless keeps XPCOM, SpiderMonkey, networking and
// layout in play while leaving windowing out, which is the right first thing
// to prove. Putting pixels on screen comes after, and will mean giving Gecko a
// compositor target backed by a XAML SwapChainPanel rather than an HWND.
//
// Compiled by clang-cl with exceptions off; see gecko_bootstrap.h for why.

#include <cstdlib>
#include "gecko_bootstrap.h"

#include <windows.h>

#include <string>
#include <vector>

#include "mozilla/Bootstrap.h"
#include "mozilla/TimeStamp.h"

// The startup timeline event ids, taken from the same header firefox.exe uses,
// in the list form it offers for exactly this. Reading them from the source
// rather than writing 1 keeps them from drifting away from the enum
// XRE_StartupTimelineRecord expects.
enum GeckoW10mStartupEvent {
#define mozilla_StartupTimeline_Event(ev, name) ev,
#include "StartupTimeline.h"
#undef mozilla_StartupTimeline_Event
};

namespace {

gecko_w10m_gecko_log_fn gLog = nullptr;

void Log(const std::string& line) {
  if (gLog) gLog(line.c_str());
}

// The Gecko side speaks UTF-8 and the Windows side UTF-16; both conversions are
// needed often enough to be worth naming.
std::string Narrow(const std::wstring& w) {
  if (w.empty()) return std::string();
  int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
  return out;
}

// Gecko reports early failures on stderr, and a packaged app has none. Point
// the standard handles at a file so those messages survive.
void RedirectStdErrTo(const std::wstring& path) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  CREATEFILE2_EXTENDED_PARAMETERS params{};
  params.dwSize = sizeof(params);
  params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
  params.lpSecurityAttributes = &sa;

  HANDLE h = ::CreateFile2(path.c_str(), FILE_APPEND_DATA | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS,
                           &params);
  if (h == INVALID_HANDLE_VALUE) {
    Log("stderr redirect failed, err " + std::to_string(::GetLastError()));
    return;
  }
  ::SetFilePointer(h, 0, nullptr, FILE_END);
  ::SetStdHandle(STD_ERROR_HANDLE, h);
  ::SetStdHandle(STD_OUTPUT_HANDLE, h);
}

// The engine reads its environment through the C runtime -- PR_GetEnv is
// getenv, and gfxPlatform::IsHeadless asks it directly -- and ucrtbase copies
// the Win32 environment block once at process start and never looks again.
// SetEnvironmentVariableW updates the block, not the copy, so nothing set that
// way is visible to the engine. Gecko.exe cannot fix that with its own
// putenv either: it links the static runtime, so its environment is a third
// one that xul.dll never reads.
//
// Setting it where the engine looks means calling ucrtbase's own _wputenv_s.
// This is why MOZ_HEADLESS was ignored -- the Windows widget backend was being
// used on a device with no windows -- and why MOZ_LOG_FILE produced nothing.
void SetEngineEnvironment(const wchar_t* name, const wchar_t* value) {
  using PutEnvFn = int(__cdecl*)(const wchar_t*, const wchar_t*);
  static PutEnvFn crtPutEnv = [] {
    HMODULE ucrt = ::GetModuleHandleW(L"ucrtbase.dll");
    return ucrt ? reinterpret_cast<PutEnvFn>(::GetProcAddress(ucrt, "_wputenv_s"))
                : nullptr;
  }();

  ::SetEnvironmentVariableW(name, value);
  // A null value removes the variable through SetEnvironmentVariableW, but
  // _wputenv_s requires a valid string on the Xbox UCRT. Passing nullptr here
  // terminates the process during startup before xul.dll is entered.
  if (crtPutEnv && value != nullptr) {
    crtPutEnv(name, value);
  }
}


// Two files the profile has to carry before Gecko reads it.
//
// Gecko opens its window at Firefox's desktop default -- 1280 by 1040 -- on a
// screen it was told is the whole phone, and leaves the rest blank. sizemode
// is how a window is asked to fill its screen, and xulstore.json is where that
// is remembered; writing it every start is right here because a phone window
// is never any other size.
//
// devPixelsPerPx is the other half. Left alone, Gecko draws one CSS pixel per
// device pixel, which on a 1440-wide phone is a browser rendered for ants. The
// shell already knows the number Windows uses for exactly this, so it passes
// it down and a CSS pixel becomes what Windows means by a pixel.
void WriteProfileFile(const std::wstring& path, const std::string& body) {
  CREATEFILE2_EXTENDED_PARAMETERS params{};
  params.dwSize = sizeof(params);
  params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
  HANDLE file = ::CreateFile2(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              CREATE_ALWAYS, &params);
  if (file == INVALID_HANDLE_VALUE) {
    Log("bootstrap: could not write " + Narrow(path));
    return;
  }
  DWORD written = 0;
  ::WriteFile(file, body.data(), static_cast<DWORD>(body.size()), &written,
              nullptr);
  ::CloseHandle(file);
}

// A brand-new profile gets the caches a previous run captured, if the
// package carries them (seed/ next to the engine; see tools/build-appx.sh).
// Gecko keeps the caches only while the profile's compatibility.ini names
// this exact build and install directory, so the shipped file is rewritten
// with the directories this phone actually installed to. Anything wrong
// with the seed -- a different build, a corrupt file -- makes Gecko purge
// and rebuild it, which is where a new profile started anyway.
void SeedProfileCaches(const std::wstring& install,
                       const std::wstring& profile) {
  const std::wstring compat = profile + L"\\compatibility.ini";
  if (::GetFileAttributesW(compat.c_str()) != INVALID_FILE_ATTRIBUTES) {
    return;  // not a new profile
  }
  const std::wstring seed = install + L"\\seed";
  const std::wstring seedCompat = seed + L"\\compatibility.ini";
  if (::GetFileAttributesW(seedCompat.c_str()) == INVALID_FILE_ATTRIBUTES) {
    Log("seed: the package carries no startup cache");
    return;
  }
  std::string body;
  {
    CREATEFILE2_EXTENDED_PARAMETERS params{};
    params.dwSize = sizeof(params);
    params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    HANDLE f = ::CreateFile2(seedCompat.c_str(), GENERIC_READ, FILE_SHARE_READ,
                             OPEN_EXISTING, &params);
    if (f == INVALID_HANDLE_VALUE) {
      Log("seed: could not read the seed's compatibility.ini");
      return;
    }
    char buf[4096];
    DWORD got = 0;
    while (::ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) {
      body.append(buf, got);
    }
    ::CloseHandle(f);
  }
  // Rewrite the two directory lines for this installation.
  std::string out;
  size_t pos = 0;
  while (pos < body.size()) {
    size_t end = body.find('\n', pos);
    if (end == std::string::npos) end = body.size();
    std::string line = body.substr(pos, end - pos);
    while (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("LastPlatformDir=", 0) == 0) {
      line = "LastPlatformDir=" + Narrow(install);
    } else if (line.rfind("LastAppDir=", 0) == 0) {
      line = "LastAppDir=" + Narrow(install + L"\\browser");
    }
    out += line + "\r\n";
    pos = end + 1;
  }
  const std::wstring cacheDir = profile + L"\\startupCache";
  ::CreateDirectoryW(cacheDir.c_str(), nullptr);
  int copied = 0;
  for (const wchar_t* name :
       {L"scriptCache.bin", L"urlCache.bin", L"startupCache.4.little"}) {
    const std::wstring from = seed + L"\\" + name;
    const std::wstring to = cacheDir + L"\\" + name;
    if (::GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES) {
      continue;
    }
    COPYFILE2_EXTENDED_PARAMETERS cp{};
    cp.dwSize = sizeof(cp);
    if (SUCCEEDED(::CopyFile2(from.c_str(), to.c_str(), &cp))) {
      ++copied;
    } else {
      Log(std::string("seed: could not copy ") + Narrow(name));
    }
  }
  WriteProfileFile(compat, out);
  Log("seed: a new profile, seeded with " + std::to_string(copied) +
      " cache files and a compatibility.ini for " + Narrow(install));
}

void PrepareProfile(const std::wstring& profile, int width, int height,
                    double scale) {
  if (width <= 0 || height <= 0) {
    return;
  }
  const int cssWidth = static_cast<int>(width / (scale > 0 ? scale : 1.0));
  const int cssHeight = static_cast<int>(height / (scale > 0 ? scale : 1.0));

  std::string store =
      "{\"chrome://browser/content/browser.xhtml\":{\"main-window\":{"
      "\"screenX\":\"0\",\"screenY\":\"0\","
      "\"width\":\"" + std::to_string(cssWidth) + "\","
      "\"height\":\"" + std::to_string(cssHeight) + "\","
      "\"sizemode\":\"maximized\"}}}\n";
  WriteProfileFile(profile + L"\\xulstore.json", store);

  std::string scaleText = std::to_string(scale);
  std::wstring downloadPath = profile + L"\\download-staging";
  ::CreateDirectoryW(downloadPath.c_str(), nullptr);
  std::string downloadPathUtf8 = Narrow(downloadPath);
  std::string escapedDownloadPath;
  escapedDownloadPath.reserve(downloadPathUtf8.size() * 2);
  for (char ch : downloadPathUtf8) {
    if (ch == '\\' || ch == '"') escapedDownloadPath.push_back('\\');
    escapedDownloadPath.push_back(ch);
  }
  std::string prefs =
      "// Written by the shell every start; see gecko_bootstrap.cpp.\n"
      "user_pref(\"layout.css.devPixelsPerPx\", \"" + scaleText + "\");\n"
      "// Downloads stay in the app container until DownloadBroker exports "
      "them through the user's FolderPicker permission.\n"
      "user_pref(\"browser.download.folderList\", 2);\n"
      "user_pref(\"browser.download.useDownloadDir\", true);\n"
      "user_pref(\"browser.download.dir\", \"" + escapedDownloadPath + "\");\n"
      "user_pref(\"browser.download.lastDir\", \"" + escapedDownloadPath + "\");\n"
      "// Firefox's desktop sanity test snapshots an offscreen HWND. The "
      "headless UWP compositor and its video overlay are not represented in "
      "that snapshot, so the test produces a false failure on this port.\n"
      "// A second NV12 composition swap chain can remove the D3D device on "
      "Xbox (DXGI_ERROR_DEVICE_REMOVED), leaving audio playing while video "
      "and the browser freeze. Keep video on the main WebRender compositor. "
      "This user preference also protects existing profiles if an engine "
      "build still has the experimental overlay enabled by default.\n"
      "user_pref(\"gfx.webrender.gecko-w10m.video-overlay\", false);\n"
      "user_pref(\"media.sanity-test.disabled\", true);\n"
      "user_pref(\"sanity-test.running\", false);\n"
      "user_pref(\"media.hardware-video-decoding.failed\", false);\n";
  WriteProfileFile(profile + L"\\user.js", prefs);
  Log("bootstrap: profile window " + std::to_string(cssWidth) + "x" +
      std::to_string(cssHeight) + " at " + scaleText + " device pixels per CSS pixel");
}

using GetBootstrapFn = void(NS_FROZENCALL*)(mozilla::Bootstrap::UniquePtr&);

}  // namespace

extern "C" void gecko_w10m_gecko_set_logger(gecko_w10m_gecko_log_fn fn) { gLog = fn; }

extern "C" int gecko_w10m_gecko_run(const wchar_t* installDir,
                                 const wchar_t* profileDir, int width,
                                 int height, double scale) {
  const std::wstring install(installDir ? installDir : L"");
  const std::wstring profile(profileDir ? profileDir : L"");

  // The desktop profile service asks the Windows shell for LocalAppData and
  // RoamingAppData even when -profile supplies the actual browser profile.
  // Those shell-folder lookups are not reliable in the Xbox app container.
  // Point both roots at the package's writable LocalState directory instead.
  // The profile passed by the host is LocalState\profile, so its parent is the
  // data root where profiles.ini and installs.ini may safely live.
  std::wstring dataRoot = profile;
  const size_t separator = dataRoot.find_last_of(L"\\/");
  if (separator != std::wstring::npos) {
    dataRoot.resize(separator);
  }
  if (!dataRoot.empty()) {
    SetEngineEnvironment(L"MOZ_APP_DATA", dataRoot.c_str());
    SetEngineEnvironment(L"MOZ_LOCAL_APP_DATA", dataRoot.c_str());
    Log("bootstrap: profile registry roots use package LocalState");
  }

  // Headless, and single process: an app container cannot spawn the content
  // child, and the build is configured for a single process anyway.
  SetEngineEnvironment(L"MOZ_HEADLESS", L"1");
  SetEngineEnvironment(L"MOZ_FORCE_DISABLE_E10S", L"1");

  // The headless screen is 1366x768 unless told otherwise, and every window
  // Gecko opens is sized from it. Since the shell shows those pixels at 1:1,
  // a wrong size here is a wrong size on the phone.
  if (width > 0 && height > 0) {
    SetEngineEnvironment(L"MOZ_HEADLESS_WIDTH",
                         std::to_wstring(width).c_str());
    SetEngineEnvironment(L"MOZ_HEADLESS_HEIGHT",
                         std::to_wstring(height).c_str());
    Log("bootstrap: headless screen " + std::to_string(width) + "x" +
        std::to_string(height));
  }

  // Where libxul writes the delay-load substitutions it had to make. See the
  // failure hook in toolkit/xre/Bootstrap.cpp.
  // Ad-hoc notes from inside the engine, for bringing this port up.
  ::SetEnvironmentVariableW(L"GECKO_W10M_NOTE_LOG",
                            (profile + L"\\gecko-notes.log").c_str());

  ::SetEnvironmentVariableW(L"GECKO_W10M_DELAYLOAD_LOG",
                            (profile + L"\\delay-load-used.log").c_str());

  // Gecko's own logging, next to ours, so a failure inside the engine says
  // more than a return code.
  // Not "gecko.log": that is the shell's own log, and two files a suffix
  // apart in the same directory is a way to read the wrong one.
  const std::wstring geckoLog = profile + L"\\gecko-moz.log";
  // Only when the user turned the verbose logs on (the shell reads the pref
  // and sets this). Synchronous and at these levels it is over a thousand
  // lines a second during a video, written by the threads doing the work.
  // Quiet, the last verbose run's file goes too: it can be tens of megabytes.
  {
    ::DeleteFileW(geckoLog.c_str());
    SetEngineEnvironment(L"MOZ_LOG", nullptr);
    SetEngineEnvironment(L"MOZ_LOG_FILE", nullptr);
    Log("bootstrap: Gecko component logging stays off for privacy");
  /*
    SetEngineEnvironment(
        L"MOZ_LOG",
        L"timestamp,sync,nsAppRunner:5,XRE:5,nsComponentManager:5,"
        L"nsChromeRegistry:5,nsIOService:5,URILoader:5,"
        // Video: which decoder was asked, what it answered, and where the
        // pipeline gave up.
        L"MediaDecoder:4,MediaFormatReader:4,PlatformDecoderModule:5,"
        L"WMFDecoderModule:5,MediaDemuxer:4,MediaSource:4,HTMLMediaElement:4,"
        // The start page after a crash opens a channel and never hears back:
        // the TLS handshake is reported, then "waiting for google.com", and
        // nsDocumentOpenInfo::OnStartRequest never arrives. These say where
        // it is standing -- the connection, the cache entry it waits for, or
        // the response that never comes.
        L"nsHttp:4,cache2:3,nsSocketTransport:3,nsHostResolver:3");
    SetEngineEnvironment(L"MOZ_LOG_FILE", geckoLog.c_str());
    Log("bootstrap: verbose logs disabled"); */
  }
  RedirectStdErrTo(profile + L"\\gecko-stderr.log");

  // firefox.exe takes this at the top of main and hands it to Gecko as
  // StartupTimeline::START. We are not firefox.exe, so nobody did -- and
  // Services.startup.getStartupInfo() only defines a property for an event
  // that was recorded. The browser window's tab strip begins its init() with
  //
  //     Services.startup.getStartupInfo().start.getTime()
  //
  // so with start missing it threw on its first statement, and every element
  // init() would have assigned stayed undefined: no arrowScrollbox, no
  // pinnedTabsContainer, no selectedTab, no tabs.
  const mozilla::TimeStamp startedAt = mozilla::TimeStamp::Now();

  PrepareProfile(profile, width, height, scale);
  SeedProfileCaches(install, profile);

  Log("bootstrap: loading xul.dll");
  HMODULE xul = ::LoadPackagedLibrary(L"xul.dll", 0);
  if (!xul) {
    Log("bootstrap: xul.dll failed to load, err " +
        std::to_string(::GetLastError()));
    return -1;
  }

  auto getBootstrap =
      reinterpret_cast<GetBootstrapFn>(::GetProcAddress(xul, "XRE_GetBootstrap"));
  if (!getBootstrap) {
    Log("bootstrap: XRE_GetBootstrap missing, err " +
        std::to_string(::GetLastError()));
    return -2;
  }

  Log("bootstrap: calling XRE_GetBootstrap");
  mozilla::Bootstrap::UniquePtr bootstrap;
  getBootstrap(bootstrap);
  if (!bootstrap) {
    Log("bootstrap: XRE_GetBootstrap returned nothing");
    return -3;
  }
  Log("bootstrap: got the Bootstrap object");

  // This is the first call that runs Gecko's own code, so it is the first
  // that can fall over. NS_LogInit is cheap and touches the allocator, which
  // makes it a useful canary before XRE_main.
  bootstrap->NS_LogInit();
  Log("bootstrap: NS_LogInit returned");

  bootstrap->XRE_StartupTimelineRecord(START, startedAt);
  Log("bootstrap: recorded StartupTimeline::START");

  // argv[0] must be the executable; XRE_main derives the install directory
  // from it. -profile keeps the profile inside LocalState, the only place a
  // packaged app may write.
  const std::string exe = Narrow(install + L"\\Gecko.exe");
  const std::string profileArg = Narrow(profile);
  std::vector<char*> argv;
  std::string a0 = exe;
  std::string a1 = "-profile";
  std::string a2 = profileArg;
  argv.push_back(a0.data());
  argv.push_back(a1.data());
  argv.push_back(a2.data());
  argv.push_back(nullptr);

  mozilla::BootstrapConfig config{};
  config.appData = nullptr;
  // With appData null this is the path of an application.ini to read, and the
  // directory that holds it becomes the application directory -- what
  // resource:/// and chrome://browser/ resolve against.
  //
  // That has to be browser/, not the root. The root application.ini says so
  // in its first line, and reading it instead left every resource:///module
  // unreachable: no command line handler could be created, so nothing opened
  // a window and nsAppStartup was handed eConsiderQuit. firefox.exe arranges
  // the same thing differently, by passing static app data with "browser" as
  // the relative directory; we have no such symbol, so we point at the copy
  // that tools/build-appx.sh stages there.
  std::wstring appIniPath = install + L"\\browser\\application.ini";
  if (::GetFileAttributesW(appIniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
    Log("bootstrap: no browser\\application.ini, falling back to the root one");
    appIniPath = install + L"\\application.ini";
  }
  const std::string appIni = Narrow(appIniPath);
  config.appDataPath = appIni.c_str();

  // Gecko's driver crash guard writes "starting D3D11" to the profile before
  // it tries, and "done" after; a launch that ends between the two is read on
  // the next one as a graphics crash, and hardware compositing is turned off
  // for good. On a phone a launch ends between the two whenever the user
  // gives up on a slow start and kills the app -- which one device log shows
  // happening twice, followed by "hardware compositing is off -- Crashed
  // during startup in a previous session" on every launch after. There is no
  // crash reporter here to tell a kill from a crash, so the guard is off. The
  // variable is read through the CRT, which the shell and xul share.
  ::_putenv_s("MOZ_DISABLE_CRASH_GUARD", "1");
  Log("bootstrap: graphics crash guards are off");

  Log("bootstrap: XRE_main with " + appIni);
  int rc = bootstrap->XRE_main(static_cast<int>(argv.size()) - 1, argv.data(),
                               config);
  Log("bootstrap: XRE_main returned " + std::to_string(rc));

  bootstrap->NS_LogTerm();
  return rc;
}
