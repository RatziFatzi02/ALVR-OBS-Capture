@echo off
setlocal
set "VSDEVCMD=%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
if not exist "%VSDEVCMD%" (
    echo Visual Studio 2022 Community C++ tools were not found: "%VSDEVCMD%"
    exit /b 1
)
call "%VSDEVCMD%" -arch=x64 -host_arch=x64 >nul
cl /nologo /std:c++17 /EHsc /O2 /W4 ALVRCapturePreview.cpp /link /out:"ALVR Capture Preview.exe" d3d11.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib winmm.lib
