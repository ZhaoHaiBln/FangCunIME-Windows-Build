@echo off
cd /d "%~dp0.."
if exist "%CD%\FangCunSettings.exe" (
    start "" "%CD%\FangCunSettings.exe"
) else (
    start "" rundll32.exe "%CD%\FangCunTSF.dll",OpenSettings
)
