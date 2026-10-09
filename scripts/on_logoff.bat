@echo off
REM Example wrapper if you prefer a script instead of calling shutdown.exe directly.
REM Point config.ini ScriptPath here, e.g. ScriptPath=scripts\on_logoff.bat
echo [%date% %time%] MyTasks action triggered>> "%~dp0on_logoff.log"
shutdown.exe /s /t 60 /c "MyTasks auto"
