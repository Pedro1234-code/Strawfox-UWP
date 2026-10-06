#!/bin/bash
# Build and package the Gecko UWP shell for Windows x64.
#
# The shell is a code-only C++/WinRT XAML app, compiled with MSVC cl.exe: clang
# uses the UWP C++ workload and the Gecko x64 UWP build.
# The package carries the ported Gecko binaries from the browser objdir.
#
# MSBuild's appx signing step fails here with APPX0107, so the package is built
# with makeappx and signed separately with signtool.
#
# Every path handed to a Windows tool goes through cygpath: msys2 rewrites
# Unix-looking arguments, and MSYS2_ARG_CONV_EXCL only stops it mangling
# switches, not paths.
set -e

source "$(dirname "$0")/env.sh"
# This script needs nothing from MozillaBuild, and its msys tools must be the
# ones of the shell running it: env.sh puts MozillaBuild's first, and with
# MSYS2_ARG_CONV_EXCL set below, MozillaBuild's rm, started from Git Bash, does
# not understand /c/... paths -- and rm -f then deletes nothing and says
# nothing.
PATH="/usr/bin:$PATH"
ROOT="$GECKO_W10M_ROOT"
APP="$ROOT/app/GeckoW10m"
# The packaged tree (mach package): two omni.ja archives instead of eight
# thousand loose files, and a preload list ordered by startup use. The raw
# dist/bin is the fallback when it has not been made.
if [ -z "$GECKO_W10M_DIST" ] && [ -f "$GECKO_W10M_OBJ/dist/firefox/omni.ja" ]; then
  GECKO_W10M_DIST="$GECKO_W10M_OBJ/dist/firefox"
fi
DIST="${GECKO_W10M_DIST:-$GECKO_W10M_OBJ/dist/bin}"
# The object directory, for the linker maps the binary patches below need.
# It used to be derived from DIST by stripping /dist/bin, which the packaged
# tree does not end in -- and the thunk patch then silently found no map.
OBJ="$GECKO_W10M_OBJ"
OUT="$ROOT/app/GeckoW10m/AppPackages"
STAGE="$OUT/stage"

VS="$GECKO_W10M_VC"
SDK="$GECKO_W10M_SDK"
SDKV="$GECKO_W10M_SDK_VERSION"
CL="$VS/bin/Hostx64/x64/cl.exe"
LLVM="$(cygpath -u "$GECKO_W10M_LLVM")"
BIN="$SDK/bin/$SDKV/x64"

export INCLUDE="$VS/include;$SDK/Include/$SDKV/ucrt;$SDK/Include/$SDKV/shared;$SDK/Include/$SDKV/um;$SDK/Include/$SDKV/winrt;$SDK/Include/$SDKV/cppwinrt"
# The umbrella library decides which api-set DLLs the shell imports, and an
# api-set that a newer SDK maps a function to may not exist on an older
# phone: the loader then refuses the whole executable before a line of ours
# runs. Headers stay at the newest SDK (cppwinrt lives only there); the
# libraries can be pinned to the oldest OS the package should start on.
SDKV_LIB="$GECKO_W10M_SDK_LIB"
export LIB="$VS/lib/x64/store;$VS/lib/x64;$SDK/Lib/$SDKV_LIB/ucrt/x64;$SDK/Lib/$SDKV_LIB/um/x64"
echo "    shell links against SDK $SDKV_LIB libraries"
export PATH="$VS/bin/Hostx64/x64:$PATH"
export MSYS2_ARG_CONV_EXCL="*"

# The build number is the fourth field of the version and belongs to the build,
# not to the author: it goes up on every build, is never reset and never
# reused. That is what makes "shell starting, version ..." in a device log name
# one package, and what makes installing over the previous one always work --
# Windows accepts only a strictly higher version.
COUNTER="$APP/build-number.txt"
[ -f "$COUNTER" ] || echo 0 > "$COUNTER"
BUILD="$(( $(tr -cd '0-9' < "$COUNTER") + 1 ))"
echo "$BUILD" > "$COUNTER"

# The first three fields are the engine's own version, from the Firefox tree
# the package carries (browser/config/version.txt: "155.0.1" -> 155.0.1, and
# a pre-release such as "156.0a1" -> 156.0.0); the fourth is the build number.
BASE="$(python - "$(cygpath -w "$ROOT/engine/firefox/browser/config/version.txt")" <<'PY'
import re, sys
text = open(sys.argv[1], encoding="utf-8").read().strip()
parts = [int(p) for p in re.findall(r"\d+", text.split("a")[0].split("b")[0])][:3]
parts += [0] * (3 - len(parts))
print(".".join(str(p) for p in parts))
PY
)"
if ! [[ "$BASE" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "cannot read the engine's version from engine/firefox/browser/config/version.txt" >&2
  exit 1
fi
VERSION="$BASE.$BUILD"
# Write it back, so the tree records the package it produced.
python - "$(cygpath -w "$APP/Package.appxmanifest")" "$VERSION" <<'PY'
import re, sys
path, version = sys.argv[1], sys.argv[2]
text = open(path, encoding="utf-8").read()
text = re.sub(r'(<Identity[^>]*?Version=")[0-9.]+(")',
              lambda m: m.group(1) + version + m.group(2), text, count=1,
              flags=re.S)
open(path, "w", encoding="utf-8").write(text)
PY
echo "=== version $VERSION (build $BUILD) ==="
PKG="$OUT/Gecko_${VERSION}_X64.appx"

# A stage left over from the last build must really be gone: files that were
# just written can still be held open for a moment (the antivirus scans them),
# Windows then only marks them for deletion, and rm -rf leaves directories
# behind without failing -- stale files that end up in the package.
for try in 1 2 3 4 5; do
  rm -rf "$STAGE" 2> /dev/null
  [ -e "$STAGE" ] || break
  sleep 2
done
if [ -e "$STAGE" ]; then
  echo "cannot remove $STAGE; something still holds files in it" >&2
  exit 1
fi
mkdir -p "$STAGE" "$OUT/obj"
cd "$APP"

STAGE_W="$(cygpath -w "$STAGE")"
OBJDIR_W="$(cygpath -w "$OUT/obj")"

echo "=== compile the shell (cl.exe, x64, C++/WinRT) ==="
SRCS="pch.cpp App.cpp MainPage.cpp client/BrowserPreferences.cpp client/DrmBridge.cpp client/EngineView.cpp client/Log.cpp client/SearchEngines.cpp client/TabManager.cpp engine/CrashProbe.cpp engine/OverlayProbe.cpp engine/GeckoEngine.cpp engine/GeckoRuntimeHost.cpp engine/gecko_capi_stub.cpp"
OBJS=""
for s in $SRCS; do
  name="$(echo "$s" | tr '/' '_' | sed 's/\.cpp$/.obj/')"
  "$CL" /nologo /c "$(cygpath -w "$APP/$s")" "/Fo:$OBJDIR_W\\$name" \
        /std:c++20 /EHsc /GR- /O2 /utf-8 \
        /DGECKO_W10M_USE_ENGINE_STUB /DWIN32 /D_WIN32 /DWIN64 /DNOMINMAX \
        /DUNICODE /D_UNICODE /DWINAPI_FAMILY=WINAPI_FAMILY_APP \
        "/I$(cygpath -w "$APP")" "/I$(cygpath -w "$SDK/Include/$SDKV/cppwinrt")"
  OBJS="$OBJS $OBJDIR_W\\$name"
done

echo "=== compile the Gecko bootstrap (clang-cl, x64) ==="
# This one translation unit includes Gecko headers, and that decides its
# compiler. mfbt uses clang builtins cl.exe does not have (__builtin_unreachable
# among them), so it must be clang-cl. The two halves meet at a plain C
# boundary in gecko_bootstrap.h.
"$LLVM/clang-cl.exe" --target=x86_64-pc-windows-msvc /nologo /c \
  "$(cygpath -w "$APP/engine/gecko_bootstrap.cpp")" \
  "/Fo:$OBJDIR_W\\engine_gecko_bootstrap.obj" \
  /std:c++20 /EHs-c- /GR- /O2 /utf-8 \
  /DWIN32 /D_WIN32 /DWIN64 /DNOMINMAX /DUNICODE /D_UNICODE \
  /DWINAPI_FAMILY=WINAPI_FAMILY_DESKTOP_APP /DXP_WIN /DGECKO_W10M=1 \
  "/I$(cygpath -w "$DIST/../include")" \
  "/I$(cygpath -w "$ROOT/engine/firefox/toolkit/components/startup")"
OBJS="$OBJS $OBJDIR_W\\engine_gecko_bootstrap.obj"

echo "=== compat stubs ==="
# Windows 10 Mobile does not carry every desktop module xul.dll imports, and a
# single missing one stops the whole engine from loading. Each stub here stands
# in for one such module; see the source for what it replaces and why failing
# is the right answer. They are linked /NODEFAULTLIB against kernel32 alone so
# a stub never drags in a runtime of its own.
for stub in ktmw32; do
  "$CL" /nologo /c "$(cygpath -w "$APP/compat/$stub.c")" \
        "/Fo:$OBJDIR_W\\$stub.obj" /O2 /MT /GS- \
        /DWIN32 /D_WIN32 /DWIN64 /DWINAPI_FAMILY=WINAPI_FAMILY_DESKTOP_APP
  "$LLVM/lld-link.exe" "$OBJDIR_W\\$stub.obj" /DLL /APPCONTAINER \
    /MACHINE:X64 /NODEFAULTLIB /ENTRY:DllMain \
    "/DEF:$(cygpath -w "$APP/compat/$stub.def")" \
    "/OUT:$STAGE_W\\$stub.dll" \
    kernel32.lib
  echo "    $stub.dll ($(stat -c%s "$STAGE/$stub.dll") bytes)"
done

echo "=== link Gecko.exe ==="
# mozglue.lib supplies moz_xmalloc and the rest of Gecko's allocator, which the
# bootstrap unit reaches through the mfbt headers. mozglue.dll already ships in
# the package, so this adds an import and no new payload.
"$LLVM/lld-link.exe" $OBJS "/OUT:$STAGE_W\\Gecko.exe" /APPCONTAINER \
  /SUBSYSTEM:WINDOWS,10.0 /ENTRY:wWinMainCRTStartup /MACHINE:X64 \
  "/MAP:$OBJDIR_W\\Gecko.map" \
  "/LIBPATH:$(cygpath -w "$DIST/../lib")" mozglue.lib \
  WindowsApp.lib OneCoreUap.lib   /DELAYLOAD:oleaut32.dll /DELAYLOAD:mozglue.dll delayimp.lib
# mozglue.dll delay-loaded as well: it was the shell's only remaining static
# import, and with it gone the executable starts with nothing but system
# DLLs -- so a launch that still dies before the first log line dies in the
# executable, the CRT or XAML, and one that reaches the log and dies at the
# first allocator call dies in mozglue's own dependencies.
# oleaut32.dll is the one static import of the shell that is neither ours nor
# OneCore: C++/WinRT reaches SysAllocString/SysFreeString/SysStringLen and
# Get/SetErrorInfo through it, on error paths only. A phone whose OS does
# not carry oleaut32 refuses the whole executable before wWinMain -- which is
# what a Lumia 650 does on 1511 and 1607 while the Elite X3 on 1709 runs.
# Delay-loaded, it is resolved on first use, not at launch.
# OneCoreUap.lib after WindowsApp.lib: the 1607 SDK's WindowsApp.lib lacks
# SetErrorInfo/GetErrorInfo, which C++/WinRT's hresult_error needs; the
# OneCore umbrella of the same SDK has them. WindowsApp.lib still resolves
# first, so nothing else changes.
echo "    $(stat -c%s "$STAGE/Gecko.exe") bytes"

echo "=== stage the package ==="
cp "$APP/Package.appxmanifest" "$STAGE/AppxManifest.xml"
mkdir -p "$STAGE/Assets"
cp "$APP/Assets/"*.png "$STAGE/Assets/"

# The C runtime comes from the x64 UWP VCLibs framework package rather than the
# desktop redist next to the compiler. Its DLLs carry
# an _app suffix and reference each other by that name, while mozglue and xul
# import the plain names, so both spellings go in the package.
VCLIBS="C:/Program Files (x86)/Microsoft SDKs/Windows Kits/10/ExtensionSDKs/Microsoft.VCLibs/14.0/Appx/Retail/x64/Microsoft.VCLibs.x64.14.00.appx"
if [ -f "$VCLIBS" ]; then
  CRTTMP="$APP/AppPackages/crt"
  rm -rf "$CRTTMP" && mkdir -p "$CRTTMP"
  powershell.exe -NoProfile -Command "
    Add-Type -AssemblyName System.IO.Compression.FileSystem;
    \$zip = [System.IO.Compression.ZipFile]::OpenRead('$(cygpath -w "$VCLIBS")');
    \$zip.Entries | Where-Object { \$_.Name -like '*.dll' } | ForEach-Object {
      [System.IO.Compression.ZipFileExtensions]::ExtractToFile(\$_,
        (Join-Path '$(cygpath -w "$CRTTMP")' \$_.Name), \$true) };
    \$zip.Dispose()" >/dev/null
  # Only the two DLLs anything of ours imports, and only under the plain
  # names. The _app names are gone from the package altogether: a Lumia 650
  # on 1607 mapped msvcp140.dll (these same bytes) and then never mapped the
  # vcruntime140_app.dll it imports, though the file, flagged APPCONTAINER,
  # lay beside it -- the plain-named copy was never even tried. The _app
  # runtime names appear to be resolved through the VCLibs framework package
  # on that OS, which this package neither declares nor carries. So the copy
  # of msvcp140 has its import rewritten to vcruntime140.dll, in place, same
  # length, and nothing in the package names an _app DLL any more.
  cp "$CRTTMP/vcruntime140_app.dll" "$STAGE/vcruntime140.dll"
  cp "$CRTTMP/msvcp140_app.dll" "$STAGE/msvcp140.dll"
  python - "$(cygpath -w "$STAGE/msvcp140.dll")" <<'PYEOF'
import re, sys
p = sys.argv[1]
d = open(p, 'rb').read()
# The name is stored as VCRUNTIME140_APP.dll; match it in any case and keep
# the field the same length, NUL-padded. No backslashes here on purpose:
# they do not survive the shells this passes through.
pat = re.compile(rb'vcruntime140_app[.]dll', re.IGNORECASE)
n = len(pat.findall(d))
assert n >= 1, 'import name not found'
out = pat.sub(lambda m: b'vcruntime140.dll' + bytes(4), d)
assert len(out) == len(d)
open(p, 'wb').write(out)
print('    msvcp140.dll: %d import name(s) rewritten to vcruntime140.dll' % n)
PYEOF
  rm -rf "$CRTTMP"
  echo "    CRT from VCLibs 14.00 x64 (UWP, appcontainer), plain names only"
else
  echo "    WARNING: VCLibs x64 package not found, no CRT staged" >&2
fi

# The ported engine.
if [ -d "$DIST" ]; then
  cp -r "$DIST/." "$STAGE/"
  # mobile-config-firefox: autoconfig at the root (the GRE directory, where
  # general.config.filename is looked for), modules and themes beside it,
  # policies in the app's distribution directory.
  MCF="$ROOT/vendor/mobile-config-firefox"
  if [ -d "$MCF" ]; then
    cp "$MCF/mobile-config-autoconfig.js" "$STAGE/mobile-config-autoconfig.js"
    mkdir -p "$STAGE/mobile-config-firefox" "$STAGE/browser/distribution"
    cp -r "$MCF/modules/." "$STAGE/mobile-config-firefox/"
    mkdir -p "$STAGE/mobile-config-firefox/themes"
    cp -r "$MCF/themes/." "$STAGE/mobile-config-firefox/themes/"
    cp "$MCF/policies.json" "$STAGE/browser/distribution/policies.json"
    echo "    mobile-config-firefox staged"
  fi
  echo "    engine payload from $DIST"
  # resource:/// and chrome://browser/ resolve against the directory holding
  # the application.ini that XRE_main is given, and for a browser build that
  # directory is browser/. The packaged tree has no application.ini there --
  # firefox.exe never needs one, it passes static app data instead -- so put
  # the root copy in place for the shell to point at.
  if [ -f "$STAGE/application.ini" ] && [ -d "$STAGE/browser" ]; then
    cp "$STAGE/application.ini" "$STAGE/browser/application.ini"
    echo "    application.ini staged into browser/ as the app directory"
  fi
  # Application default preferences for this port. The only one that matters
  # so far decides whether anything can be seen at all: the hardware
  # compositors present to a surface, and a headless widget has none, so the
  # software one -- which paints into a buffer the shell reads back -- is the
  # only one that reaches the screen here.
  # In the packaged tree the defaults live inside browser/omni.ja; the loose
  # directory is still read, and is ours to fill.
  mkdir -p "$STAGE/browser/defaults/preferences"
  if [ -d "$STAGE/browser/defaults/preferences" ]; then
    # THE NAME MATTERS. Gecko sorts this directory REVERSE-alphabetically and
    # applies the files in that order, so the alphabetically FIRST name is
    # applied LAST and wins (modules/libpref/Preferences.cpp,
    # pref_CompareFileNames). As "gecko_w10m.js" this file was applied before
    # firefox.js, which then quietly put its own defaults back: the home page,
    # the start page, first-run behaviour, resume-from-crash and
    # close-window-with-last-tab were all set here and none of them counted.
    # A leading "00-" puts this last in the queue and first in authority.
    cat > "$STAGE/browser/defaults/preferences/00-gecko.js" <<PREFS
// Gecko, Windows 10 Mobile. See tools/build-appx.sh.
// Back on. The control experiment answered: with the GPU path switched off
// entirely the window was still hidden, so nothing about the swap chain hides
// it -- but the software build died of its own separate fault on the way, in
// CompositorD3D11::Initialize, which is the line below.
// Software WebRender, hardware compositing left on. This is the one
// configuration that has never run to first paint: the earlier software
// control died in CompositorD3D11 before it got there, and that path is now
// closed by the pref below. The window is hidden at first paint with no
// panel, no swap chain and no GPU frame -- and the one thing every dying run
// has that every living run lacked is ANGLE and EGL coming up. With this true
// they never do: RenderCompositor::Create takes the software branch and
// falls through to RenderCompositorSWGL, while gfxConfig still says hardware
// compositing is on and the D3D11 device is still made. If the window lives,
// the hider is inside ANGLE's initialisation and the hardware path has one
// file to fix. If it is hidden anyway, it is the gfx configuration itself.
pref("gfx.webrender.software", false);

// Software WebRender composited by D3D11, for phones whose GPU stops at
// feature level 9_3 (Lumia 550/650/950): hardware WebRender needs GLES3 and
// ANGLE there offers only GLES2, so the tiles are drawn on the CPU -- but they
// are put together and presented on the GPU, into the same XAML
// SwapChainPanel ANGLE uses on the bigger phones, instead of being copied out
// through main memory into an Image every frame.
//
// This was false because CompositorD3D11::Initialize reached through the
// widget for an HWND the headless widget does not have and read 0x1c out of
// a null pointer. It no longer asks: with no HWND it makes a composition swap
// chain and hands it to the panel (gfx/layers/d3d11/GeckoW10mPanelSwapChain.cpp).
// Phones that run hardware WebRender never reach it; if it cannot start, the
// software branch falls through to RenderCompositorSWGL as before.
pref("gfx.webrender.software.d3d11", true);
// ...and the pixels of that software WebRender shaded on the GPU as well.
// SWGL keeps running WebRender's vertex shaders on the CPU -- a 9_3 GPU has
// no vertex texture fetch, which is why hardware WebRender cannot run there
// -- but the quads go to the compositor's Direct3D 11 device, which shades
// them straight into the compositor's tiles with pixel shaders built for
// ps_4_0_level_9_3 (gfx/wr/swgl/src/gpu_d3d11.h). Programs and blend modes
// that do not fit stay on the CPU; text is greyscale-antialiased, since
// subpixel text needs dual-source blending.
pref("gfx.webrender.software.d3d11.gpu-shading", true);
// Closing the last tab opens a fresh one instead of quitting the browser: on a
// phone a quit is a black screen and a relaunch, not something anyone asked for.
pref("browser.tabs.closeWindowWithLastTab", false);
// Every window a page opens is a tab, the sized ones included: sign-in windows
// (Google, VK and the like) call window.open with a width and a height, and
// Firefox makes those a window of their own -- which this port never shows, so
// the sign-in did nothing. As a tab it is selected at once, keeps its opener
// (the result gets back to the site), and window.close() returns to the tab
// that opened it.
pref("browser.link.open_newwindow", 3);
pref("browser.link.open_newwindow.restriction", 0);

// The start page. A default, so Settings > Home can change it and the change
// sticks in the profile.
pref("browser.startup.homepage", "https://google.com");

// The port's own version, for Settings > About. The engine's version is
// Firefox's and is displayed beside it.
pref("gecko.port.version", "$VERSION");
// Settings > About > Verbose logs for debugging. Off: the shell writes what a
// crash needs and Gecko's own logging stays off. The shell reads the user's
// choice out of prefs.js at launch, so it takes effect after a restart.
pref("gecko.debug.verbose_logs", false);
// 0: the first tab is blank. Every launch used to begin with google.com,
// and on a network where it does not answer the handshake sat in front of the
// whole startup with the browser deaf to taps until it gave up. The home
// button still goes to the home page above; startup just does not wait for
// the network. (1 to start on the home page again.)
pref("browser.startup.page", 0);
// On a brand new profile Firefox skips the home page on purpose, because it
// normally shows its onboarding tour instead -- and that is off here, so the
// first launch after installing landed on about:blank. Do not skip it.
pref("browser.startup.firstrunSkipsHomepage", false);

// No gamepads on a phone, and looking for them is not free: the raw input
// calls the service uses are absent from this device's USER32, so it ended up
// allocating an array for an uninitialised device count and aborting the
// browser. Fixed in the stub as well; this switches off a service that has
// nothing to do here anyway.
pref("dom.gamepad.enabled", false);
pref("dom.gamepad.extensions.enabled", false);
pref("dom.gamepad.haptic_feedback.enabled", false);

// A phone suspends an app and then kills it, which Firefox cannot tell from a
// crash -- so it restored the previous session on every launch and the start
// page above was never reached. Start fresh instead. To keep tabs across a
// suspend instead of seeing the start page, set this back to true and
// browser.startup.page to 3.
pref("browser.sessionstore.resume_from_crash", false);

// Nothing else opens a tab of its own on a first run: no privacy-notice tab
// (that is the second tab that used to appear), no onboarding, no what's-new
// page after an update.
pref("datareporting.policy.firstRunURL", "");
pref("startup.homepage_welcome_url", "");
pref("startup.homepage_welcome_url.additional", "");
pref("startup.homepage_override_url", "");
pref("browser.aboutwelcome.enabled", false);
pref("browser.startup.upgradeDialog.enabled", false);
// Memory. The engine's defaults are a desktop's; these are the values Firefox
// for Android ships for phones of this class, where the cache sizes below
// were sized for a machine with a swap file. The phone gives an app a fixed
// ceiling and ends it past that.
//
// Decoded images kept around: 128 MB, not two gigabytes.
pref("image.mem.surfacecache.max_size_kb", 131072);
// Pages kept alive for back/forward: two, not a count worked out from the
// RAM present. Each one is a whole document held in memory.
pref("browser.sessionhistory.max_total_viewers", 2);
// The in-memory network cache: 16 MB, not sized from the RAM present.
pref("browser.cache.memory.capacity", 16384);
// The JS nursery: 16 MB, not 64.
pref("javascript.options.mem.nursery.max_kb", 16384);
// Video a page has handed to Media Source Extensions -- every HLS and DASH
// player, rutube's included -- is kept until a source buffer holds 150 MB of
// it, and 20 MB of audio beside that. On a 390 MB ceiling that is the whole
// app: the Lumia 650 was ended on rutube.ru at 475 MB with the heap doubling
// in the last minute. 24 MB of video is still half a minute of 720p ahead of
// the playhead; the player simply asks for the next segment sooner.
pref("media.mediasource.eviction_threshold.video", 25165824);
pref("media.mediasource.eviction_threshold.audio", 4194304);
// The in-memory media cache for plain <video src>: 4 MB per stream instead of
// 8, and all of them together no more than 24 MB.
pref("media.memory_cache_max_size", 4096);
pref("media.memory_caches_combined_limit_kb", 24576);
// Tabs in the background are unloaded when memory is tight, and the phone
// says when that is (gecko_w10m_memory_pressure).
pref("browser.tabs.unloadOnLowMemory", true);
// Connection pool sized as on Android.
pref("network.http.max-connections", 128);
// Twenty seconds before a script is called slow, as on Android: the CPU is a
// phone's, and the dialog offering to stop it cannot be answered here yet.
pref("dom.max_script_run_time", 20);
// CSS error reporting is work done for a console nobody opens.
pref("layout.css.report_errors", false);
// The session file every ten seconds rather than fifteen: it is what survives
// the app being ended, which is how every session ends here.
pref("browser.sessionstore.interval", 10000);

// Local ML models -- translation, alt text, the chat sidebar -- are not
// something this phone will run, and the inference runtime is not shipped.
pref("browser.ml.enable", false);
pref("browser.ml.chat.enabled", false);
pref("browser.ml.linkPreview.enabled", false);
// Quarter-size shared texture-cache pages: 4 MB each instead of 16. See the
// pref's entry in StaticPrefList.yaml.
pref("gfx.webrender.texture-cache.small", true);

// The intermediate-certificate preload and CRLite revocation filters: a
// 15 MB store loaded at every start (explicit/cert-storage/storage) and
// refreshed from Remote Settings, which this build does not reach anyway.
// Certificates are still verified; the chain is fetched from the site (AIA)
// and revocation falls back to OCSP as it did before CRLite.
pref("security.remote_settings.intermediates.enabled", false);
pref("security.remote_settings.crlite_filters.enabled", false);
pref("security.pki.crlite_mode", 0);
// Breach alerts and the trust panel that shows them: the Rust store behind
// them fails to open its file in the container on every start ("Access is
// denied" from RustBreachAlerts) and nothing here needs them.
pref("signon.management.page.breach-alerts.enabled", false);
pref("browser.urlbar.trustPanel.featureGate", false);

// No HTTP/3: QUIC over UDP on this phone's stack is an unknown, and a stuck
// QUIC attempt looks exactly like a page that never loads.
pref("network.http.http3.enable", false);

// mobile-config-firefox (postmarketOS): a phone-shaped chrome for desktop
// Firefox. Autoconfig loads its modules from mobile-config-firefox/ under the
// engine directory; see vendor/mobile-config-firefox/README-PORT.md.
pref("general.config.filename", "mobile-config-autoconfig.js");
pref("general.config.obscure_value", 0);
pref("general.config.sandbox_enabled", false);
pref("browser.uidensity", 2);
// A phone: touch events on, mobile viewport handling on, pinch zoom on.
pref("dom.w3c_touch_events.enabled", 1);
pref("dom.meta-viewport.enabled", true);
pref("apz.allow_zooming", true);
pref("apz.allow_double_tap_zooming", true);
pref("ui.touch.radius.enabled", true);
// The same for the click a tap becomes: without it a tap must land inside the
// element, and desktop-sized controls -- a menu item, the buttons of a dialog
// in the settings -- are a few millimetres tall at this density. These are
// Firefox for Android's values; the desktop ones reach 12 mm and pick the
// wrong neighbour.
pref("ui.mouse.radius.enabled", true);
pref("ui.mouse.radius.topmm", 2);
pref("ui.mouse.radius.rightmm", 3);
pref("ui.mouse.radius.bottommm", 2);
pref("ui.mouse.radius.leftmm", 3);
pref("ui.mouse.radius.reposition", true);

// The XAML compositor dies about two and a half seconds after these load, and
// the last thing in the log before it is always the same run: mozavcodec,
// mfplat, mf, dxva2, evr -- the media stack with hardware decoding, and the gfx
// sanity test trying to decode a video. The same libraries loaded in every
// software build and nothing happened; the difference now is that D3D11 is on,
// so DXVA takes a device.
//
// So this build does not ask. Hardware video decoding was already reported off
// as a feature, which did not stop the probe from happening.
// Hardware video decoding. Without it every frame above 360p is decoded by
// four phone cores and then converted from YV12 to RGB by the same cores,
// which is why 720p stuttered: the log said "wmf H264 codec software video
// decoder - no DXVA, yv12". With DXVA the Adreno decodes H.264, VP9 and AV1
// itself and hands the compositor an NV12 texture it can sample directly.
// force-enabled skips the blocklist, which has nothing to say about a phone
// GPU it has never heard of.
pref("media.hardware-video-decoding.enabled", true);
pref("media.hardware-video-decoding.force-enabled", true);
pref("media.wmf.dxva.enabled", true);
pref("media.wmf.dxva.d3d11.enabled", true);
pref("media.sanity-test.disabled", true);

// The Adreno in this phone decodes H.264 and nothing else: Windows 10 Mobile
// ships no VP9 or AV1 decoder, so those codecs land on the CPU, where four
// ARM32 cores manage about 360p. Sites that offer a choice -- YouTube offers
// VP9 first -- have to be told we cannot take it.
//
// Not with these prefs, though. Switching the codecs off browser-wide takes
// them from every site, including ones with nothing else to fall back to, and
// a site with no H.264 ladder has nothing left to play. h264ify has done this
// for years without that: it answers "no" to VP8, VP9 and AV1 inside the page,
// on YouTube only, and YouTube picks H.264 out of the ladder it was going to
// offer anyway. That is in the vendored config now; the hosts it does it on
// are gecko.h264ify.hosts, and everywhere else the browser keeps every codec
// Firefox ships with.
pref("gecko.h264ify.hosts", "youtube.com,youtube-nocookie.com");
// PlayReady through the phone, offered to these hosts only: their page
// script sees a com.microsoft.playready key system whose license exchange
// runs on the phone's own PlayReady (see gecko_spotify in the autoconfig and
// client/DrmBridge.cpp in the shell). Stage one, license acquisition only.
// Off on master: the experiment lives on the drm-playready-experiment branch.
pref("gecko.spotify.hosts", "");

// Fingerprinting protection, minus the part that lies about the screen.
//
// The console says it plainly on every YouTube load: "Fingerprinting
// Protection is altering screen.availWidth and screen.availHeight". A video
// player works out which controls to build by comparing the window it has
// against the screen it thinks exists, and in fullscreen the settings menu is
// the one piece of that decision that comes out wrong here -- the click lands
// on the gear and the page builds nothing at all. ScreenRect, ScreenAvailRect
// and WindowOuterSize are the three it alters; the rest of the protection
// stays on.
pref("privacy.fingerprintingProtection.overrides",
     "-ScreenRect,-ScreenAvailRect,-WindowOuterSize");

// One hardware-decoded video at a time. The pre-roll ad on vkvideo.ru is a
// second <video> with a decoder of its own, and 16 ms after the second DXVA
// session was configured the Adreno's D3D11 driver went into an endless
// recursion and blew its stack -- 0xc00000fd, qcdx11um8996.dll+0x73503
// calling +0x71b73 calling +0x73503, all the way down. Upstream allows eight
// at once; this phone manages one. The ad is 360p and the CPU has never had
// trouble with 360p, so the one that gets the GPU is the one that needs it.
pref("media.wmf.dxva.max-videos", 1);

// And whatever decoders there are share a single D3D11 device instead of
// making one each. Firefox does this on Windows already; the blocklist is
// what kept it off here, and the blocklist has never heard of this GPU.
pref("gfx.direct3d11.reuse-decoder-device", true);
pref("gfx.direct3d11.reuse-decoder-device.force-enabled", true);

// The URL classifier's local database. The browser went silent for 79 seconds
// shortly after a cold start -- render thread included, taps queuing up and
// arriving all at once when it came back -- and the last thing in the engine
// log before the silence was "Classifier Update #1", writing and SHA-hashing
// several hundred files in a row. That is Safe Browsing keeping a multi-
// megabyte blocklist up to date, which a desktop does not notice and this
// phone's storage plainly does.
//
// It costs the warning page for known phishing and malware sites. Nothing
// else: certificates, mixed content, tracking protection and the permission
// prompts are all untouched.
pref("browser.safebrowsing.malware.enabled", false);
pref("browser.safebrowsing.phishing.enabled", false);
pref("browser.safebrowsing.downloads.enabled", false);
pref("browser.safebrowsing.provider.google4.updateURL", "");
pref("browser.safebrowsing.provider.google4.gethashURL", "");
pref("browser.safebrowsing.provider.mozilla.updateURL", "");
pref("browser.safebrowsing.provider.mozilla.gethashURL", "");
pref("urlclassifier.trackingTable", "");

// And the rest of what a fresh profile does over the network while the person
// is waiting for their first page: studies, experiments, telemetry pings.
pref("app.normandy.enabled", false);
pref("app.normandy.api_url", "");
pref("app.shield.optoutstudies.enabled", false);
pref("datareporting.healthreport.uploadEnabled", false);
pref("datareporting.policy.dataSubmissionEnabled", false);
pref("toolkit.telemetry.unified", false);
pref("toolkit.telemetry.archive.enabled", false);

// Canvas 2D was drawing on the CPU for a reason that does not apply here:
// upstream only accelerates it in the GPU process, and there is none, so the
// feature reported "Disabled by GPU Process disabled". allow-in-parent is the
// switch upstream added for exactly this case; force-enabled is again for the
// blocklist, which does not know this GPU.
// Partial present: WebRender redraws only what changed and trusts the swap
// chain to have kept the rest. On this ANGLE-on-D3D11 chain what comes back is
// not always what was left there -- the log shows frames rendered with one
// dirty rect, and the leftovers show up as specks of an older frame, white
// dots scattered over a dark photograph. Redrawing the whole frame costs fill
// rate this GPU can spare more easily than it can spare being wrong.
pref("gfx.webrender.max-partial-present-rects", 0);
pref("gfx.webrender.allow-partial-present-buffer-age", false);

// The sidebar's launcher strip is off. It is the vertical bar down the left
// edge with the tab list and the settings cog, and on a phone it is a column
// of chrome in front of the page.
pref("sidebar.revamp", false);
pref("sidebar.visibility", "hide-on-close");
pref("sidebar.verticalTabs", false);

pref("gfx.canvas.accelerated", true);
pref("gfx.canvas.accelerated.allow-in-parent", true);
pref("gfx.canvas.accelerated.force-enabled", true);
PREFS
    echo "    app default preferences staged"
  fi
  # A startup cache captured on a device (app/GeckoW10m/seed/, untracked):
  # scriptCache.bin, urlCache.bin, startupCache.4.little and the profile's
  # compatibility.ini from the run that made them. The shell copies them
  # into a brand-new profile, so the first launch after installing starts
  # the way a second launch does. Compiled chrome is only valid for the
  # engine build that compiled it, so the seed ships only when its build id
  # is this package's; otherwise it is left out and said so.
  SEED="$APP/seed"
  if [ -f "$SEED/compatibility.ini" ]; then
    seed_id=$(grep -o "LastVersion=[^/]*" "$SEED/compatibility.ini" | sed 's/.*_//')
    our_id=$(grep -o "^BuildID=.*" "$STAGE/browser/application.ini" | cut -d= -f2 | tr -d '\r')
    if [ -n "$seed_id" ] && [ "$seed_id" = "$our_id" ]; then
      mkdir -p "$STAGE/seed"
      for f in compatibility.ini scriptCache.bin urlCache.bin startupCache.4.little; do
        [ -f "$SEED/$f" ] && cp "$SEED/$f" "$STAGE/seed/$f"
      done
      echo "    startup cache seed staged (build $our_id): $(ls "$STAGE/seed" | tr '\n' ' ')"
    else
      echo "    startup cache seed NOT staged: it is for build ${seed_id:-?}, this is $our_id" >&2
    fi
  fi

  # resources.pri: every packaged XAML app carries one and this one did not.
  # The XAML runtime on 1709 tolerated its absence; older ones are not known
  # to. Built from the manifest and the Assets alone, in a scratch tree, so
  # makepri does not index the 250 MB of engine files.
  PRITMP="$OUT/pri"
  rm -rf "$PRITMP" && mkdir -p "$PRITMP/Assets"
  cp "$STAGE/AppxManifest.xml" "$PRITMP/" && cp "$STAGE/Assets/"* "$PRITMP/Assets/"
  if "$SDK/bin/$SDKV/x64/makepri.exe" createconfig /cf "$(cygpath -w "$PRITMP/priconfig.xml")" /dq en-US /pv 10.0.0 /o >/dev/null 2>&1      && "$SDK/bin/$SDKV/x64/makepri.exe" new /pr "$(cygpath -w "$PRITMP")" /cf "$(cygpath -w "$PRITMP/priconfig.xml")"           /mn "$(cygpath -w "$PRITMP/AppxManifest.xml")" /of "$(cygpath -w "$STAGE/resources.pri")" /o >/dev/null 2>&1; then
    echo "    resources.pri built ($(stat -c %s "$STAGE/resources.pri") bytes)"
  else
    echo "    WARNING: makepri failed, no resources.pri" >&2
  fi
  rm -rf "$PRITMP"

  # The following compatibility rewrites exist only for the ARM32 phone
  # binary. x64 has neither Thumb entry points nor the old mobile api-set set.
  if [ "$GECKO_W10M_ARCH" = arm ]; then
  # Api-set forwarder shims for phones whose OS lacks an api-set name (a
  # Lumia 650 on 1607 lacks api-ms-win-core-fibers-l1-1-0, and the loader
  # refused vcruntime140_app.dll for it). See tools/gen-apiset-shims.py.
  python "$(cygpath -w "$ROOT/tools/gen-apiset-shims.py")" "$(cygpath -w "$STAGE")" "$(cygpath -w "$OUT/shims")" "$CL" "$(cygpath -w "$LLVM/lld-link.exe")" "$(cygpath -w "$SDK/Lib/$SDKV_LIB/um/arm")"

  # Control Flow Guard, as lld-link emits it for 32-bit ARM, is wrong: the
  # guard dispatcher reaches its target with bx, and an entry recorded without
  # the Thumb bit lands there in ARM state, where the first honest Thumb
  # instruction is an illegal one. Every module we link needs the flag cleared,
  # not just xul.dll -- nss3.dll died this way at the entry of PR_CallOnce.
  # The CRT staged from VCLibs is Microsoft's own build and is left alone.
  # See tools/strip-cfg.py.
  cfg_targets=()
  while IFS= read -r rel; do
    cfg_targets+=("$(cygpath -w "$STAGE/$rel")")
  done < <(cd "$DIST" && find . \( -name '*.dll' -o -name '*.exe' \) -printf '%P\n')
  python "$(cygpath -w "$ROOT/tools/strip-cfg.py")" "${cfg_targets[@]}"     | sed 's/^/    /'
  # lld-link synthesises the __imp_ pointer for a dllimport-declared symbol that
  # turns out to live in the same image, and on 32-bit ARM it synthesises it
  # without the Thumb bit -- so the indirect call through it lands in ARM state.
  # nss3.dll died at PR_CallOnce that way. See tools/fix-thumb-pointers.py.
  python "$(cygpath -w "$ROOT/tools/fix-thumb-pointers.py")" "${cfg_targets[@]}"     | sed 's/^/    /'
  # clang's 32-bit ARM virtual-call thunks tail-jump through r1, the first
  # argument register, so every pointer-to-member call on a virtual function
  # arrives with its first argument destroyed. The linker map is what finds
  # them: clang allocates registers differently from one thunk to the next, so
  # their bytes vary while the map names every one. See
  # tools/patch-vcall-thunks.py.
  python "$(cygpath -w "$ROOT/tools/patch-vcall-thunks.py")"     "$(cygpath -w "$STAGE/xul.dll")"     "$(cygpath -w "$OBJ/toolkit/library/build/xul.map")"     | sed 's/^/    /'
  # ANGLE's two DLLs have the same thunks and, until 0.2.4.6, no map -- so they
  # went unpatched, and every glUniform* reached the D3D backend with the
  # location replaced by the thunk's address (a fast fail under the hardened
  # STL, and the reason hardware WebRender never survived its first frame).
  for angle in libGLESv2 libEGL; do
    map="$OBJ/third_party/angle/${angle}_gn/${angle}.map"
    if [ -f "$map" ]; then
      python "$(cygpath -w "$ROOT/tools/patch-vcall-thunks.py")"         "$(cygpath -w "$STAGE/${angle}.dll")"         "$(cygpath -w "$map")"         | sed 's/^/    /'
    else
      echo "    WARNING: no linker map for ${angle}.dll -- its virtual-call thunks stay broken" >&2
    fi
  done
  fi
else
  echo "    WARNING: $DIST not found, packaging the shell alone" >&2
fi
find "$STAGE" -name "*.pdb" -delete 2>/dev/null || true
echo "    staged $(find "$STAGE" -type f | wc -l) files, $(du -sm "$STAGE" | cut -f1) MB"

echo "=== pack ==="
rm -f "$PKG"
"$BIN/makeappx.exe" pack /d "$STAGE_W" /p "$(cygpath -w "$PKG")" /o /nv

echo "=== sign ==="
# Signing runs through PowerShell because msys2 leaves TEMP and TMP empty in
# the environment it hands to children, and signtool needs a temp directory to
# repack an appx. See tools/sign-appx.ps1.
# The signer's subject must be exactly the manifest's Publisher or the package
# will not install. app/GeckoW10m/Gecko.pfx (not tracked) is used when it is
# there; otherwise sign-appx.ps1 makes a self-signed certificate for that
# Publisher. Gecko.cer beside it is the certificate to trust on the phone.
CERT="$APP/Gecko.pfx"
PUBLISHER="$(tr -d '\r' < "$APP/Package.appxmanifest" | sed -n '/<Identity/,/\/>/{s/.*Publisher="\([^"]*\)".*/\1/p}' | head -1)"
powershell.exe -NoProfile -ExecutionPolicy Bypass \
  -File "$(cygpath -w "$ROOT/tools/sign-appx.ps1")" \
  -Package "$(cygpath -w "$PKG")" \
  -Certificate "$(cygpath -w "$CERT")" \
  -Subject "$PUBLISHER" \
  -SdkBin "$(cygpath -w "$BIN")"

# The certificate the package is signed with. A phone will not install a
# sideloaded package whose signer it does not trust, so this travels with it.
if [ -f "$APP/Gecko.cer" ]; then
  cp "$APP/Gecko.cer" "$OUT/Gecko.cer"
  cat > "$OUT/INSTALL.txt" <<'INSTALL'
Gecko for Windows 10 Mobile
===========================

The package needs nothing else installed: the C++ runtime it uses is inside
it, and there are no framework dependencies to fetch.

Two things the phone does need, and both are about the phone, not the package:

1. Trust the signing certificate. Copy Gecko.cer to the phone and open it,
   or install it through Interop Tools / WPinternals. Without this the
   installer refuses the package as untrusted, which is the usual reason a
   sideload fails on a device that has never seen this signer before.

2. Be interop-unlocked. The package asks for restricted capabilities --
   codeGeneration (the JIT, without which no modern web engine runs) and
   full file-system access for the profile. A stock, locked device will not
   grant them and the install fails.

Then install the .appx as usual (Device Portal, or an appx installer on the
phone). If an older build signed by a different publisher is installed, it is
a separate app: remove it if you do not want both.
INSTALL
  echo "    certificate and INSTALL.txt copied next to the package"
fi

echo
echo "PACKAGE: $PKG"
ls -la "$PKG" | awk '{printf "  %.1f MB\n", $5/1048576}'
