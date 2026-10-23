@echo off
cd /d "%~dp0"
python play_win.py %*
if errorlevel 1 (
    echo.
    echo [ERROR] Launcher exited with code %errorlevel%. Check logs\play.log and logs\ew2gl.log
    pause
)
