@echo off
rem Removes the ac-dlssg test build again: runs ..\scripts\uninstall.ps1 of this
rem package, from any current folder. Arguments are passed on, for example:
rem uninstall.bat -RemoveData
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\scripts\uninstall.ps1" %*
exit /b %ERRORLEVEL%
