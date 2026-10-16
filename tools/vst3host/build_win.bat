@echo off
REM ---------------------------------------------------------------------------
REM Build one vst3host test program with MSVC.
REM
REM   build_win.bat <test-source.cpp> <output-name>
REM   e.g. build_win.bat tests.cpp vst3tests
REM
REM Run this from a shell where vcvars64.bat has already been executed.
REM Override the SDK location by setting VST3_SDK before calling.
REM
REM This is the Windows counterpart of build.sh / build_deep.sh / etc. The only
REM platform-specific difference is module_win32.cpp in place of module_mac.mm
REM (and no -framework flags) -- everything else in the harness is portable.
REM ---------------------------------------------------------------------------
setlocal EnableDelayedExpansion

if "%~2"=="" (
  echo usage: build_win.bat ^<test-source.cpp^> ^<output-name^>
  exit /b 1
)

set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"

REM Resolve the SDK from this script's own location (tools\vst3host -> repo root)
REM so the harness works from any checkout. Override VST3_SDK to point elsewhere.
if "%VST3_SDK%"=="" for %%R in ("%HERE%\..\..") do set "VST3_SDK=%%~fR\iPlug2\Dependencies\IPlug\VST3_SDK"

if not exist "%VST3_SDK%\pluginterfaces\base\funknown.cpp" (
  echo ERROR: VST3_SDK does not look like a VST3 SDK: "%VST3_SDK%"
  exit /b 1
)
if not exist "%HERE%\%~1" (
  echo ERROR: no such test source: "%HERE%\%~1"
  exit /b 1
)

where cl >nul 2>&1
if errorlevel 1 (
  echo ERROR: cl.exe not on PATH -- run vcvars64.bat first.
  exit /b 1
)

set "OBJ=%HERE%\obj-win\%~2"
if not exist "%OBJ%" mkdir "%OBJ%"

REM /Zc:__cplusplus matters: without it MSVC reports __cplusplus as 199711L and
REM the SDK's C++17 feature checks silently take their pre-C++17 branches.
set "CFLAGS=/nologo /std:c++17 /Zc:__cplusplus /O2 /EHsc /MD /utf-8"
REM _USE_MATH_DEFINES: MSVC does not define M_PI in <cmath> without it.
set "CFLAGS=%CFLAGS% /DRELEASE=1 /DNDEBUG=1 /DWIN32 /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS /D_USE_MATH_DEFINES"
set "CFLAGS=%CFLAGS% /wd4996 /wd4244 /wd4267 /wd4305 /wd4018 /wd4101"
for %%R in ("%HERE%\..\..") do set "REPO=%%~fR"
set "CFLAGS=%CFLAGS% /I"%VST3_SDK%" /I"%HERE%" /I"%REPO%\AudioDSPTools" /I"%REPO%\eigen""

set "SRCS="
set "SRCS=%SRCS% "%HERE%\host.cpp""
set "SRCS=%SRCS% "%HERE%\heaphook.cpp""
set "SRCS=%SRCS% "%REPO%\AudioDSPTools\dsp\dsp.cpp""
set "SRCS=%SRCS% "%REPO%\AudioDSPTools\dsp\RecursiveLinearFilter.cpp""
set "SRCS=%SRCS% "%HERE%\%~1""
set "SRCS=%SRCS% "%VST3_SDK%\public.sdk\source\vst\hosting\module.cpp""
set "SRCS=%SRCS% "%VST3_SDK%\public.sdk\source\vst\hosting\module_win32.cpp""
set "SRCS=%SRCS% "%VST3_SDK%\public.sdk\source\vst\hosting\hostclasses.cpp""
set "SRCS=%SRCS% "%VST3_SDK%\public.sdk\source\vst\hosting\parameterchanges.cpp""
set "SRCS=%SRCS% "%VST3_SDK%\public.sdk\source\vst\hosting\eventlist.cpp""
set "SRCS=%SRCS% "%VST3_SDK%\public.sdk\source\vst\hosting\processdata.cpp""
set "SRCS=%SRCS% "%VST3_SDK%\public.sdk\source\vst\hosting\pluginterfacesupport.cpp""
set "SRCS=%SRCS% "%VST3_SDK%\public.sdk\source\common\memorystream.cpp""
set "SRCS=%SRCS% "%VST3_SDK%\public.sdk\source\common\commonstringconvert.cpp""
set "SRCS=%SRCS% "%VST3_SDK%\public.sdk\source\vst\utility\stringconvert.cpp""
set "SRCS=%SRCS% "%VST3_SDK%\pluginterfaces\base\funknown.cpp""
set "SRCS=%SRCS% "%VST3_SDK%\pluginterfaces\base\coreiids.cpp""
set "SRCS=%SRCS% "%VST3_SDK%\public.sdk\source\vst\vstinitiids.cpp""
set "SRCS=%SRCS% "%VST3_SDK%\pluginterfaces\base\conststringtable.cpp""
set "SRCS=%SRCS% "%VST3_SDK%\pluginterfaces\base\ustring.cpp""
set "SRCS=%SRCS% "%VST3_SDK%\base\source\fstring.cpp""
set "SRCS=%SRCS% "%VST3_SDK%\base\source\fobject.cpp""
set "SRCS=%SRCS% "%VST3_SDK%\base\source\fdebug.cpp""
set "SRCS=%SRCS% "%VST3_SDK%\base\source\fbuffer.cpp""
set "SRCS=%SRCS% "%VST3_SDK%\base\source\fstreamer.cpp""
set "SRCS=%SRCS% "%VST3_SDK%\base\source\baseiids.cpp""
set "SRCS=%SRCS% "%VST3_SDK%\base\source\updatehandler.cpp""
set "SRCS=%SRCS% "%VST3_SDK%\base\thread\source\flock.cpp""

cl %CFLAGS% %SRCS% /Fo"%OBJ%\\" /Fe"%HERE%\%~2.exe" ^
   /link /SUBSYSTEM:CONSOLE Shlwapi.lib Ole32.lib Shell32.lib User32.lib Psapi.lib Avrt.lib Gdi32.lib
if errorlevel 1 (
  echo BUILD FAILED: %~2
  exit /b 1
)

echo built: %HERE%\%~2.exe
endlocal
