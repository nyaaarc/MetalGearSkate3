@echo off
rem Installs Metal Gear Skate 3 into METAL GEAR SOLID 3 and converts your own Skate 3 data.
rem   setup.cmd            interactive
rem   setup.cmd -DryRun    show what it would do
rem   setup.cmd -Skate3 "D:\Skate 3.iso" -Yes
rem Options are tools\setup.ps1's (or setup.ps1's in a release).
setlocal
set "SCRIPT=%~dp0tools\setup.ps1"
if not exist "%SCRIPT%" set "SCRIPT=%~dp0setup.ps1"
powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" %*
set RC=%ERRORLEVEL%
if "%~1"=="" pause
exit /b %RC%
