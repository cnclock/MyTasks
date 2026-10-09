@echo off
cd /d "%~dp0"
if not exist build\MyTasks.exe (
  call build.bat
  if errorlevel 1 exit /b 1
)
echo Installing service MyTasks as LocalSystem / Automatic ...
echo Optional: install.bat -config PATH -log PATH
build\MyTasks.exe install %*
exit /b %ERRORLEVEL%
