# Airdour Windows-native build (MSVC)

## Locked audio target

- **WASAPI shared mode only** — `libs/backends/wasapi/`
- **Not** WASAPI exclusive / DirectSound / ASIO
- PortAudio is **not** on the default Windows-only product path

## Local prerequisites

1. Visual Studio 2022+ (Desktop C++, MSVC) + Windows 10/11 SDK (x64)
2. CMake ≥ 3.20
3. vcpkg at a **short path** (recommended `C:\vcpkg`) — see `Install-HeadlessDeps.ps1`
4. **No** Steinberg ASIO SDK; **no** GTK2 / `libs/tk` / Cairo canvas (GUI is `win_ardour` Direct2D)

## Quick smoke (what CI builds)

Run from a VS Developer PowerShell (`ilammy/msvc-dev-cmd` does this in CI):

```bat
cd cmake\win64
cmake --preset ui
cmake --build --preset ui
ctest --preset ui
cd ..\..
build-ninja-ui\artifacts\airdour_win_shell.exe --timeout 3 --no-engine
```

Presets live in `cmake\win64\CMakePresets.json` (`ui`, `engine`, `vst2-bridge-x86`, all Ninja + Release).
Produces `airdour_wasapi_io.lib` (WasapiIO + Windows SDK) and `airdour_win_shell.exe` (Win32+D2D+DirectWrite, no GTK). The `ui` preset does **not** link libardour.

### VST2-32 helper

`ui` and `engine` are x64. They do not build `ardour-vst2-bridge-x86.exe`. Ninja does not accept `-A Win32`; it compiles with the `cl.exe` on PATH, and the helper option refuses a compiler that is not x86.

From an **x86** VS developer prompt (`vcvarsall.bat x86`):

```bat
cd cmake\win64
cmake --preset vst2-bridge-x86
cmake --build --preset vst2-bridge-x86
```

Output: `build-win32-vst2-bridge\artifacts\ardour-vst2-bridge-x86.exe` (i386). Copy that exe next to `airdour_win_shell.exe` to exercise the bridge from a build tree. The portable packager stages this path when the PE machine is i386. It does not take a helper from the x64 shell tree.

Visual Studio generators do accept `-A`. Use the same output directory so packaging finds it:

```bat
cmake -S cmake\win64 -B build-win32-vst2-bridge -G "Visual Studio 17 2022" -A Win32 ^
  -DAIRDOUR_BUILD_VST2_BRIDGE_X86=ON -DAIRDOUR_BUILD_WIN_SHELL=OFF -DAIRDOUR_BUILD_WASAPI_IO_SMOKE=OFF
cmake --build build-win32-vst2-bridge --config Release --target ardour-vst2-bridge-x86
```

That writes `build-win32-vst2-bridge\artifacts\Release\ardour-vst2-bridge-x86.exe`. Passing `-DAIRDOUR_BUILD_WIN_SHELL=ON` (or the wasapi smoke option) on either command still builds those targets; the preset turns them off so this tree is only the helper.

The MSVC targets get `/utf-8 /Zc:__cplusplus /permissive- /W4`; set
`-DAIRDOUR_WARNINGS_AS_ERRORS=ON` to add `/WX`.

## Headless C deps

```bat
powershell -ExecutionPolicy Bypass -File tools\win-native\Install-HeadlessDeps.ps1 -VcpkgRoot C:\vcpkg
```

CMake finds vcpkg via `AIRDOUR_VCPKG_PREFIX`, `VCPKG_INSTALLED_DIR`, or the in-repo `vcpkg_installed/x64-windows` (in that order).

### Blocker before `libpbd`

Stock vcpkg `glibmm` / `libsigcpp` are **2.68+/3.x**. Ardour needs **glibmm-2.4 + sigc++-2.0** — provided by overlay ports `glibmm24` + `libsigcpp2` (`tools/win-native/overlays/`). Do not enable feature `cxx-glib-modern-incompatible` for product builds.

## Full product (engine-linked shell)

```bat
tools\win-native\configure-waf-release.cmd build-rel
tools\win-native\build-waf-release.cmd build-rel
cd cmake\win64
cmake --preset engine
cmake --build --preset engine
ctest --preset engine
cd ..\..
```

The scripts derive the repo root from their own location, find VS via `vswhere`,
honor `VCPKG_INSTALLED_DIR`, and accept an output dir. `engine` refuses to
configure when `build-rel\libs\ardour\ardour.lib` is missing instead of silently
building a UI-only exe, and stages the waf DLLs at build time.

Default backends: **dummy** + **wasapi**. Needs the MSVC deps above **plus** the glibmm-2.4 overlay.

Run the suite against any configured tree: `tools\win-native\run-smoke-suite.cmd [build-dir]`.

## Formatting

`.clang-format` / `.clang-tidy` target `win_ardour` (upstream `libs/` keeps its own style).
CI checks only the lines changed in the branch (legacy files are never reformatted wholesale):

```bat
pwsh tools\win-native\ci\Format-Check.ps1 -BaseRef origin/master
pwsh tools\win-native\ci\Format-Check.ps1 -BaseRef origin/master -Fix   :: rewrite changed lines
```

## GitHub Actions

| Workflow | File | When | Produces |
|----------|------|------|----------|
| Format check | `.github/workflows/windows-msvc.yml` (`format`) | push/PR | clang-format check on changed `win_ardour` files |
| Windows MSVC CI | `.github/workflows/windows-msvc.yml` | push/PR/`workflow_dispatch` | CTest smoke suite + artifact `.lib`, `.exe`, `BUILD_STATUS.txt` |
| Windows Engine | `.github/workflows/windows-engine.yml` | manual + weekly | vcpkg + waf + engine shell + CTest suite |
| Windows MSVC Release | `.github/workflows/windows-release.yml` | tag `v*` or manual | Zip on Actions; GitHub Release on tags |

### Secrets / deps

| Item | Required today? | Notes |
|------|-----------------|-------|
| `GITHUB_TOKEN` | Auto | Release publish on tags |
| `VCPKG_BINARY_SOURCES` | No (future) | Binary cache when vcpkg job is enabled |
| Code signing | No | Not wired |

Manifest: `tools/win-native/vcpkg.json` (core headless deps; **no GTK2 chrome**).  
Optional MSVC **Cairo** shell blit (not product UI): `-WithGtkLibs` → feature `gtk-libs`.  

### Packaging scripts

- `tools/win-native/ci/Write-BuildStatus.ps1`
- `tools/win-native/ci/Package-Release.ps1`
- `tools/win-native/ci/Package-WinShell-Nsis.ps1` (open-source NSIS installer)

## Success for this phase

- Shared-mode device I/O through `ARDOUR::AudioBackend` on win64
- CI publishes reproducible win64 smoke/release zips
