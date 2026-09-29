@echo off
rem Removes ac-dlssg from the game folder this file is in: runs
rem scripts\uninstall.ps1 next to it, which works on this game folder only (no
rem Steam lookup). Arguments are passed on, for example: uninstall.bat -RemoveData
rem After a successful uninstall and the Enter at its end, that script also
rem deletes this file, collect-logs.bat and the scripts folder (with -RemoveData
rem the whole ac-dlssg folder); when the game folder needs administrator rights,
rem the elevated run does that in its own window while this one waits.
rem cmd.exe reads a batch file one line at a time, so everything after the
rem PowerShell run is on its line: the folder change first (so that ac-dlssg can
rem be deleted), and on exit code 0 "(goto)", which ends this batch file without
rem reading it again, so there is no "The batch file cannot be found".
cd /d "%~dp0.." & "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\uninstall.ps1" -InstalledCopy %* & if not errorlevel 1 ((goto) 2>nul & (call ))
exit /b %ERRORLEVEL%
