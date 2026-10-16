@echo off
REM Build thresholdproof.exe (proof of the 1.2.0 gate threshold migration)
REM against the AudioDSPTools noise gate that 1.0.0 / 1.1.0 used.
REM Run from a shell where vcvars64.bat has been executed.
setlocal
set "HERE=%~dp0"
set "ROOT=%HERE%..\.."
set "DSP=%ROOT%\AudioDSPTools"
if not exist "%HERE%obj" mkdir "%HERE%obj"
cl /nologo /std:c++20 /O2 /EHsc /MD /utf-8 /DNDEBUG /wd4244 /wd4267 /wd4305 /wd4996 ^
  /I"%DSP%" ^
  "%HERE%thresholdproof.cpp" "%DSP%\dsp\NoiseGate.cpp" "%DSP%\dsp\dsp.cpp" ^
  /Fo"%HERE%obj\\" /Fe"%HERE%thresholdproof.exe"
if errorlevel 1 exit /b 1
echo built: %HERE%thresholdproof.exe
