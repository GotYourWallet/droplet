@echo off
setlocal

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VS_DIR=%%i"
)

if not defined VS_DIR (
    echo ERROR: Visual Studio with C++ build tools not found.
    exit /b 1
)

call "%VS_DIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo ERROR: Failed to initialize MSVC x64 environment.
    exit /b 1
)

if not exist build mkdir build

echo Compiling DropletDemo.exe (x64 Release, DirectX 11)...
cl /nologo /O2 /Oi /GL /fp:fast /std:c++17 /EHsc /W3 /DUNICODE /D_UNICODE ^
    src\main.cpp src\renderer.cpp ^
    /Fo:build\ /Fe:build\DropletDemo.exe ^
    /link /LTCG user32.lib gdi32.lib d3d11.lib dxgi.lib d3dcompiler.lib

if errorlevel 1 (
    echo Build FAILED!
    exit /b 1
)

echo.
echo Build SUCCEEDED: build\DropletDemo.exe
echo Run: build\DropletDemo.exe
endlocal
