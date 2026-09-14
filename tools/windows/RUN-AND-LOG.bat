@echo off
REM Runs melee.exe with verbose logging and leaves the window open, so a run
REM from a USB stick on a machine we cannot debug still produces something
REM worth reading. Writes melee-pc.log next to melee.exe.
setlocal
cd /d "%~dp0"

set MELEE_DEBUG=1
set MELEE_LOG_FILE=melee-pc.log

echo === system ===> melee-pc-env.log
ver >> melee-pc-env.log 2>&1
wmic path win32_VideoController get name,driverversion >> melee-pc-env.log 2>&1
echo === files ===>> melee-pc-env.log
dir /b >> melee-pc-env.log 2>&1

echo Running melee.exe with logging enabled...
echo.
melee.exe %* 2>&1
set RC=%ERRORLEVEL%

echo.
echo === melee.exe exited with code %RC% ===
echo.
echo Send back these two files from this folder:
echo    melee-pc.log
echo    melee-pc-env.log
echo.
pause
