@echo off
REM ############################################################################
REM proj_dir.bat -- put the shell in a directory ESP-IDF can actually build in.
REM Called by build.bat / menuconfig.bat / flash.bat; not meant to be run alone.
REM Leaves PROJ (project root) in the environment and cd's into it.
REM
REM Why the mirror step exists: ESP-IDF's CMake probes the compiler by spawning
REM cmd.exe, and cmd.exe refuses to use a UNC path (\\wsl.localhost\...) as its
REM current directory -- it falls back to C:\Windows and the linker then fails
REM with "cannot open output file ... Permission denied".  A project hosted on a
REM WSL share therefore cannot be built in place, so this script mirrors it to a
REM local directory first.
REM
REM The mirror keeps its own sdkconfig and build/: both are excluded from the
REM copy, so the Wi-Fi credentials and the incremental objects stay put.
REM
REM EDIT SOURCES IN THE REPOSITORY.  Anything changed inside the mirror is
REM overwritten by the next mirror step (only sdkconfig and build/ survive).
REM ############################################################################
setlocal
for %%i in ("%~dp0..") do set "SRC=%%~fi"
endlocal & set "SRC=%SRC%"

if not "%SRC:~0,2%"=="\\" goto :in_place

set "PROJ=%USERPROFILE%\esp32s3_gateway_build"
echo Project is on a UNC path, mirroring to %PROJ%
echo ^(build-only: edit sources in the repository, not in the mirror^)
robocopy "%SRC%" "%PROJ%" /MIR /XD build managed_components /XF sdkconfig sdkconfig.old /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 goto :rcfail
cd /d "%PROJ%" || exit /b 1
exit /b 0

:in_place
set "PROJ=%SRC%"
cd /d "%PROJ%" || exit /b 1
exit /b 0

:rcfail
echo robocopy failed with errorlevel %errorlevel%
exit /b 1
