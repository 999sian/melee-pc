@echo off
REM Runs melee.exe with verbose logging and leaves the window open, so a run
REM from a USB stick on a machine we cannot debug still produces something
REM worth reading. Writes melee-pc.log next to melee.exe.
setlocal
cd /d "%~dp0"

set MELEE_DEBUG=1
set MELEE_LOG_FILE=melee-pc.log
set MELEE_FPS=1

REM Workaround for audio thread startup race: use DirectSound instead of WASAPI
set SDL_AUDIO_DRIVER=directsound

echo === system ===> melee-pc-env.log
ver >> melee-pc-env.log 2>&1
REM wmic is gone from current Windows 11, so ask PowerShell instead.
powershell -NoProfile -Command "Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,AdapterRAM | Format-List; [Environment]::OSVersion.VersionString" >> melee-pc-env.log 2>&1
echo === files ===>> melee-pc-env.log
dir /b >> melee-pc-env.log 2>&1

REM Use a disc image sitting next to the exe if there is one, otherwise fall
REM through to the launcher.
set DISC=
if exist "%~dp0melee.ciso" set DISC=melee.ciso
if exist "%~dp0melee.iso" set DISC=melee.iso
if not defined DISC (
    for %%i in ("%~dp0*.iso" "%~dp0*.ciso") do (
        if not defined DISC if exist "%%~fi" set DISC="%%~nxi"
    )
)

echo Running melee.exe %DISC% with logging enabled...
echo.
melee.exe %DISC% %* 2>melee-frames.log
set RC=%ERRORLEVEL%

echo.
echo === melee.exe exited with code %RC% ===
echo.
echo Send back these two files from this folder:
echo    melee-pc.log
echo    melee-pc-env.log
echo    melee-frames.log
echo.
pause
