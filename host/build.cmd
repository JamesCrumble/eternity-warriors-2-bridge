@echo off
rem Builds host\build-win\ew3host.exe (or the targets given: build.cmd ew3gl) with MSVC + Ninja from the Visual Studio install (nothing else needed).
rem pushd maps the \\wsl.localhost share to a drive letter: cmd.exe, and with it ninja's
rem "cmd /C cd . && link", cannot run with a UNC current directory.
setlocal
set VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
pushd "%~dp0" || exit /b 1
set SRC=%CD%
set BUILD_DIR=%SRC%\build-win
if exist "%BUILD_DIR%\CMakeCache.txt" findstr /c:"CMAKE_HOME_DIRECTORY:INTERNAL=%SRC:\=/%" "%BUILD_DIR%\CMakeCache.txt" >nul || rmdir /s /q "%BUILD_DIR%"
if not exist "%BUILD_DIR%\build.ninja" cmake -G Ninja -S "%SRC%" -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=RelWithDebInfo || (popd & exit /b 1)
set TARGETS=%*
if "%TARGETS%"=="" set TARGETS=ew3host
cmake --build "%BUILD_DIR%" --target %TARGETS% || (popd & exit /b 1)
popd
endlocal
