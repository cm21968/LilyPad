@echo off
set LOGFILE=C:\Users\Connor\Desktop\LilyPad\build_output.txt
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
cd /d C:\Users\Connor\Desktop\LilyPad
echo === CMake Configure === > "%LOGFILE%"
cmake --preset x64-Release >> "%LOGFILE%" 2>&1
echo CONFIGURE_EXIT=%ERRORLEVEL% >> "%LOGFILE%"
if %ERRORLEVEL% NEQ 0 (
    echo === Configure FAILED === >> "%LOGFILE%"
    exit /b %ERRORLEVEL%
)
echo === CMake Build === >> "%LOGFILE%"
cmake --build out/build/x64-Release >> "%LOGFILE%" 2>&1
echo BUILD_EXIT=%ERRORLEVEL% >> "%LOGFILE%"
if %ERRORLEVEL% NEQ 0 (
    echo === Build FAILED === >> "%LOGFILE%"
    exit /b %ERRORLEVEL%
)
echo === Build SUCCESS === >> "%LOGFILE%"
