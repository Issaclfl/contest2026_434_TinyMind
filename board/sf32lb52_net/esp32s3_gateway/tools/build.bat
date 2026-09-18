@echo off
REM Build the ESP32-S3 gateway.  See idf_env.bat for the environment details and
REM proj_dir.bat for the UNC-path mirroring.
setlocal

call "%~dp0idf_env.bat" || exit /b 1
call "%~dp0proj_dir.bat" || exit /b 1
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
echo     tools\flash.bat COMx
echo     tools\menuconfig.bat      (Wi-Fi credentials, kept out of the repo)
endlocal
