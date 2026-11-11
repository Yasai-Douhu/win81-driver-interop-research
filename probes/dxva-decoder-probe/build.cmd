@echo off
rem Build the D3D11 H.264 decoder probe (x64).
call "C:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\vcvarsall.bat" x86_amd64
pushd "%~dp0"
if not exist out mkdir out
cl /nologo /O2 /W3 /MT /EHsc DecoderProbe.cpp /Fo"out\\" /Fe"out\DecoderProbe.exe" d3d11.lib
set e=%errorlevel%
popd
exit /b %e%
