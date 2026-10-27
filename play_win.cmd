@echo off
rem Starts Eternity Warriors 2 on Windows. The game itself runs in WSL2 (play_wsl.sh): the ARM code under qemu-arm,
rem OpenGL ES on the Windows GPU through ANGLE/Direct3D 11, sound through WSLg. See README.md.
rem Usage: play_win.cmd [WIDTHxHEIGHT]      Settings: environment variables EW2_* (README.md), EW2_WSL_DISTRO=<name>
cd /d "%~dp0"
where wsl >nul 2>nul
if errorlevel 1 (
    echo [ERROR] WSL is not installed. Run "wsl --install" in an administrator terminal, reboot, and see README.md.
    pause
    exit /b 1
)
where python >nul 2>nul
if errorlevel 1 (
    py -3 play_win.py %*
) else (
    python play_win.py %*
)
if errorlevel 1 (
    echo.
    echo [ERROR] Launcher exited with code %errorlevel%. Check logs\play.log, logs\ew2gl.log and logs\history\
    pause
)
