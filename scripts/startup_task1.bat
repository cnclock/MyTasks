@echo off
REM Example program launched after service start (see [Startup] in config.ini).
echo [%date% %time%] Startup task1 ran>> "%~dp0startup_task1.log"
exit /b 0
