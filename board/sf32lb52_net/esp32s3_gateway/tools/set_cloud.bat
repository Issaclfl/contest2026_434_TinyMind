@echo off
REM ############################################################################
REM set_cloud.bat -- one command: enter the cloud LLM API key, rebuild, flash.
REM
REM     tools\set_cloud.bat            asks for the key (no echo)
REM     tools\set_cloud.bat COM10      same, then rebuilds and flashes that port
REM     tools\set_cloud.bat --show     what is configured now (key masked)
REM     tools\set_cloud.bat --clear    erase the key (local model answers all)
REM
REM The key only ever lands in sdkconfig, which .gitignore excludes -- never in
REM a committed file, a command line, or a log.  With a key set, requests go to
REM the cloud while the uplink is up and fall back to the on-device model when
REM it is not; the board keeps one address and never learns the difference.
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

python "tools\set_cloud.py" %ARG% || exit /b 1

REM --show / --clear are not asking for a build.
if not "%ARG%"=="" goto :done

REM Re-read the file, so the confirmation is about what is on disk rather than
REM about what the write was supposed to have done.
echo.
python "tools\set_cloud.py" --show

echo.
echo Rebuilding with the new key...
call idf.py build || exit /b 1

if "%PORT%"=="" goto :hint
call idf.py -p %PORT% flash || exit /b 1
echo.
echo Flashed %PORT%.  Point the board at the gateway with model name "auto":
echo     set_llm http://10.0.0.1/v1 auto local
goto :done

:hint
echo Build done.  Flash it with:  tools\flash.bat COMx

:done
endlocal
