# GTK lib probe (MSVC — no chrome)

Proves **GTK3 + Cairo** from `tools/win-native/vcpkg_installed` link under MSVC.
Creates a Cairo **image surface** and resolves `gtk_widget_get_type` — **no windows, no themes**.

Product UI stays **Win32 + Direct2D** (`win_ardour`).

## Install deps

```powershell
powershell -ExecutionPolicy Bypass -File tools\win-native\Install-HeadlessDeps.ps1 `
  -VcpkgRoot C:\vcpkg -WithGtkLibs
```

Manifest feature: `gtk-libs` in `tools/win-native/vcpkg.json` (pulls `gtk3`, `cairo`, `pango`).

## Build + run

```bat
tools\win-native\gtk-lib-probe\build-and-run.cmd
```

Expect `PASS` and `EXIT=0` (GTK 3.24.x + Cairo image surface; no windows).

Verified on TUFNUTZ: `gtk 3.24.52`, `cairo 1.18.6`.
