@echo off
rem Build the display affinity probe as x64 under two names (opted-in helper name and control).
call "C:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\vcvarsall.bat" x86_amd64
pushd "%~dp0"
if not exist out mkdir out
cl /nologo /O2 /W3 /MT AffinityProbe.c /Fo"out\\" /Fe"out\codex-computer-use.exe" user32.lib gdi32.lib
if errorlevel 1 goto fail
copy /y out\codex-computer-use.exe out\EnumProbeControl.exe >nul
popd
exit /b 0
:fail
popd
exit /b 1
