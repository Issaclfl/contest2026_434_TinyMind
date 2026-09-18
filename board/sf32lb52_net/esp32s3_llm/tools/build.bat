@echo off
REM Build the local model demo.  See idf_env.bat for the environment details and
REM proj_dir.bat for the UNC-path mirroring.
setlocal

call "%~dp0idf_env.bat" || exit /b 1
call "%~dp0proj_dir.bat" || exit /b 1
echo Building in %CD%

REM components/espllm/espllm_engine.c is generated: regenerate it so the tree
REM always matches the vendored upstream source.  The script asserts every
REM anchor it rewrites, so an upstream change fails loudly instead of
REM half-porting.
python "..\esp32s3_common\tools\port_llama2.py" || exit /b 1

if not exist sdkconfig (
    echo No sdkconfig yet, selecting the target first.
    call idf.py set-target esp32s3 || exit /b 1
)

call idf.py build || exit /b 1

echo.
echo Built.  The model is flashed separately from the app:
echo     tools\model.bat COMx     (fetch + pack + write the llm partition)
echo     tools\flash.bat COMx     (the app itself)
endlocal
