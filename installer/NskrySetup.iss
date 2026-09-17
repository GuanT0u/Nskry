; Build with Inno Setup 6 after creating the Release build in ..\\build.
; Optional official packages belong in installer\\bundled and are always sent
; through Nskry.exe's PackageManager CLI, never installed by Setup itself.

#define AppName "Nskry"
#define AppVersion "0.4.0"
#define AppPublisher "Nskry"
#define AppExeName "Nskry.exe"

[Setup]
AppId={{40ECBBFC-9FA7-4244-A481-0BDEA535E373}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={localappdata}\Programs\Nskry
DefaultGroupName=Nskry
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputDir=..\dist
OutputBaseFilename=NskrySetup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#AppExeName}
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full"; Description: "Full installation"
Name: "compact"; Description: "Core only"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "core"; Description: "Nskry Core"; Types: full compact custom; Flags: fixed
Name: "plugin_ocr"; Description: "Official plugin: OCR"; Types: full
Name: "plugin_scroll"; Description: "Official plugin: Long Screenshot"; Types: full
Name: "plugin_translate"; Description: "Official plugin: Translation"; Types: full
Name: "plugin_record"; Description: "Official plugin: Screen Recording"; Types: full

[Tasks]
Name: "startup"; Description: "Start Nskry when I sign in"; GroupDescription: "Options:"; Flags: checkedonce
Name: "launch"; Description: "Launch Nskry after installation"; GroupDescription: "Options:"; Flags: checkedonce

[Files]
Source: "..\build\Nskry.exe"; DestDir: "{app}"; Flags: ignoreversion
; A missing optional package is deliberately skipped, so Core-only releases
; remain buildable. Add the named .nskryplugin before building a release.
Source: "bundled\nskry-ocr.nskryplugin"; DestDir: "{app}\bundled"; Components: plugin_ocr; Flags: ignoreversion skipifsourcedoesntexist
Source: "bundled\nskry-scroll.nskryplugin"; DestDir: "{app}\bundled"; Components: plugin_scroll; Flags: ignoreversion skipifsourcedoesntexist
Source: "bundled\nskry-translate.nskryplugin"; DestDir: "{app}\bundled"; Components: plugin_translate; Flags: ignoreversion skipifsourcedoesntexist
Source: "bundled\nskry-record.nskryplugin"; DestDir: "{app}\bundled"; Components: plugin_record; Flags: ignoreversion skipifsourcedoesntexist

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Nskry"; ValueData: """{app}\{#AppExeName}"""; Tasks: startup; Flags: uninsdeletevalue

[Run]
Filename: "{app}\{#AppExeName}"; Parameters: "--install-plugin ""{app}\bundled\nskry-ocr.nskryplugin"" --silent --source official"; Components: plugin_ocr; Check: BundledPackageExists('nskry-ocr.nskryplugin'); Flags: runhidden waituntilterminated
Filename: "{app}\{#AppExeName}"; Parameters: "--install-plugin ""{app}\bundled\nskry-scroll.nskryplugin"" --silent --source official"; Components: plugin_scroll; Check: BundledPackageExists('nskry-scroll.nskryplugin'); Flags: runhidden waituntilterminated
Filename: "{app}\{#AppExeName}"; Parameters: "--install-plugin ""{app}\bundled\nskry-translate.nskryplugin"" --silent --source official"; Components: plugin_translate; Check: BundledPackageExists('nskry-translate.nskryplugin'); Flags: runhidden waituntilterminated
Filename: "{app}\{#AppExeName}"; Parameters: "--install-plugin ""{app}\bundled\nskry-record.nskryplugin"" --silent --source official"; Components: plugin_record; Check: BundledPackageExists('nskry-record.nskryplugin'); Flags: runhidden waituntilterminated
Filename: "{app}\{#AppExeName}"; Description: "Launch Nskry"; Tasks: launch; Flags: nowait postinstall skipifsilent

[Code]
function BundledPackageExists(const FileName: String): Boolean;
begin
  Result := FileExists(ExpandConstant('{app}\bundled\' + FileName));
end;
