@echo off
chcp 65001 >nul
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo 正在请求管理员权限以升级方寸输入法...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

set "SCRIPT_DIR=%~dp0"
set "ROOT_DIR=%SCRIPT_DIR%.."

if exist "%ROOT_DIR%\FangCunTSF.dll" (
    set "BUILD_DIR=%ROOT_DIR%"
    set "ASSETS_DIR=%ROOT_DIR%\assets"
) else (
    set "BUILD_DIR=%ROOT_DIR%\build\Release"
    set "ASSETS_DIR=%ROOT_DIR%\..\方寸输入法-FangCunIME\app\src\main\assets"
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%upgrade.ps1" -BuildDir "%BUILD_DIR%" -AssetsDir "%ASSETS_DIR%"
echo.
pause
