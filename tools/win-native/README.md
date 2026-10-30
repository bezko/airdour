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

## Quick smoke (what CI builds today)

```bat
cmake -S cmake\win64 -B build-win64 -G "Visual Studio 17 2022" -A x64 -DAIRDOUR_BUILD_WASAPI_IO_SMOKE=ON -DAIRDOUR_BUILD_WIN_SHELL=ON
cmake --build build-win64 --config Release --target wasapi_io_smoke
cmake --build build-win64 --config Release --target airdour_win_shell
build-win64\artifacts\Release\airdour_win_shell.exe --timeout 3
```

Produces `airdour_wasapi_io.lib` (WasapiIO + Windows SDK) and `airdour_win_shell.exe` (Win32+D2D+DirectWrite, no GTK).

## Headless C deps (next slice on laptop)

```bat
powershell -ExecutionPolicy Bypass -File tools\win-native\Install-HeadlessDeps.ps1 -VcpkgRoot C:\vcpkg

cmake -S cmake\win64 -B build-win64-deps -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_MANIFEST_DIR=%CD%/tools/win-native ^
  -DAIRDOUR_BUILD_WASAPI_IO_SMOKE=ON
```

### Blocker before `libpbd`

Stock vcpkg `glibmm` / `libsigcpp` are **2.68+/3.x**. Ardour needs **glibmm-2.4 + sigc++-2.0** — provided by overlay ports `glibmm24` + `libsigcpp2` (`tools/win-native/overlays/`). Do not enable feature `cxx-glib-modern-incompatible` for product builds.

## Full product (not green in CI yet)

```bat
set PKG_CONFIG_PATH=C:\path\to\deps\lib\pkgconfig
python waf configure --dist-target=msvc --windows-only --prefix=C:\airdour-build
python waf build -j %NUMBER_OF_PROCESSORS%
```

Default backends: **dummy** + **wasapi**. Needs MSVC deps above **plus** the glibmm-2.4 overlay.

## GitHub Actions

| Workflow | File | When | Produces |
|----------|------|------|----------|
| Windows MSVC CI | `.github/workflows/windows-msvc.yml` | push/PR/`workflow_dispatch` | Artifact: smoke `.lib` + `BUILD_STATUS.txt` |
| Windows MSVC Release | `.github/workflows/windows-release.yml` | tag `v*` or manual | Zip on Actions; GitHub Release on tags |

### Secrets / deps

| Item | Required today? | Notes |
|------|-----------------|-------|
| `GITHUB_TOKEN` | Auto | Release publish on tags |
| `VCPKG_BINARY_SOURCES` | No (future) | Binary cache when vcpkg job is enabled |
| Code signing | No | Not wired |
| GTK2 MSVC prefix | Blocked | Main GUI release blocker |

Manifest: `tools/win-native/vcpkg.json` (core headless deps; **no GTK2 chrome**).  
Optional MSVC **GTK3/Cairo libs** (not product UI): `-WithGtkLibs` → feature `gtk-libs`.  

### Packaging scripts

- `tools/win-native/ci/Write-BuildStatus.ps1`
- `tools/win-native/ci/Package-Release.ps1`
- `tools/win-native/ci/Package-WinShell-Nsis.ps1` (open-source NSIS installer)

## Bring-up checklist

1. [x] CI compiles WasapiIO with MSVC (smoke)
2. [x] Laptop: vcpkg headless C deps
3. [x] Overlay `glibmm-2.4` + `sigc++-2.0` installed; waf past glibmm
4. [x] waf Windows-only configure green; MSVC link `libpbd` (`build\libs\pbd\pbd.dll`)
5. [x] MSVC link `libardour` → `hardour` (dummy + WASAPI) — laptop
6. [x] hardour engine-only smoke (dummy + WASAPI shared)
7. [x] Minimal Win32+D2D shell (`airdour_win_shell`) — laptop + CI target
8. [ ] Tune period/CPU on a cheap laptop; measured WasapiIO latency
9. [x] Port WinMME MIDI; grow `win_ardour` (engine attach, wave strip, FX)
10. [x] Windows-only cull: waf/cmake never require `libs/tk`, `gtk2_ardour`, Cairo canvas
11. [x] NSIS per-user installer (`tools/win-native/installer/airdour.nsi`); [ ] MSIX. Engine portable zip via `Package-WinShell-Portable.ps1`
8. [x] Port WinMME MIDI; grow `win_ardour` (engine attach, wave strip, FX)
9. [x] Windows-only cull: waf/cmake never require `libs/tk`, `gtk2_ardour`, Cairo canvas
10. [x] Engine portable zip already ships via `Package-WinShell-Portable.ps1`

## Success for this phase

- Shared-mode device I/O through `ARDOUR::AudioBackend` on win64
- CI publishes reproducible win64 smoke/release zips without claiming a full GUI app
