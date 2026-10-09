@echo off
setlocal
cd /d "%~dp0"

if not exist build mkdir build

where g++ >nul 2>&1
if errorlevel 1 (
  if exist "D:\MinGW\bin\g++.exe" set "PATH=D:\MinGW\bin;%PATH%"
)

echo Building MyTasks.exe ...
g++ -O2 -std=c++17 -municode -o build\MyTasks.exe src\mytasks.cpp -ladvapi32 -lwtsapi32 -luserenv -s
if errorlevel 1 (
  echo Build failed.
  exit /b 1
)

copy /Y config.ini build\config.ini >nul
if not exist build\scripts mkdir build\scripts
copy /Y scripts\*.bat build\scripts\ >nul

echo OK: build\MyTasks.exe
echo.
echo Install  (Admin): build\MyTasks.exe install
echo   custom paths:   build\MyTasks.exe install -config D:\cfg\config.ini -log D:\logs\MyTasks.log
echo Uninstall(Admin): build\MyTasks.exe uninstall
echo Show paths:       build\MyTasks.exe paths
echo Test / idle / startup: build\MyTasks.exe test^|idle^|startup
exit /b 0
