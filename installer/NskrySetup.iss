; Build with Inno Setup 6.5+ after creating the Release build in ..\\build.
; Optional official packages belong in installer\\bundled and are always sent
; through Nskry.exe's PackageManager CLI, never installed by Setup itself.

#define AppName "Nskry"
#define AppVersion "0.5.1"
#define AppPublisher "Nskry"
#define AppExeName "Nskry.exe"
#ifndef CoreExePath
#define CoreExePath "..\build\Nskry.exe"
#endif
#ifndef ChineseMessagesFile
#define ChineseMessagesFile "compiler:Languages\ChineseSimplified.isl"
#endif

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
SetupIconFile=..\src\ui\resources\nskry.ico
UninstallDisplayIcon={app}\{#AppExeName}
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "{#ChineseMessagesFile}"

[CustomMessages]
english.FullInstall=Full installation
chinesesimplified.FullInstall=完整安装
english.CoreOnly=Core only
chinesesimplified.CoreOnly=仅安装核心
english.CustomInstall=Custom installation
chinesesimplified.CustomInstall=自定义安装
english.CoreName=Nskry Core
chinesesimplified.CoreName=Nskry 核心
english.ScrollName=Official plugin: Long Screenshot
chinesesimplified.ScrollName=官方插件：长截图
english.OcrName=Official plugin: Text recognition
chinesesimplified.OcrName=官方插件：文字识别
english.RecordName=Official plugin: Screen recording
chinesesimplified.RecordName=官方插件：录屏
english.StartOnSignIn=Start Nskry when I sign in
chinesesimplified.StartOnSignIn=登录 Windows 时启动 Nskry
english.LaunchAfterInstall=Launch Nskry after installation
chinesesimplified.LaunchAfterInstall=安装后启动 Nskry
english.InstallOptions=Options:
chinesesimplified.InstallOptions=安装选项
english.LaunchApp=Launch Nskry
chinesesimplified.LaunchApp=启动 Nskry
english.PluginInstallError=%1 could not be installed. Retry the plugin installation, or skip it and finish installing Nskry.
chinesesimplified.PluginInstallError=无法安装 %1。可重试安装该插件，或跳过并完成 Nskry 安装。

[Types]
Name: "full"; Description: "{cm:FullInstall}"
Name: "compact"; Description: "{cm:CoreOnly}"
Name: "custom"; Description: "{cm:CustomInstall}"; Flags: iscustom

[Components]
Name: "core"; Description: "{cm:CoreName}"; Types: full compact custom; Flags: fixed
#ifexist "bundled\nskry-scroll.nskryplugin"
Name: "plugin_scroll"; Description: "{cm:ScrollName}"; Types: full
#endif
#ifexist "bundled\nskry-ocr.nskryplugin"
Name: "plugin_ocr"; Description: "{cm:OcrName}"; Types: full
#endif
#ifexist "bundled\nskry-record.nskryplugin"
Name: "plugin_record"; Description: "{cm:RecordName}"; Types: full
#endif

[Tasks]
Name: "startup"; Description: "{cm:StartOnSignIn}"; GroupDescription: "{cm:InstallOptions}"; Flags: checkedonce
Name: "launch"; Description: "{cm:LaunchAfterInstall}"; GroupDescription: "{cm:InstallOptions}"; Flags: checkedonce

[Files]
Source: "{#CoreExePath}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\COPYING"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\PLUGIN_EXCEPTION"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\docs\plugins\third_party_policy.md"; DestDir: "{app}\docs"; Flags: ignoreversion
; A component is shown only when its package exists at installer compile time.
#ifexist "bundled\nskry-scroll.nskryplugin"
Source: "bundled\nskry-scroll.nskryplugin"; DestDir: "{app}\bundled"; Components: plugin_scroll; Flags: ignoreversion skipifsourcedoesntexist
#endif
#ifexist "bundled\nskry-ocr.nskryplugin"
Source: "bundled\nskry-ocr-provider.nskryplugin"; DestDir: "{app}\bundled"; Components: plugin_ocr; Flags: ignoreversion
Source: "bundled\nskry-ocr.nskryplugin"; DestDir: "{app}\bundled"; Components: plugin_ocr; Flags: ignoreversion skipifsourcedoesntexist
#endif
#ifexist "bundled\nskry-record.nskryplugin"
Source: "bundled\nskry-record.nskryplugin"; DestDir: "{app}\bundled"; Components: plugin_record; Flags: ignoreversion skipifsourcedoesntexist
#endif

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Nskry"; ValueData: """{app}\{#AppExeName}"""; Tasks: startup; Flags: uninsdeletevalue

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchApp}"; Tasks: launch; Flags: nowait postinstall skipifsilent

[Code]
procedure InstallBundledPlugin(const Component, FileName, DisplayName: String);
var
  PackagePath: String;
  ResultCode: Integer;
  Installed: Boolean;
  Response: Integer;
begin
  if not WizardIsComponentSelected(Component) then
    Exit;
  PackagePath := ExpandConstant('{app}\bundled\' + FileName);
  repeat
    ResultCode := 0;
    Installed := FileExists(PackagePath) and
      Exec(ExpandConstant('{app}\{#AppExeName}'), '--install-plugin "' + PackagePath +
        '" --silent --source official --with-ocr-provider', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and
      (ResultCode = 0);
    if Installed then
      Exit;
    Log('Unable to install bundled plugin ' + FileName + ' (code ' + IntToStr(ResultCode) + ').');
    if WizardSilent then
      Exit;
    Response := MsgBox(FmtMessage(CustomMessage('PluginInstallError'), [DisplayName]),
      mbError, MB_RETRYCANCEL);
  until Response <> IDRETRY;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep <> ssPostInstall then
    Exit;
#ifexist "bundled\nskry-scroll.nskryplugin"
  InstallBundledPlugin('plugin_scroll', 'nskry-scroll.nskryplugin', CustomMessage('ScrollName'));
#endif
#ifexist "bundled\nskry-ocr.nskryplugin"
  InstallBundledPlugin('plugin_ocr', 'nskry-ocr.nskryplugin', CustomMessage('OcrName'));
#endif
#ifexist "bundled\nskry-record.nskryplugin"
  InstallBundledPlugin('plugin_record', 'nskry-record.nskryplugin', CustomMessage('RecordName'));
#endif
end;
