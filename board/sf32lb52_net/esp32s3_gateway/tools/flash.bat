@echo off
REM Flash the gateway to the board's USB serial port:  tools\flash.bat COM10
REM
REM Note for this particular setup: the ESP32-S3 here reaches the PC through a
REM USB-serial bridge on UART0 (GPIO43/44), which is also the ROM download port,
REM so esptool's auto-reset works and no button press is needed.  That port is
REM also what the bench mode (GATEWAY_UPLINK=n) uses for PPP data, which is why
REM the console is switched off there -- see the README's bench section.
setlocal

if "%~1"=="" (
    echo Usage: tools\flash.bat COMx
    exit /b 1
)

call "%~dp0idf_env.bat" || exit /b 1
call "%~dp0proj_dir.bat" || exit /b 1

call idf.py -p %~1 flash || exit /b 1
endlocal
