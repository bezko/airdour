# Airdour Windows-native build (MSVC)

## Locked audio target

- **WASAPI shared mode only** — `libs/backends/wasapi/`
- **Not** WASAPI exclusive / DirectSound / ASIO
- PortAudio is **not** on the default Windows-only product path

## Local prerequisites

1. Visual Studio 2022 (Desktop C++, MSVC v143) + Windows 10/11 SDK (x64)
2. CMake ≥ 3.20
3. Optional later: vcpkg manifest `tools/win-native/vcpkg.json` for core deps
4. **No** Steinberg ASIO SDK

## Quick smoke (what CI builds today)

```bat
cmake -S cmake\win64 -B build-win64 -G "Visual Studio 17 2022" -A x64 -DAIRDOUR_BUILD_WASAPI_IO_SMOKE=ON
cmake --build build-win64 --config Release --target wasapi_io_smoke
```

Produces `airdour_wasapi_io.lib` (WasapiIO + Windows SDK). Does **not** link libardour.

## Full product (not green in CI yet)

```bat
set PKG_CONFIG_PATH=C:\path\to\deps\lib\pkgconfig
python waf configure --dist-target=msvc --windows-only --prefix=C:\airdour-build
python waf build -j %NUMBER_OF_PROCESSORS%
```

Default backends: **dummy** + **wasapi**. Needs a full MSVC dependency prefix (see vcpkg.json).

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

Manifest for the next deps stage: `tools/win-native/vcpkg.json` (core only — no GTK2).

### Packaging scripts

- `tools/win-native/ci/Write-BuildStatus.ps1`
- `tools/win-native/ci/Package-Release.ps1`

## Bring-up checklist

1. [x] CI compiles WasapiIO with MSVC (smoke)
2. [ ] MSVC link `libpbd` → `libardour` → `hardour`
3. [ ] Dummy backend processes a session headless
4. [ ] Load `wasapi_audiobackend`; open shared-mode Default device
5. [ ] Tune period/CPU on a cheap laptop
6. [ ] Port WinMME MIDI; GUI / installer release

## Success for this phase

- Shared-mode device I/O through `ARDOUR::AudioBackend` on win64
- CI publishes reproducible win64 smoke/release zips without claiming a full GUI app
