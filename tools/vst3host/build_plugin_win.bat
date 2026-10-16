@echo off
REM ---------------------------------------------------------------------------
REM Build a NeuralAmpModeler/GatewayStereoPlus VST3 for Windows x64.
REM
REM   build_plugin_win.bat <source-root> <vst3-output-dir> [target]
REM     source-root      the dir containing NeuralAmpModeler\ (i.e. the repo root)
REM     vst3-output-dir  where to put the built bundle, e.g. D:\out\VST3
REM     target           msbuild target, default NeuralAmpModeler-vst3
REM
REM VST3_64_PATH/VST3_32_PATH are overridden so the post-build step installs to
REM the given directory instead of its default C:\Program Files\Common Files\VST3.
REM Both are Condition="...==''" in common-win.props, so /p: wins.
REM
REM The build MUST go through the .sln: the props sheets use $(SolutionDir) for
REM BUILD_DIR and for the pre/post-build script paths, and that is undefined when
REM msbuild is pointed straight at the .vcxproj.
REM ---------------------------------------------------------------------------
setlocal

if "%~2"=="" (
  echo usage: build_plugin_win.bat ^<source-root^> ^<vst3-output-dir^> [target]
  exit /b 1
)

set "SRCROOT=%~1"
set "OUTDIR=%~2"
set "TARGET=%~3"
if "%TARGET%"=="" set "TARGET=NeuralAmpModeler-vst3"

REM Point VCVARS at your own Build Tools install, or set it in the environment.
if "%VCVARS%"=="" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo ERROR: vcvars64.bat not found at "%VCVARS%"
  echo        Set VCVARS to your vcvars64.bat before calling this script.
  exit /b 1
)
if not exist "%SRCROOT%\NeuralAmpModeler\NeuralAmpModeler.sln" (
  echo ERROR: no solution at "%SRCROOT%\NeuralAmpModeler\NeuralAmpModeler.sln"
  exit /b 1
)

if not exist "%OUTDIR%" mkdir "%OUTDIR%"

call "%VCVARS%" >nul
if errorlevel 1 (
  echo ERROR: vcvars64.bat failed
  exit /b 1
)

msbuild "%SRCROOT%\NeuralAmpModeler\NeuralAmpModeler.sln" ^
  /t:"%TARGET%" ^
  /p:Configuration=Release ^
  /p:Platform=x64 ^
  /p:VST3_64_PATH="%OUTDIR%" ^
  /p:VST3_32_PATH="%OUTDIR%" ^
  /p:MultiProcessorCompilation=true /p:CL_MPCount=8 ^
  /m /nologo /v:minimal /clp:Summary

if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)

echo.
echo BUILD OK -- contents of %OUTDIR%:
dir /s /b "%OUTDIR%"
endlocal
