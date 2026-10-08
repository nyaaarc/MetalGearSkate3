@echo off
rem Builds mgs3-plugin\build\Release\MetalGearSkate.asi and skate-ffi\target\x86_64-pc-windows-msvc\release\skate_ffi.dll
setlocal
cd /d "%~dp0"

rem CMake and MSVC come from the Visual Studio (Build Tools) developer environment.
where cmake >nul 2>nul || (
  for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do call "%%i\Common7\Tools\VsDevCmd.bat" -arch=x64 -no_logo
)
where cmake >nul 2>nul || (echo Install Visual Studio Build Tools with "Desktop development with C++". & exit /b 1)

cmake -S mgs3-plugin -B mgs3-plugin\build -A x64 || exit /b 1
cmake --build mgs3-plugin\build --config Release || exit /b 1

pushd skate-ffi
rem MGS3 is 64-bit; static CRT so players don't need the VC++ redistributable.
set RUSTFLAGS=-C target-feature=+crt-static
cargo build --release --target x86_64-pc-windows-msvc || (popd & exit /b 1)
popd

echo.
echo Built:
echo   mgs3-plugin\build\Release\MetalGearSkate.asi
echo   skate-ffi\target\x86_64-pc-windows-msvc\release\skate_ffi.dll
