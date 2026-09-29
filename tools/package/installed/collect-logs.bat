@echo off
rem Collects the ac-dlssg logs of the game folder this file is in into one zip in
rem this folder, and prints its full path: runs scripts\collect-logs.ps1 next to
rem it. It only reads files and writes that zip. Arguments are passed on.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\collect-logs.ps1" -GameDir "%~dp0.." -OutDir "%~dp0." %*
exit /b %ERRORLEVEL%
