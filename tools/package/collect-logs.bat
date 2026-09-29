@echo off
rem Collects the ac-dlssg logs into one zip next to this file: runs
rem ..\scripts\collect-logs.ps1 of this package, from any current folder.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\scripts\collect-logs.ps1" -OutDir "%~dp0." %*
exit /b %ERRORLEVEL%
