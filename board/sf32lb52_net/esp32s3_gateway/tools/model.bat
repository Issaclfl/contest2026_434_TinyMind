@echo off
REM Fetch (if needed), pack and flash the model partition:
REM     tools\model.bat COM10
REM
REM The model lives in ../esp32s3_common/assets/llm.bin and the tools that
REM produce and write it are shared with the ../esp32s3_llm project.
setlocal

if "%~1"=="" (
    echo Usage: tools\model.bat COMx
    exit /b 1
)

call "%~dp0idf_env.bat" || exit /b 1
call "%~dp0proj_dir.bat" || exit /b 1

python "..\esp32s3_common\tools\fetch_model.py" || exit /b 1
python "..\esp32s3_common\tools\flash_model.py" %~1 || exit /b 1
endlocal
