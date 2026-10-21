@echo off
rem Build and pack the release: version.dll and README.md only, no Google files.
rem Usage: release.cmd [version]   (default v0.1)
setlocal
set ROOT=%~dp0..
set VER=%~1
if "%VER%"=="" set VER=v0.1
call "%~dp0build.cmd" || exit /b 1
set STAGE=%ROOT%\build\release\picasa-viewer-mod-%VER%
set ZIP=%ROOT%\release\picasa-viewer-mod-%VER%.zip
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%" || exit /b 1
if not exist "%ROOT%\release" mkdir "%ROOT%\release"
copy /y "%ROOT%\build\mod\version.dll" "%STAGE%\" >nul || exit /b 1
copy /y "%ROOT%\README.md" "%STAGE%\" >nul || exit /b 1
powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%\*' -DestinationPath '%ZIP%' -Force" || exit /b 1
echo packed %ZIP%
