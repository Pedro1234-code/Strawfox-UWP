# <img width="38" height="38" alt="geckologo" src="https://github.com/user-attachments/assets/b8563757-0adb-45a0-b608-0d0bf2716e39" /> Strawfox Browser

A **Gecko**-based browser for Xbox: the real Firefox engine, with **JIT
enabled**, inside a UWP app.

Join the XB Desktop Mode discord server: https://discord.gg/dEKYCqFXt

## Screenshots
<img width="1280" height="720" alt="image" src="https://github.com/user-attachments/assets/c812ee9e-2af5-41e2-9aa4-4676a378dfcd" /> <img width="1280" height="720" alt="image" src="https://github.com/user-attachments/assets/04b571b8-af8e-45f5-86f9-d58bd8de88f3" /> <img width="1280" height="720" alt="image" src="https://github.com/user-attachments/assets/603c17ae-45e5-4196-ab34-aad2473b56f8" /> <img width="1280" height="720" alt="image" src="https://github.com/user-attachments/assets/a9e62934-35a0-4db0-a12f-5cb636da29f1" />
Taken on Xbox Series S

## Features

Comparison with the Xbox's built-in Edge browser:

| Capability | Edge on Xbox | Strawfox |
| --- | --- | --- |
| USB mouse input | Native mouse support | Native mouse support |
| Memory available to the browser | Subject to Edge's Xbox memory limit | Up to 5 GB available to the app |
| Tabs in the background | No background multi-tab browsing | Multiple tabs can remain active in the background |
| Download files | Not available | Available* |
| Upload files | Not available | Available through the system file picker |
| Add-ons | Not available | Firefox add-ons supported |
| Developer tools | Not available | Inspector and Developer Tools included |

*On the first download, the app asks the user to choose a destination folder. Xbox requires this user-granted folder permission before the browser can export downloaded files.

## Limitations

- Can often freeze. The issues are being investigated.
- No sandbox, due to the nature of the UWP isolated environment.
- It first downloads to the LocalStorage and then moves to the user Downloads folder. Due to this, the file will appear as "Deleted" in the Downloads popup. This will be fixed.
- Gamepad navigation is still WIP. I strongly recommend to use a mouse and a keyboard.
- I can't guarantee compatibility with all addons.
- Some problems with video playback on movie sites.

## Issues

If you are facing any issues, open a ticket here on GitHub or on the Xbox Desktop Mode server. Always include the log files:

Instructions (Xbox):

1. Open Device Portal
2. Click on File Explorer.
3. Navigate to LocalAppData\Strawfox\LocalState
4. Download "gecko-boot.txt", "gecko.log", "pc-trace.bin".
5. Enter the profile folder and download "gecko-notes.log"
6. Send the downloaded logs on the Issue ticket message.

## How Gecko runs under UWP

Xbox apps run in the UWP app container, which by default blocks the three
things a browser engine needs. Each is handled:

| Need | Solution |
|---|---|
| **JIT / executable memory** | The `codeGeneration` restricted capability + `VirtualAllocFromApp` / `VirtualProtectFromApp`. A force-included compat header remaps the allocators tree-wide; ARM I-cache flushing is added by a small wrapper. |
| **No child processes** | Gecko is built **single-process** (`MOZ_FORCE_DISABLE_E10S=1`). |
| **Storage / filesystem** | Uses the app's LocalStorage for Downloads, profile, addons, etc. |

Full detail: [`docs/PORTING-W10M.md`](docs/PORTING-W10M.md).

## Layout

```
app/GeckoW10m/        C++/WinRT UWP shell (the browser UI)
  ├─ App.cpp          application entry
  ├─ MainPage.*       address bar, nav, tabs, engine host
  ├─ client/          core models (preferences, search, tabs)
  ├─ engine/          GeckoEngine wrapper + gecko_capi C ABI
  └─ Package.appxmanifest   declares codeGeneration + restricted capabilities
mozconfig/            engine build configurations (x64 UWP, plus legacy ARM32)
patches/w10m/         historical ARM32 port patches
tools/                Gecko build tools and legacy packaging scripts
vendor/               mobile-config-firefox: the phone UI theme and default prefs
engine/firefox/       Gecko submodule pinned to the Strawfox fork
```

## Build for Xbox (x64)

Strawfox has two build steps: compile the Gecko engine, then build and package
the standalone UWP host in Visual Studio. The ARM32/Windows Phone scripts and
patches in this repository are historical references.

### Prerequisites

- Windows x64, Git, and [MozillaBuild](https://firefox-source-docs.mozilla.org/setup/windows_build.html).
- Visual Studio with C++ desktop and UWP tooling, Windows SDK `10.0.26100.0`,
  and the x64 VCLibs Desktop 14.0 Extension SDK. The engine scripts look for an
  installed x64 MSVC toolset (v145 in the current setup); the host project
  targets v143, so install the v143 x64/x86 build tools and v143 UWP C++ tools
  as well.
- LLVM with `clang-cl.exe` and `lld-link.exe`, plus NASM, GNU Make, Node.js,
  `sccache`, and `cbindgen` 0.29.4 or newer.
- Rust through rustup, with a nightly toolchain and its `rust-src` component.
  The engine uses the `x86_64-uwp-windows-msvc` target and builds its Rust
  standard library from source.
- The x64 Windows App SDK runtime DLLs. Use the redistributable pinned in
  [Gecko's toolchain definitions](https://github.com/Pedro1234-code/firefox/blob/strawfox-uwp/taskcluster/kinds/fetch/toolchains.yml)
  (search for `windowsappsdk-redist`). Extract the x64 runtime MSIX from it,
  then copy the DLLs listed in
  [Gecko's Windows App SDK extraction script](https://github.com/Pedro1234-code/firefox/blob/strawfox-uwp/taskcluster/scripts/misc/build-WindowsAppSDK.sh)
  into the directory you set as `MOZ_WINDOWS_APP_SDK_DIR`.

### 1. Clone the source and build Gecko

Run these commands in the MozillaBuild shell (`C:\mozilla-build\start-shell.bat`).
Use short paths for the checkout and object directory to avoid path length
errors when compiling WebRTC.

```bash
cd /c
git clone https://github.com/Pedro1234-code/Strawfox-UWP.git strawfox
cd strawfox
git submodule update --init --depth 1 engine/firefox
```

The submodule pins the matching revision of the
[Strawfox Gecko fork](https://github.com/Pedro1234-code/firefox/tree/strawfox-uwp).
Use that revision rather than an upstream Firefox checkout.

```bash
rustup toolchain install nightly --component rust-src
rustup override set nightly
cargo install cbindgen --locked
cargo install sccache --locked

export GECKO_W10M_OBJ=C:/rw-obj
export GECKO_W10M_SCCACHE="$(cygpath -m "$USERPROFILE")/.cargo/bin"
export MOZ_WINDOWS_APP_SDK_DIR=C:/firefox-deps/windowsappsdk-x64

bash tools/prepare-windows-rs.sh
bash tools/uwp-install-std.sh
bash tools/browser-build.sh
```

Adjust the paths for your machine before running the scripts. See
[`tools/env.sh`](tools/env.sh) for defaults and overrides, including
`GECKO_W10M_VC` (the toolset directory containing `bin/Hostx64/x64/cl.exe`),
`GECKO_W10M_LLVM`, `GECKO_W10M_NASM`, `GECKO_W10M_GMAKE`, and
`GECKO_W10M_JOBS`. If the configured GNU Make binary has a different name or
location, set `GMAKE` to its full path. If the SDK's `dxcompiler.dll` is in a
different location, set `MOZ_DXCOMPILER_PATH` to the x64 DLL.

The build uses [the x64 UWP mozconfig](mozconfig/mozconfig.x64-uwp-browser)
and should finish with `=== build exit=0`. Check for `xul.dll` and
`omni.ja` under `C:\rw-obj\dist\bin`. Reuse the object directory for
incremental builds. Rebuild the Rust standard library after changing the
nightly toolchain.

### 2. Build and package the standalone app

1. Open [`app/GeckoW10m.sln`](app/GeckoW10m.sln) and select **Release | x64**
   (or **Debug | x64** when debugging the host).
2. Build the solution. The project consumes Gecko's `dist\bin`,
   `dist\include`, and `dist\lib` directly; you do not need to run
   `mach package`. If you used a different object directory or LLVM location,
   set the MSBuild properties `GeckoW10mObj` and `GeckoW10mClangCl`, or set
   those environment variables before opening Visual Studio.
3. Use **Publish > Create App Packages** and choose sideloading. Configure your
   own test signing certificate whose subject matches the package manifest's
   `Publisher`. The certificate thumbprint in the project is specific to the
   original build machine. Use the generated x64 package and its dependencies
   to deploy to your Xbox in Developer Mode.

If you change only the standalone host, rebuild the Visual Studio project
against the existing Gecko output. If you change Gecko, rebuild the engine
and regenerate the UWP package so it includes the updated runtime.

### Contributing engine changes

Push engine changes to the Strawfox Gecko fork first. Then commit the updated
`engine/firefox` submodule pointer in this repository so other checkouts can
fetch the same engine revision.

## Credits

Mozzila, for Firefox.

Computershik73 for the ARM32 UWP port.

Codex.

Whoever found the mouse workaround on Xbox.

## License

Shell code: GPL-3.0. `patches/` and the engine branch (Gecko modifications): MPL-2.0.
