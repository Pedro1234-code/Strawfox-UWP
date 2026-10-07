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
mozconfig/            the engine's build configuration (arm-uwp, single-process, JIT)
patches/w10m/         the engine port as git format-patch files over the release tag
tools/                build scripts (build-all.sh runs the whole chain)
vendor/               mobile-config-firefox: the phone UI theme and default prefs
engine/firefox/       Gecko submodule: the port's branch w10m-port, on Firefox 155.0.1
```

## Build

The build runs on x64 Windows. The tools below are the
versions it is known to build with.

### Requirements

| Tool | Version | Notes |
|---|---|---|
| Visual Studio 2026 | 17.14 (MSVC 14.44) | Workload "Desktop development with C++" plus the individual component **MSVC v143 ARM build tools**. 17.14 is the last release with ARM32 tools. |
| Windows 10/11 SDK | 10.0.26100.0 | Headers, tools, cppwinrt. The newer 26100 dropped ARM32 libraries. |
| VCLibs x64 | 14.00 | `Microsoft.VCLibs.arm.14.00.appx` under `C:\Program Files (x86)\Microsoft SDKs\Windows Kits\10\ExtensionSDKs\Microsoft.VCLibs\14.0`, installed with the UWP workload of Visual Studio. The package carries its CRT from it. |
| VCLibs Desktop x64 | 14.00 | `Microsoft.VCLibs.Desktop.arm.14.00.appx` under `C:\Program Files (x86)\Microsoft SDKs\Windows Kits\10\ExtensionSDKs\Microsoft.VCLibs.Desktop\14.0`, installed with the UWP workload of Visual Studio. |
| LLVM | 22.1 | clang-cl and lld-link, installed in `C:\Program Files\LLVM`. |
| MozillaBuild | 4.2 | In `C:\mozilla-build`. Run the scripts from its `start-shell.bat` or any bash that has it on PATH. |
| Rust | nightly (built with 1.96.0-nightly 2026-03-11) | The default toolchain, with the `rust-src` component: `rustup default nightly-2026-03-11`, `rustup component add rust-src`. The std for the ARM32 UWP target is compiled from source by `tools/uwp-install-std.sh`. |
| cbindgen | 0.29 | `cargo install cbindgen` |
| Node.js | 24 | On PATH. |

All locations can be changed with environment variables; see the top of
[`tools/env.sh`](tools/env.sh). The engine's object directory is `C:\rw-obj` by default
(`GECKO_W10M_OBJ`); keep its path short, or libwebrtc's deepest objects exceed MAX_PATH.
About 25 GB of free space are needed for it.

### Get the engine

Either way gives the same tree:

```bash
# a) the submodule, from the fork that carries the port's branch
git submodule update --init --depth 1 engine/firefox

# b) or plain Firefox at the tag in engine/release.txt, plus patches/w10m
git clone --depth 1 --branch FIREFOX_155_0_1_RELEASE https://github.com/mozilla-firefox/firefox engine/firefox
bash tools/apply-patches-w10m.sh      # or: tools\apply-patches-w10m.ps1
```

### Build the package

```bash
bash tools/build-all.sh
```

That fetches and patches the `windows` crate (`tools/prepare-windows-rs.sh`), builds the
Rust std for the target once (`tools/uwp-install-std.sh`), builds the engine
(`tools/browser-build.sh`), packages it (`tools/package.sh`) and builds, packs and signs
the appx (`tools/build-appx.sh`). The package lands in `app/GeckoW10m/AppPackages/`,
together with the certificate to trust on the phone and `INSTALL.txt`. A full engine build
takes about half an hour on 16 cores; after that the steps can be run one by one.

### Signing

I suggest you to create your own certificate on Visual Studio. In order for native USB mouse to work on Xbox, you must use a certificate with Microsoft's CN and Microsoft Edge package identity name (Microsoft.MicrosoftEdge.Name for example).


### Changing the engine

Commit to the `w10m-port` branch in `engine/firefox`, then refresh the patches, which also
checks that they rebuild the branch exactly, and commit them with the submodule:

```bash
bash tools/export-patches-w10m.sh
```

### Older scripts

`tools/build-gecko-uwp.ps1`, `uwp-configure.sh`, `uwp-build.sh`, `uwp-build-std.sh` and
`mozconfig/mozconfig.arm-uwp` are the SpiderMonkey-only bring-up from before the whole
browser built; they are kept for reference and are not part of the build above.

## Credits

Mozzila, for Firefox.

Computershik75 for the ARM32 UWP port.

Codex.

Whoever found the mouse workaround on Xbox.

## License

Shell code: GPL-3.0. `patches/` and the engine branch (Gecko modifications): MPL-2.0.
