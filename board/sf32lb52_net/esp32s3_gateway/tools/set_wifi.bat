@echo off
REM ############################################################################
REM set_wifi.bat -- one command: enter the Wi-Fi credentials, rebuild, flash.
REM
REM     tools\set_wifi.bat            asks for SSID and password
REM     tools\set_wifi.bat COM10      same, then rebuilds and flashes that port
REM     tools\set_wifi.bat --show     what is configured now (password masked)
REM     tools\set_wifi.bat --clear    erase the credentials
REM
REM The credentials only ever land in sdkconfig, which .gitignore excludes --
REM never in a committed file, a command line, or a log.  This exists because
REM hunting for two fields through the menuconfig TUI is a bad ask.
REM ############################################################################
setlocal

set "PORT="
set "ARG="

if "%~1"=="" goto :ready
echo %~1 | findstr /B /R "COM[0-9][0-9]*" >nul
if errorlevel 1 (
    set "ARG=%~1"
) else (
    set "PORT=%~1"
)

:ready
call "%~dp0idf_env.bat" || exit /b 1
call "%~dp0proj_dir.bat" || exit /b 1
echo Working in %CD%
echo.

python "tools\set_wifi.py" %ARG% || exit /b 1

REM --show / --clear are not asking for a build.
if not "%ARG%"=="" goto :done

echo.
echo Rebuilding with the new credentials...
call idf.py build || exit /b 1

if "%PORT%"=="" goto :hint
call idf.py -p %PORT% flash || exit /b 1
echo.
echo Flashed %PORT%.  What it prints is worth reading: the S3 logs the Wi-Fi
echo address it got, then "ready -- waiting for the board to dial in".
goto :done

:hint
echo Build done.  Flash it with:  tools\flash.bat COMx

:done
endlocal
