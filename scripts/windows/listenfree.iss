#ifndef StageDir
  #error StageDir is required
#endif
#ifndef ReleaseVersion
  #define ReleaseVersion "0.3.2"
#endif
[Setup]
AppId={{C9DFBE62-EA54-44D9-AE71-378F2715325D}
AppName=ListenFree
AppVersion={#ReleaseVersion}
AppPublisher=ListenFree
DefaultDirName={localappdata}\Programs\ListenFree
DefaultGroupName=ListenFree
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=ListenFree-{#ReleaseVersion}-windows-x64-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\..\music_player_desktop\assets\icons\app.ico
UninstallDisplayIcon={app}\listenfree.exe
CloseApplications=yes
RestartApplications=no
DisableProgramGroupPage=yes

[Languages]
Name: chinesesimplified; MessagesFile: "ChineseSimplified.isl"

[Tasks]
Name: desktopicon; Description: "创建桌面快捷方式"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Excludes: "portable.mode"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\ListenFree"; Filename: "{app}\listenfree.exe"; WorkingDir: "{app}"; AppUserModelID: "ListenFree.Desktop"
Name: "{autodesktop}\ListenFree"; Filename: "{app}\listenfree.exe"; WorkingDir: "{app}"; AppUserModelID: "ListenFree.Desktop"; Tasks: desktopicon

[Run]
Filename: "{app}\listenfree.exe"; Description: "启动 ListenFree"; Flags: nowait postinstall skipifsilent

; Uninstall removes only installed files. User databases and credentials are
; managed by the application and deliberately absent from the installer.
