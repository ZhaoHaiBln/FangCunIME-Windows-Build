#define MyAppName "方寸输入法"
#define MyAppVersion "0.2.0"
#define MyAppPublisher "FangCunIME"
#define MyAppURL "https://github.com/ZhaoHaiBln/FangCunIME-Windows-Build"
#ifndef MySourceDir
  #define MySourceDir "dist"
#endif

[Setup]
AppId={{A7F6E5D4-3C2B-4A19-8F0E-1D2C3B4A5968}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
DefaultDirName={autopf}\FangCunIME
DefaultGroupName={#MyAppName}
LicenseFile={#MySourceDir}\rime-ice-LICENSE.txt
OutputDir=out
OutputBaseFilename=FangCunIME-{#MyAppVersion}-win64-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#MySourceDir}\FangCunTSF.dll"; DestDir: "{app}"; Flags: ignoreversion regserver 64bit restartreplace uninsrestartdelete
Source: "{#MySourceDir}\FangCunSettings.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MySourceDir}\assets\dict.db"; DestDir: "{app}\assets"; Flags: ignoreversion
Source: "{#MySourceDir}\assets\pinyin-v1.dbz"; DestDir: "{app}\assets"; Flags: ignoreversion
Source: "{#MySourceDir}\assets\pinyin-v1.json"; DestDir: "{app}\assets"; Flags: ignoreversion
Source: "{#MySourceDir}\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MySourceDir}\rime-ice-LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\方寸输入法设置"; Filename: "{app}\FangCunSettings.exe"; WorkingDir: "{app}"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "FangCunIMECompanion"; ValueData: """{app}\FangCunSettings.exe"" --tray"; Flags: uninsdeletevalue

[Run]
Filename: "{app}\FangCunSettings.exe"; Parameters: "--tray"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent; Description: "启动方寸输入法伴侣（浮动状态栏、托盘与 Alt+V 剪贴板）"

[UninstallRun]
Filename: "taskkill.exe"; Parameters: "/F /IM FangCunSettings.exe"; Flags: runhidden; RunOnceId: "StopFangCunSettings"

[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
begin
  Exec('taskkill.exe', '/F /IM FangCunSettings.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Result := '';
end;
