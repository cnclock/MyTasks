@echo off
cd /d "%~dp0"
if not exist build\MyTasks.exe (
  echo build\MyTasks.exe not found. Build first, or run from an already-installed copy.
  exit /b 1
)
build\MyTasks.exe uninstall
exit /b %ERRORLEVEL%
