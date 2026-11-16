#Requires -RunAsAdministrator
param(
    [string]$BuildDir = "$PSScriptRoot\..\build\Release",
    [string]$InstallDir = "$env:ProgramFiles\FangCunIME",
    [string]$AssetsDir = "$PSScriptRoot\..\..\方寸输入法-FangCunIME\app\src\main\assets"
)

$ErrorActionPreference = "Stop"

$DllSource = Join-Path $BuildDir "FangCunTSF.dll"
if (-not (Test-Path $DllSource)) {
    throw "未找到新版 $DllSource，请先构建。"
}

$TargetDll = Join-Path $InstallDir "FangCunTSF.dll"
$RegSvr = Join-Path $env:SystemRoot "System32\regsvr32.exe"

Write-Host "[1/4] 注销旧版 TSF 服务（保留用户词频与个人词典）..."
if (Test-Path $TargetDll) {
    Start-Process -FilePath $RegSvr -ArgumentList "/u /s `"$TargetDll`"" -Wait | Out-Null
    $OldBackup = "$TargetDll.old"
    if (Test-Path $OldBackup) {
        Remove-Item -Force $OldBackup -ErrorAction SilentlyContinue
    }
    try {
        Move-Item -Force $TargetDll $OldBackup -ErrorAction Stop
    } catch {
        Write-Warning "旧版 DLL 正在被其他进程占用，尝试标记重启替换或请先关闭记事本等宿主应用。"
        throw
    }
}

Write-Host "[2/4] 部署新版 DLL 与词库..."
& "$PSScriptRoot\install.ps1" -BuildDir $BuildDir -InstallDir $InstallDir -AssetsDir $AssetsDir

Write-Host "[3/4] 清理旧版备份..."
$OldBackup = "$TargetDll.old"
if (Test-Path $OldBackup) {
    Remove-Item -Force $OldBackup -ErrorAction SilentlyContinue
}

Write-Host "[4/4] 升级完成，用户数据（%LOCALAPPDATA%\FangCunIME）已完整保留。"
