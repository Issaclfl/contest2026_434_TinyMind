@echo off
REM 米家设备表 → sdkconfig。token 是凭据：默认隐藏输入，不落命令行历史。
setlocal
cd /d "%~dp0.."
python "tools\set_miio.py" %*
endlocal
