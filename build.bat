@echo off
rem Builds Star Wars Trilogy Native: extracts the ROM, recompiles it and builds with MSVC.
rem Usage: build.bat [path to swtrilgy.zip]   (or set SWT_ROM_ZIP, or put it next to
rem this file; only needed the first time)
setlocal
cd /d "%~dp0"
rem Started with a double click (cmd /c): keep the window open at the end so the
rem message can be read
set "PAUSE_AT_END="
for %%a in (%cmdcmdline%) do if /i "%%~a"=="/c" set PAUSE_AT_END=1
rem update.bat and install.bat run it and pause themselves
if defined SWT_NO_PAUSE set "PAUSE_AT_END="
set ZIP=%~1
if "%ZIP%"=="" set ZIP=%SWT_ROM_ZIP%
if "%ZIP%"=="" set ZIP=%~dp0swtrilgy.zip

rem Python 3.10 or later, checked: "python" in PATH can be an older one (issue #51)
call tools\find_python.bat || goto no_python
rem drive_program.bin too: data\ from an older build.bat can lack it (a set without
rem epr-21119), and the game then stops at FAILED FEEDBACK LEVER INITIALIZE
if exist data\crom.bin if exist data\drive_program.bin goto rom_ok
if not exist "%ZIP%" goto no_rom
%BUILD_PY% tools\romtool.py "%ZIP%" data || goto fail
:rom_ok
%BUILD_PY% tools\recomp.py data\crom.bin build\gen || goto fail
rem NVIDIA DLSS SDK (not in the repository): fetched from NVIDIA's GitHub the first time.
rem Every file is checked, not only the library: a third_party\ngx left incomplete
rem (an interrupted download, files copied by hand) is fetched again (issue #32)
set "NGX_OK=1"
for %%f in (include\nvsdk_ngx.h include\nvsdk_ngx_defs.h include\nvsdk_ngx_defs_vk.h include\nvsdk_ngx_helpers.h include\nvsdk_ngx_helpers_vk.h include\nvsdk_ngx_loader.h include\nvsdk_ngx_params.h include\nvsdk_ngx_vk.h lib\nvsdk_ngx_d.lib bin\nvngx_dlss.dll) do (
    if not exist third_party\ngx\%%f set "NGX_OK="
)
if not defined NGX_OK (
    call tools\get_dlss.bat || goto fail
)
rem SDL3 and miniz: downloaded once into third_party\downloads, CMake builds from there
call tools\get_deps.bat || goto fail

rem Visual Studio 2022 or later (Build Tools or any edition) with the C++ tools and
rem its CMake, and a Windows SDK
call tools\find_vs.bat
if errorlevel 2 goto no_sdk
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" goto no_vs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || goto fail
set "CMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "CTEST=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
if not exist "%CMAKE%" goto no_vs
rem glslang needs CMake 3.27, and the CMake of Visual Studio 2022 before 17.8 is
rem older: then use a newer cmake.exe from PATH if there is one
set "OLD_CMAKE=%CMAKE%"
set "CMAKE="
call :try_cmake "%OLD_CMAKE%"
if defined CMAKE goto cmake_ok
for /f "delims=" %%i in ('where cmake 2^>nul') do if not defined CMAKE call :try_cmake "%%i"
if not defined CMAKE goto old_cmake
for %%i in ("%CMAKE%") do set "CTEST=%%~dpictest.exe"
echo The CMake of Visual Studio is too old, using "%CMAKE%"
:cmake_ok
"%CMAKE%" -S . -B build\cmake -G Ninja -DCMAKE_BUILD_TYPE=Release || goto fail
"%CMAKE%" --build build\cmake || goto fail
"%CTEST%" --test-dir build\cmake --output-on-failure || goto fail
copy /y build\cmake\StarWarsTrilogy.exe StarWarsTrilogy.exe >nul
rem NVIDIA DLSS runtime (official signed DLL) for --aa dlaa
if exist third_party\ngx\bin\nvngx_dlss.dll copy /y third_party\ngx\bin\nvngx_dlss.dll nvngx_dlss.dll >nul
echo.
echo Done: StarWarsTrilogy.exe
if defined PAUSE_AT_END pause
exit /b 0

:fail
echo.
echo Build failed: see the message above.
if defined PAUSE_AT_END pause
exit /b 1

:no_rom
echo ROM not found ^("%ZIP%"^): pass the path to swtrilgy.zip as an argument ^(build.bat D:\roms\swtrilgy.zip^),
echo in the SWT_ROM_ZIP environment variable, or put it next to build.bat. Only needed the first time: it is extracted into data\.
goto fail

:no_python
echo Python 3.10 or later not found ^(an older Python or the Microsoft Store alias does not count^).
echo Run install.bat instead: it installs Python and the C++ tools, then builds.
goto fail

:try_cmake
rem Sets CMAKE to the cmake.exe in %1 if it is at least the version
rem tools\cmake_version.cmake asks for
"%~1" -P tools\cmake_version.cmake >nul 2>&1
if not errorlevel 1 set "CMAKE=%~1"
exit /b 0

:old_cmake
"%OLD_CMAKE%" -P tools\cmake_version.cmake
echo The CMake of Visual Studio ^("%VSDIR%"^) is too old: this needs CMake 3.27 or newer.
echo Update Visual Studio ^(Visual Studio Installer, Update button^) or run install.bat, which updates it.
echo A cmake.exe in PATH is ignored unless it is 3.27 or newer.
goto fail

:no_vs
echo Visual Studio 2022 with the x64 C++ tools and CMake not found.
echo Run install.bat instead: it installs them, then builds.
goto fail
:no_sdk
echo Visual Studio is in "%VSDIR%" but there is no Windows SDK. Run install.bat: it adds
echo it. Or add "Windows 11 SDK" in the Visual Studio Installer ^(Individual components^).
goto fail
