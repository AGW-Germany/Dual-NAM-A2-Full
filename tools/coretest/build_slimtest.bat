@echo off
REM Build slimtest.exe against the NeuralAmpModelerCore submodule that the plugin
REM uses, with the same NAM_ENABLE_A2_FAST define as the plugin build.
REM Run from a shell where vcvars64.bat has been executed.
setlocal
set "HERE=%~dp0"
set "ROOT=%HERE%..\.."
set "CORE=%ROOT%\NeuralAmpModelerCore"
if not exist "%HERE%obj" mkdir "%HERE%obj"
cl /nologo /std:c++20 /O2 /EHsc /MD /utf-8 /DNAM_ENABLE_A2_FAST /DNDEBUG /wd4244 /wd4267 /wd4305 /wd4996 ^
  /I"%CORE%" /I"%CORE%\NAM" /I"%CORE%\Dependencies\nlohmann" /I"%ROOT%\eigen" ^
  "%HERE%slimtest.cpp" ^
  "%CORE%\NAM\activations.cpp" "%CORE%\NAM\conv1d.cpp" "%CORE%\NAM\convnet.cpp" "%CORE%\NAM\dsp.cpp" ^
  "%CORE%\NAM\get_dsp.cpp" "%CORE%\NAM\lstm.cpp" "%CORE%\NAM\ring_buffer.cpp" "%CORE%\NAM\util.cpp" ^
  "%CORE%\NAM\container.cpp" "%CORE%\NAM\wavenet\model.cpp" "%CORE%\NAM\wavenet\a2_fast.cpp" ^
  "%CORE%\NAM\wavenet\slimmable.cpp" ^
  /Fo"%HERE%obj\\" /Fe"%HERE%slimtest.exe"
if errorlevel 1 exit /b 1
echo built: %HERE%slimtest.exe
