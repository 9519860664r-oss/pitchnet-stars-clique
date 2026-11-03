; PitchNet Stars Clique Edition: Windows installer (Inno Setup 6).
;
; Build the Release configuration first, then:
;   ISCC.exe packaging\windows\PitchNetStarsClique.iss
; The installer lands in Output\.

#define MyAppName "PitchNet Stars Clique Edition"
#define MyAppShortName "PitchNet"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "Stars Clique"
#define MyAppURL "https://github.com/9519860664r-oss/pitchnet-stars-clique"
#define MyPluginName "PitchNet (Stars Clique ver)"
#define MyAppExeName "PitchNet (Stars Clique ver).exe"

[Setup]
; Paths below are relative to the repository root.
SourceDir=..\..
OutputDir=Output
OutputBaseFilename=PitchNet-StarsClique-{#MyAppVersion}-Setup
AppId={{8F3C2B1E-6A4D-4E9B-9C57-5A1D2E7B3F90}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
AppCopyright=PitchNet (C) Session Loops and contributors. Modifications (C) 2026 Goth Angel, Stars Clique. AGPLv3.
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} installer
DefaultDirName={commonpf64}\Stars Clique\PitchNet
DefaultGroupName=Stars Clique\PitchNet
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
MinVersion=10.0
SetupIconFile=build\PitchNet_artefacts\JuceLibraryCode\icon.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}
WizardStyle=modern
WizardImageFile=packaging\windows\wizard-image.bmp
WizardSmallImageFile=packaging\windows\wizard-small.bmp
WizardImageStretch=yes
LicenseFile=LICENSE
InfoBeforeFile=NOTICE.md
Compression=lzma2/max
SolidCompression=yes
CloseApplications=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Types]
Name: "full"; Description: "Plug-in and standalone app"
Name: "plugin"; Description: "VST3 plug-in only"
Name: "custom"; Description: "Custom"; Flags: iscustom

[Components]
Name: "models"; Description: "Neural models (required)"; Types: full plugin custom; Flags: fixed
Name: "vst3"; Description: "VST3 plug-in"; Types: full plugin custom
Name: "app"; Description: "Standalone app"; Types: full
Name: "docs"; Description: "User manuals (PDF, English and Russian)"; Types: full plugin custom

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Components: app; Flags: unchecked

[Files]
; VST3 plug-in bundle
Source: "build\PitchNetPlugin_artefacts\Release\VST3\{#MyPluginName}.vst3\*"; DestDir: "{commoncf64}\VST3\Stars Clique\{#MyPluginName}.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs

; Standalone app
Source: "build\PitchNet_artefacts\Release\{#MyAppExeName}"; DestDir: "{app}"; Components: app; Flags: ignoreversion
Source: "build\PitchNet_artefacts\Release\onnxruntime.dll"; DestDir: "{app}"; Components: app; Flags: ignoreversion

; Neural models: PitchNet looks for them here. Shared with the original
; PitchNet, so they are reference-counted rather than removed outright.
Source: "Resources\models\*"; DestDir: "{commonappdata}\Session Loops\PitchNet\models"; Components: models; Flags: ignoreversion recursesubdirs createallsubdirs sharedfile uninsnosharedfileprompt

; Fonts used by the interface
Source: "Resources\fonts\Montserrat-Medium.ttf"; DestDir: "{commonfonts}"; FontInstall: "Montserrat Medium"; Components: vst3 app; Flags: onlyifdoesntexist uninsneveruninstall
Source: "Resources\fonts\Montserrat-Regular.ttf"; DestDir: "{commonfonts}"; FontInstall: "Montserrat"; Components: vst3 app; Flags: onlyifdoesntexist uninsneveruninstall

; Documents
Source: "docs\PitchNet-StarsClique-Manual-EN.pdf"; DestDir: "{app}\docs"; Components: docs; Flags: ignoreversion
Source: "docs\PitchNet-StarsClique-Manual-RU.pdf"; DestDir: "{app}\docs"; Components: docs; Flags: ignoreversion
Source: "LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "NOTICE.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "THIRD_PARTY_NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "CHANGELOG.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Components: app
Name: "{group}\User manual (English)"; Filename: "{app}\docs\PitchNet-StarsClique-Manual-EN.pdf"; Components: docs
Name: "{group}\Руководство (русский)"; Filename: "{app}\docs\PitchNet-StarsClique-Manual-RU.pdf"; Components: docs
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\docs\PitchNet-StarsClique-Manual-EN.pdf"; Description: "Open the user manual"; Flags: postinstall shellexec skipifsilent unchecked; Components: docs; Languages: english
Filename: "{app}\docs\PitchNet-StarsClique-Manual-RU.pdf"; Description: "Открыть руководство пользователя"; Flags: postinstall shellexec skipifsilent unchecked; Components: docs; Languages: russian

[UninstallDelete]
Type: dirifempty; Name: "{commoncf64}\VST3\Stars Clique"
Type: dirifempty; Name: "{app}\docs"
Type: dirifempty; Name: "{app}"
Type: dirifempty; Name: "{commonpf64}\Stars Clique"
