@echo off
rem Builds aimp_mpegh.dll and test_host.exe with Visual Studio 2022: out\x64 and out\x86.
rem   DEPS - folder with the mpeghdec checkout (DEPS\mpeghdec); keep the path short. Default C:\mhb
rem   SDK  - AIMP SDK C++ headers. Default .\aimp_sdk
rem   VCVARS_DIR - folder of vcvars64.bat / vcvars32.bat, if it is not found automatically
setlocal
set "HERE=%~dp0"
if "%DEPS%"=="" set "DEPS=C:\mhb"
if "%SDK%"=="" set "SDK=%HERE%aimp_sdk"

if "%VCVARS_DIR%"=="" (
  for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VCVARS_DIR=%%i\VC\Auxiliary\Build"
)
if not exist "%VCVARS_DIR%\vcvars64.bat" (
  echo Visual C++ build tools not found. Set VCVARS_DIR to the folder containing vcvars64.bat.
  exit /b 1
)
if not exist "%DEPS%\mpeghdec\CMakeLists.txt" (
  echo mpeghdec not found in %DEPS%\mpeghdec. Clone https://github.com/Fraunhofer-IIS/mpeghdec there.
  exit /b 1
)
if not exist "%SDK%\apiPlugin.h" (
  echo AIMP SDK headers not found in %SDK%.
  exit /b 1
)

call :build 64 x64 b64 "" || exit /b 1
call :build 32 x86 b32 "/EXPORT:AIMPPluginGetHeader=_AIMPPluginGetHeader@4" || exit /b 1
echo Done: out\x64 and out\x86
exit /b 0

:build
rem %1 = vcvars bits, %2 = output folder, %3 = mpeghdec build folder, %4 = extra linker option
setlocal
call "%VCVARS_DIR%\vcvars%1.bat" >nul 2>nul
for /f "delims=" %%n in ('where ninja 2^>nul') do set "HAVE_NINJA=1"
if not defined HAVE_NINJA set "PATH=%DevEnvDir%CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "B=%DEPS%\%3"
if not exist "%B%\lib\mpeghdec.lib" (
  echo Building mpeghdec %2 ...
  cmake -S "%DEPS%\mpeghdec" -B "%B%" -G Ninja -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ^
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
    -DCMAKE_POLICY_DEFAULT_CMP0091=NEW > "%B%.log" 2>&1 || (echo mpeghdec configure failed, see %B%.log & exit /b 1)
  cmake --build "%B%" >> "%B%.log" 2>&1 || (echo mpeghdec build failed, see %B%.log & exit /b 1)
)
set INC=/I"%SDK%" /I"%DEPS%\mpeghdec\include" /I"%B%\_deps\mmtisobmff-src\include" /I"%B%\_deps\ilo-src\include"
set "OUT=%HERE%out\%2"
if not exist "%OUT%" mkdir "%OUT%"
echo Building plugin %2 ...
cl /nologo /std:c++17 /EHsc /O2 /MT /LD /DNOMINMAX /DMPEGHDEC_STATIC /DUNICODE /D_UNICODE %INC% ^
  "%HERE%src\aimp_mpegh.cpp" /Fo:"%OUT%\\" /Fe:"%OUT%\aimp_mpegh.dll" ^
  /link /LIBPATH:"%B%\lib" mpeghdec.lib mmtisobmff.lib ilo.lib uuid.lib user32.lib ole32.lib %~4 || exit /b 1
cl /nologo /std:c++17 /EHsc /O1 /MT /DNOMINMAX /DUNICODE /D_UNICODE %INC% ^
  "%HERE%test\test_host.cpp" /Fo:"%OUT%\\" /Fe:"%OUT%\test_host.exe" /link uuid.lib ole32.lib user32.lib || exit /b 1
exit /b 0
