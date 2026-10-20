; Airdour Windows installer (Inno Setup 6)
; Brand: Airdour — Win32 + Direct2D/DirectWrite shell (no GTK).
; Payloads: same tree as Package-WinShell-Portable.ps1 Engine Release zip.
;
; Compile via Package-WinShell-Installer.ps1 (passes SourceDir / OutName / Version).

#ifndef MyAppVersion
  #define MyAppVersion "0.1.0"
#endif
#ifndef MyAppSourceDir
  #define MyAppSourceDir "..\..\dist\shell-stage"
#endif
#ifndef MyAppOutputDir
  #define MyAppOutputDir "..\..\dist"
#endif
#ifndef MyAppOutputBase
  #define MyAppOutputBase "airdour-v0.1.0-rel-win64-setup"
#endif

[Setup]
AppId={{A1B2C3D4-E5F6-4789-A012-3456789ABCDE}}
AppName=Airdour
AppVersion={#MyAppVersion}
AppVerName=Airdour {#MyAppVersion}
AppPublisher=Airdour
AppPublisherURL=https://github.com/bezko/airdour
DefaultDirName={autopf}\Airdour
DefaultGroupName=Airdour
DisableProgramGroupPage=yes
LicenseFile=
InfoBeforeFile=
OutputDir={#MyAppOutputDir}
OutputBaseFilename={#MyAppOutputBase}
Compression=lzma2/fast
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog commandline
UninstallDisplayIcon={app}\airdour_win_shell.exe
UninstallDisplayName=Airdour
SetupIconFile=
DisableWelcomePage=no
AllowNoIcons=yes
ChangesAssociations=no
CloseApplications=yes
RestartApplications=no
MinVersion=10.0

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional icons:"; Flags: unchecked

[Files]
; Entire portable Engine Release tree (exe, DLLs, backends, LV2, README, …).
Source: "{#MyAppSourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Airdour"; Filename: "{app}\run-airdour.bat"; WorkingDir: "{app}"; IconFilename: "{app}\airdour_win_shell.exe"; Comment: "Airdour — Windows native DAW shell"
Name: "{group}\Uninstall Airdour"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Airdour"; Filename: "{app}\run-airdour.bat"; WorkingDir: "{app}"; IconFilename: "{app}\airdour_win_shell.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\run-airdour.bat"; Description: "Launch Airdour"; Flags: nowait postinstall skipifsilent shellexec

[UninstallDelete]
Type: filesandordirs; Name: "{app}"
