@echo off
REM Build the ESP32-S3 gateway.
REM
REM Why this wrapper exists rather than "just run idf.py build":
REM   * ESP-IDF is not on PATH.  idf_cmd_init.bat only installs DOSKEY macros,
REM     which do nothing in a non-interactive shell, so export.bat is what
REM     actually sets IDF_PATH and the toolchain paths.
REM   * export.bat refuses to run when MSYSTEM is defined, which Git Bash sets
REM     for every process it starts, so that variable is cleared first.
REM   * export.bat derives the virtualenv name from whichever `python` it finds
REM     on PATH.  This machine's PATH has a system Python 3.13 while the IDF
REM     installation carries idf5.5_py3.11_env, so IDF's own interpreter is put
REM     first -- otherwise the export fails with "virtual environment
REM     ...idf5.5_py3.13_env not found" before any compiling starts.
setlocal

if "%IDF_TOOLS_PATH%"==""  set "IDF_TOOLS_PATH=E:\Espressif"
if "%IDF_PATH%"==""        set "IDF_PATH=E:\Espressif\frameworks\esp-idf-v5.5.5"

set "MSYSTEM="

set "IDF_PYTHON=%IDF_TOOLS_PATH%\tools\idf-python\3.11.2"
if exist "%IDF_PYTHON%\python.exe" (
    set "PATH=%IDF_PYTHON%;%PATH%"
) else (
    echo WARNING: %IDF_PYTHON%\python.exe not found; falling back to PATH python,
    echo          which will fail unless it matches the installed python_env.
)

call "%IDF_PATH%\export.bat" || exit /b 1

cd /d "%~dp0.."
echo Building in %CD%

REM One-line correction to a bad gate in ESP-IDF's own CMakeLists.txt, without
REM which the esp_netif PPP layer is left out of the build and linking fails on
REM undefined references.  See the script header.  Idempotent.
python "tools\fix_idf_ppp_gate.py" || exit /b 1

if not exist sdkconfig (
    echo No sdkconfig yet, selecting the target first.
    call idf.py set-target esp32s3 || exit /b 1
)

call idf.py build || exit /b 1

echo.
echo Built.  Flashing needs the board, and the SSID is set separately:
echo     idf.py -p COMx flash monitor
endlocal
