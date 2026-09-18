@echo off
REM Fetch (if needed), pack and flash the model partition:
REM     tools\model.bat COM10        the fp32 model
REM     tools\model.bat COM10 q8     the int8-quantized pack (llm_q8.bin)
REM     tools\model.bat COM10 q4     the int4-quantized pack (llm_q4.bin)
REM
REM The model lives in ../esp32s3_common/assets/llm.bin and the tools that
REM produce and write it are shared with the ../esp32s3_llm project.  The
REM quantized packs come from tools/quantize_model.py; the firmware tells the
REM three apart by the format byte in the pack header.
setlocal

if "%~1"=="" (
    echo Usage: tools\model.bat COMx [q8^|q4]
    exit /b 1
)

call "%~dp0idf_env.bat" || exit /b 1
call "%~dp0proj_dir.bat" || exit /b 1

python "..\esp32s3_common\tools\fetch_model.py" || exit /b 1
if "%~2"=="" (
    python "..\esp32s3_common\tools\flash_model.py" %~1 || exit /b 1
) else (
    python "..\esp32s3_common\tools\flash_model.py" %~1 --image "assets\llm_%~2.bin" || exit /b 1
)
endlocal
