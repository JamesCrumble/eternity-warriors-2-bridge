@echo off
rem Builds host\build-native\ew2host.exe with MSVC + CMake + Ninja from a Visual Studio install (Desktop development
rem with C++ workload; nothing else is needed). Usage: build.cmd [cmake targets]   (default: ew2host)
rem pushd maps a \\wsl.localhost share to a drive letter: cmd.exe, and with it ninja's "cmd /C cd . && link",
rem cannot run with a UNC current directory.
setlocal
if not defined VSROOT (
    for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
)
if not defined VSROOT (
    echo [ERROR] Visual Studio with the C++ tools was not found. Install "Desktop development with C++" or set VSROOT.
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
pushd "%~dp0" || exit /b 1
set SRC=%CD%
set BUILD_DIR=%SRC%\build-native
if exist "%BUILD_DIR%\CMakeCache.txt" findstr /c:"CMAKE_HOME_DIRECTORY:INTERNAL=%SRC:\=/%" "%BUILD_DIR%\CMakeCache.txt" >nul || rmdir /s /q "%BUILD_DIR%"
if not exist "%BUILD_DIR%\build.ninja" cmake -G Ninja -S "%SRC%" -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=RelWithDebInfo %EW2_CMAKE_ARGS% || (popd & exit /b 1)
set TARGETS=%*
if "%TARGETS%"=="" set TARGETS=ew2host
cmake --build "%BUILD_DIR%" --target %TARGETS% || (popd & exit /b 1)
popd
endlocal
