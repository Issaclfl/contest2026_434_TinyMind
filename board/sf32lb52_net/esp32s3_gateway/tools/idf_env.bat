@echo off
REM Shared ESP-IDF environment setup, called by build.bat and menuconfig.bat.
REM
REM Why this exists rather than "just run idf.py":
REM   * ESP-IDF is not on PATH.  idf_cmd_init.bat only installs DOSKEY macros,
REM     which do nothing in a non-interactive shell, so export.bat is what
REM     actually sets IDF_PATH and the toolchain paths.
REM   * export.bat refuses to run when MSYSTEM is defined, which Git Bash sets
REM     for every process it starts, so that variable is cleared.
REM   * export.bat derives the virtualenv name from whichever `python` it finds
REM     on PATH.  This machine's PATH has a system Python 3.13 while the IDF
REM     installation carries idf5.5_py3.11_env, so IDF's own interpreter is put
REM     first -- otherwise the export fails with "virtual environment
REM     ...idf5.5_py3.13_env not found" before anything else happens.

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

call "%IDF_PATH%\export.bat"
exit /b %errorlevel%
