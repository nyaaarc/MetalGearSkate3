@echo off
rem Builds the player release zip in dist\ (plugin, engine DLL, setup, README, licences; no game data).
rem   package.cmd           package the current build outputs
rem   package.cmd -Build    run build.cmd first
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\package.ps1" %*
exit /b %ERRORLEVEL%
