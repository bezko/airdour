; Airdour Windows installer
; Compiler: NSIS 3 (open source, https://nsis.sourceforge.io/)
; Payload: the portable shell tree from Package-WinShell-Portable.ps1
;   (UiOnly on GitHub Actions, Engine when that stage is packaged on a laptop).
;
;   makensis /DVERSION=0.1.0 /DPRODUCT_VERSION=0.1.0.0 ^
;     /DSOURCE_DIR=C:/build/shell-stage ^
;     /DOUT_FILE=C:/build/airdour-setup.exe ^
;     /DLICENSE_FILE=C:/src/COPYING ^
;     airdour.nsi
;
; Silent per-user install (no UAC). /D= must be last and must not be quoted:
;   airdour-setup.exe /S /D=C:\Airdour
; Silent uninstall that stays in this process (so a script can wait on it):
;   Uninstall.exe /S _?=C:\Airdour

Unicode true
; x86-unicode stub (the default). It installs the 64-bit shell per-user.
; GitHub-hosted runners ship NSIS without the amd64 stub, so Target amd64-unicode
; fails there. Per-user files go to %LOCALAPPDATA%, which is not WOW64-redirected.
!include "LogicLib.nsh"
!include "x64.nsh"
!include "WinVer.nsh"
!include "FileFunc.nsh"
!include "MUI2.nsh"

!ifndef VERSION
  !define VERSION "0.1.0"
!endif
!ifndef PRODUCT_VERSION
  !define PRODUCT_VERSION "0.1.0.0"
!endif
!ifndef SOURCE_DIR
  !define SOURCE_DIR "..\..\dist\shell-stage"
!endif
!ifndef OUT_FILE
  !define OUT_FILE "..\..\dist\airdour-setup.exe"
!endif
!ifndef LICENSE_FILE
  !define LICENSE_FILE "..\..\..\COPYING"
!endif

; Paths are relative to this script (makensis cds here). Absolute D:/ paths make
; compile-time /FileExists miss the payload on the Windows NSIS that ships on
; GitHub-hosted runners.
!if /FileExists "${SOURCE_DIR}/airdour_win_shell.exe"
!else
  !error "SOURCE_DIR is missing airdour_win_shell.exe (${SOURCE_DIR})"
!endif
!if /FileExists "${LICENSE_FILE}"
!else
  !error "LICENSE_FILE not found (${LICENSE_FILE})"
!endif

!define APP_NAME "Airdour"
!define APP_KEY "Software\Airdour"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Airdour"

Name "${APP_NAME} ${VERSION}"
OutFile "${OUT_FILE}"
InstallDir "$LOCALAPPDATA\Airdour"
InstallDirRegKey HKCU "${APP_KEY}" "InstallDir"
RequestExecutionLevel user
ShowInstDetails show
ShowUnInstDetails show
ManifestDPIAware true
ManifestSupportedOS Win10

VIProductVersion "${PRODUCT_VERSION}"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "FileDescription" "Airdour setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "GPL-2.0"

!define MUI_ABORTWARNING
!define MUI_UNABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Airdour Setup"
!define MUI_WELCOMEPAGE_TEXT "This installs Airdour for the current user. No administrator prompt.$\r$\n$\r$\nDefault folder: %LOCALAPPDATA%\Airdour$\r$\n$\r$\nThe Windows shell is Win32 + Direct2D. Audio is WASAPI shared mode. GitHub Actions builds install the UI shell; a machine with the engine stage installs Dummy, WASAPI, and the bundled plugins that were packaged with it.$\r$\n$\r$\nAirdour is free software under the GNU GPL v2."
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Launch Airdour"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchAirdour
!define MUI_FINISHPAGE_NOAUTOCLOSE

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${LICENSE_FILE}"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  SetShellVarContext current
  SetRegView 64
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "Airdour requires 64-bit Windows."
    Abort
  ${EndIf}
  ${IfNot} ${AtLeastWin10}
    MessageBox MB_ICONSTOP "Airdour requires Windows 10 or later."
    Abort
  ${EndIf}
FunctionEnd

Function un.onInit
  SetShellVarContext current
  SetRegView 64
FunctionEnd

Function LaunchAirdour
  ExecShell "open" "$INSTDIR\run-airdour.bat"
FunctionEnd

Section "Airdour" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  SetOverwrite on
  File /r "${SOURCE_DIR}/*.*"
  ; COPYING has no extension; keep the GPL text in the install folder
  ; even if the wildcard above skipped it.
  File "/oname=COPYING" "${LICENSE_FILE}"

  WriteUninstaller "$INSTDIR\Uninstall.exe"

  WriteRegStr HKCU "${APP_KEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${APP_KEY}" "Version" "${VERSION}"

  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "${APP_NAME}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\airdour_win_shell.exe"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
  WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" "$\"$INSTDIR\Uninstall.exe$\" /S"
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "URLInfoAbout" "https://github.com/bezko/airdour"
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" "$0"

  CreateDirectory "$SMPROGRAMS\Airdour"
  CreateShortcut "$SMPROGRAMS\Airdour\Airdour.lnk" "$INSTDIR\run-airdour.bat" "" "$INSTDIR\airdour_win_shell.exe" 0
  CreateShortcut "$SMPROGRAMS\Airdour\Uninstall Airdour.lnk" "$INSTDIR\Uninstall.exe"
SectionEnd

Section /o "Desktop shortcut" SecDesktop
  CreateShortcut "$DESKTOP\Airdour.lnk" "$INSTDIR\run-airdour.bat" "" "$INSTDIR\airdour_win_shell.exe" 0
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "Application files, Start menu shortcut, and uninstaller."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "Also put an Airdour shortcut on the desktop."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  Delete "$DESKTOP\Airdour.lnk"
  Delete "$SMPROGRAMS\Airdour\Airdour.lnk"
  Delete "$SMPROGRAMS\Airdour\Uninstall Airdour.lnk"
  RMDir "$SMPROGRAMS\Airdour"

  Delete "$INSTDIR\Uninstall.exe"
  RMDir /r "$INSTDIR"

  DeleteRegKey HKCU "${UNINST_KEY}"
  DeleteRegKey HKCU "${APP_KEY}"
SectionEnd
