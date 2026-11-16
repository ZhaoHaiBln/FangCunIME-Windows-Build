#define MyAppName "方寸输入法"
#define MyAppVersion "0.1.0"
#define MyAppPublisher "FangCunIME"
#define MyAppURL "https://github.com/ZhaoHaiBln/cursor-apps-archive"
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
Source: "{#MySourceDir}\installer\*"; DestDir: "{app}\installer"; Flags: ignoreversion

[Icons]
Name: "{group}\方寸输入法设置"; Filename: "{app}\FangCunSettings.exe"; WorkingDir: "{app}"
