@echo off
rem Installs the ac-dlssg test build into Assetto Corsa: runs scripts\install.ps1
rem of the folder this file is in, from any current folder. Arguments are passed
rem on, for example: install.bat -NoSpoof
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\install.ps1" %*
exit /b %ERRORLEVEL%
