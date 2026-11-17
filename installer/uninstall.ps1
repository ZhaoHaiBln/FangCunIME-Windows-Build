#Requires -RunAsAdministrator
param(
    [string]$InstallDir = "$env:ProgramFiles\FangCunIME",
    [switch]$PurgeUserData
)

$ErrorActionPreference = "Stop"

Stop-Process -Name "FangCunSettings" -Force -ErrorAction SilentlyContinue
Remove-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" -Name "FangCunIMECompanion" -ErrorAction SilentlyContinue

$TargetDll = Join-Path $InstallDir "FangCunTSF.dll"
$RegSvr = Join-Path $env:SystemRoot "System32\regsvr32.exe"

if (Test-Path $TargetDll) {
    Write-Host "[1/3] 注销 Windows TSF COM 服务..."
    $proc = Start-Process -FilePath $RegSvr -ArgumentList "/u /s `"$TargetDll`"" -Wait -PassThru
    if ($proc.ExitCode -ne 0) {
        Write-Warning "regsvr32 /u 返回退出码: $($proc.ExitCode)"
    }
} else {
    Write-Host "[1/3] 未找到已注册的 DLL，跳过注销。"
}

Write-Host "[2/3] 清理安装目录: $InstallDir"
if (Test-Path $InstallDir) {
    try {
        Remove-Item -Recurse -Force $InstallDir
    } catch {
        Write-Warning "部分文件可能仍被宿主进程加载，请注销或重启 Windows 后再次删除 $InstallDir。"
    }
}

if ($PurgeUserData) {
    $UserDataDir = Join-Path $env:LOCALAPPDATA "FangCunIME"
    if (Test-Path $UserDataDir) {
        Write-Host "[3/3] 清理用户数据: $UserDataDir"
        Remove-Item -Recurse -Force $UserDataDir
    }
} else {
    Write-Host "[3/3] 已保留用户数据（%LOCALAPPDATA%\FangCunIME）。如需一并删除请加 -PurgeUserData。"
}

Write-Host "卸载完成。"
