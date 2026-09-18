@echo off
REM Flash the app (not the model -- use flash_model.bat for that):
REM     tools\flash.bat COM10
setlocal

if "%~1"=="" (
    echo Usage: tools\flash.bat COMx
    exit /b 1
)

call "%~dp0idf_env.bat" || exit /b 1
call "%~dp0proj_dir.bat" || exit /b 1

call idf.py -p %~1 flash || exit /b 1
endlocal
