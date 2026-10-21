@echo off
rem Build mod\ as a 32-bit version.dll and, when the original install (ORIG) is there, assemble a
rem runnable viewer in dist\. ORIG is only read, never written.
setlocal
set ROOT=%~dp0..
set ORIG=%ROOT%\..\picasa3-64.3.9.0
set DIST=%ROOT%\dist
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS=%%i
if not defined VS (echo Visual Studio with C++ tools not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
cmake -S "%ROOT%\mod" -B "%ROOT%\build\mod" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  "-DCMAKE_TOOLCHAIN_FILE=%ROOT%\..\vcpkg\scripts\buildsystems\vcpkg.cmake" ^
  -DVCPKG_TARGET_TRIPLET=x86-windows-static -DVCPKG_MANIFEST_INSTALL=OFF ^
  "-DVCPKG_INSTALLED_DIR=%ROOT%\build\vcpkg_installed" || exit /b 1
cmake --build "%ROOT%\build\mod" || exit /b 1
if not exist "%ORIG%\PicasaPhotoViewer.exe" (echo built %ROOT%\build\mod\version.dll & exit /b 0)
rem The installed files are copied unchanged: the mod changes nothing on disk (its resource pack changes
rem are made at run time). Files no longer in the install are removed from dist; version.dll and the
rem log are not taken from the install.
robocopy "%ORIG%" "%DIST%" /MIR /XD _archive /XF version.dll picasa_mod.log /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1
copy /y "%ROOT%\build\mod\version.dll" "%DIST%\version.dll" >nul || exit /b 1
echo built %DIST%\version.dll
