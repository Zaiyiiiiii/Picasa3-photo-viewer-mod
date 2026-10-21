@echo off
rem Install the 32-bit static dependencies from mod\vcpkg.json into build\vcpkg_installed.
rem Local downloads go through curl.exe --ssl-no-revoke: vcpkg's own downloader fails certificate
rem revocation checks behind the local proxy (CRYPT_E_REVOCATION_OFFLINE). CI uses vcpkg's downloader.
setlocal
set ROOT=%~dp0..
if not defined CI set X_VCPKG_ASSET_SOURCES=clear;x-script,curl.exe --ssl-no-revoke -fsSL --retry 3 -o {dst} {url};x-block-origin
"%ROOT%\..\vcpkg\vcpkg.exe" install --triplet x86-windows-static ^
  "--x-manifest-root=%ROOT%\mod" ^
  "--x-install-root=%ROOT%\build\vcpkg_installed" ^
  "--overlay-ports=%ROOT%\tools\vcpkg-overlays"
