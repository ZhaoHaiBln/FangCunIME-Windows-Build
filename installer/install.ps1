#Requires -RunAsAdministrator
param(
    [string]$BuildDir = "$PSScriptRoot\..\build\Release",
    [string]$InstallDir = "$env:ProgramFiles\FangCunIME",
    [string]$AssetsDir = "$PSScriptRoot\..\..\方寸输入法-FangCunIME\app\src\main\assets"
)

$ErrorActionPreference = "Stop"

$DllSource = Join-Path $BuildDir "FangCunTSF.dll"
if (-not (Test-Path $DllSource)) {
    throw "未找到 $DllSource，请先使用 CMake 构建 Release 版本。"
}

Write-Host "[1/4] 创建安装目录: $InstallDir"
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $InstallDir "assets") | Out-Null

Write-Host "[2/4] 复制 FangCunTSF.dll 与词库资产..."
Copy-Item -Force $DllSource (Join-Path $InstallDir "FangCunTSF.dll")

$DictDb = Join-Path $AssetsDir "dict.db"
$PinyinDbz = Join-Path $AssetsDir "pinyin-v1.dbz"
$PinyinJson = Join-Path $AssetsDir "pinyin-v1.json"

if (Test-Path $DictDb) {
    Copy-Item -Force $DictDb (Join-Path $InstallDir "assets\dict.db")
}
if (Test-Path $PinyinDbz) {
    Copy-Item -Force $PinyinDbz (Join-Path $InstallDir "assets\pinyin-v1.dbz")
}
if (Test-Path $PinyinJson) {
    Copy-Item -Force $PinyinJson (Join-Path $InstallDir "assets\pinyin-v1.json")
}

Write-Host "[3/4] 注册 Windows TSF COM 服务..."
$RegSvr = Join-Path $env:SystemRoot "System32\regsvr32.exe"
$TargetDll = Join-Path $InstallDir "FangCunTSF.dll"
$proc = Start-Process -FilePath $RegSvr -ArgumentList "/s `"$TargetDll`"" -Wait -PassThru
if ($proc.ExitCode -ne 0) {
    throw "regsvr32 注册失败，退出码: $($proc.ExitCode)"
}

Write-Host "[4/4] 安装完成！请按 Win+Space 在输入法列表中选择「方寸输入法」。"
