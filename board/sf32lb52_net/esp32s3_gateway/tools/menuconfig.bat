@echo off
REM Open the gateway's configuration, which is where the Wi-Fi credentials go.
REM
REM They are kept out of the repository on purpose: they land in sdkconfig,
REM which .gitignore excludes, and never in a committed file.  Same reason this
REM is a local step rather than something typed into a chat or an issue.
setlocal

call "%~dp0idf_env.bat" || exit /b 1

cd /d "%~dp0.."
echo Configuring %CD%
echo.
echo   SF32LB52 gateway -^> Wi-Fi SSID / Wi-Fi password
echo.

call idf.py menuconfig || exit /b 1
endlocal
