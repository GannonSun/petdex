#define MyAppName "Petdex"
#define MyAppVersion "0.1.0"
#define MyAppPublisher "Petdex"
#define MyAppExeName "petdex-desktop.exe"

[Setup]
AppId={{F2EF6DA9-9488-45A4-9BDE-3C629C0AD501}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\Programs\Petdex
DefaultGroupName={#MyAppName}
DisableDirPage=no
DisableProgramGroupPage=no
PrivilegesRequired=lowest
OutputDir=..\dist\installer
OutputBaseFilename=PetdexSetup-{#MyAppVersion}
Compression=lzma
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#MyAppExeName}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked
Name: "launch"; Description: "Launch Petdex after installation"; GroupDescription: "After installation:"; Flags: unchecked

[Dirs]
Name: "{localappdata}\.petdex"
Name: "{localappdata}\.petdex\pets"
Name: "{localappdata}\.petdex\pets\bao"

[Files]
Source: "..\zig-out\bin\petdex-desktop.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "default-pets\bao\*"; DestDir: "{localappdata}\.petdex\pets\bao"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Petdex"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall Petdex"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Petdex"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch Petdex"; Flags: nowait postinstall skipifsilent; Tasks: launch

