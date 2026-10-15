@echo off
setlocal EnableExtensions
cd /d "%~dp0"
set "LOG=%~dp0build.log"
rem Where the built plugin is copied: your MO2 mod folder (edit this line)
set "MODDIR=D:\Games\Skyrim SE Virtual Folder\mods\Faction Wardrobe"
rem vcpkg and its libraries live here, shared by builds (reuses an existing RideAlongTools folder if there is one)
set "TOOLS=%LOCALAPPDATA%\SKSEBuildTools"
if exist "%LOCALAPPDATA%\RideAlongTools\vcpkg\vcpkg.exe" set "TOOLS=%LOCALAPPDATA%\RideAlongTools"

echo ================================================== > "%LOG%"
echo Faction Wardrobe build  %DATE% %TIME% >> "%LOG%"
echo ================================================== >> "%LOG%"
echo.
echo  Faction Wardrobe - building. The first build downloads libraries and can take 15-30 minutes.
echo  Progress is written to build.log
echo.

rem ---------- Git ----------
where git >nul 2>&1
if errorlevel 1 (
  echo [ERROR] Git was not found. Install Git for Windows from https://git-scm.com/download/win >> "%LOG%"
  goto :fail
)

rem ---------- Visual Studio Build Tools ----------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [ERROR] vswhere.exe not found - are the Visual Studio Build Tools installed? >> "%LOG%"
  goto :fail
)
set "VSDIR="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo [ERROR] No Visual Studio install with the C++ tools was found. In the installer, tick "Desktop development with C++". >> "%LOG%"
  goto :fail
)
echo Using Visual Studio at %VSDIR% >> "%LOG%"
rem CMake ships with the Build Tools; it finds the compiler by itself (no developer prompt needed)
set "CMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE%" (
  echo [ERROR] CMake not found. In the Visual Studio Installer, modify Build Tools and add "C++ CMake tools for Windows". >> "%LOG%"
  goto :fail
)
"%CMAKE%" --version >> "%LOG%" 2>&1

rem ---------- vcpkg (package manager for the libraries) ----------
set "VCPKG_ROOT=%TOOLS%\vcpkg"
if not exist "%VCPKG_ROOT%\vcpkg.exe" (
  echo. & echo  Setting up vcpkg...
  echo --- cloning vcpkg >> "%LOG%"
  if not exist "%TOOLS%" mkdir "%TOOLS%"
  if not exist "%VCPKG_ROOT%\.git" git clone https://github.com/microsoft/vcpkg "%VCPKG_ROOT%" >> "%LOG%" 2>&1
  call "%VCPKG_ROOT%\bootstrap-vcpkg.bat" -disableMetrics >> "%LOG%" 2>&1
)
if not exist "%VCPKG_ROOT%\vcpkg.exe" (
  echo [ERROR] vcpkg setup failed. >> "%LOG%"
  goto :fail
)

rem ---------- CommonLibSSE-NG ----------
if not exist "%~dp0extern\CommonLibSSE-NG\CMakeLists.txt" (
  echo. & echo  Downloading CommonLibSSE-NG...
  echo --- cloning CommonLibSSE-NG v11.0.0 >> "%LOG%"
  git clone --depth 1 --branch v11.0.0 --recurse-submodules --shallow-submodules https://github.com/alandtse/CommonLibSSE-NG.git "%~dp0extern\CommonLibSSE-NG" >> "%LOG%" 2>&1
)
if not exist "%~dp0extern\CommonLibSSE-NG\CMakeLists.txt" (
  echo [ERROR] Could not download CommonLibSSE-NG. >> "%LOG%"
  goto :fail
)

rem ---------- configure + build ----------
echo. & echo  Configuring...
echo --- configure >> "%LOG%"
"%CMAKE%" -S . -B build -A x64 ^
  -DCMAKE_CONFIGURATION_TYPES=Release ^
  -DCOMMONLIB_PREBUILT_MULTICONFIG=ON ^
  -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md ^
  -DVCPKG_INSTALLED_DIR="%TOOLS%\vcpkg_installed" >> "%LOG%" 2>&1
if errorlevel 1 (
  echo [ERROR] configure failed >> "%LOG%"
  goto :fail
)
echo. & echo  Compiling...
echo --- build >> "%LOG%"
"%CMAKE%" --build build --config Release >> "%LOG%" 2>&1
if errorlevel 1 (
  echo [ERROR] build failed >> "%LOG%"
  goto :fail
)

rem ---------- install into the MO2 mod folder ----------
if not exist "%MODDIR%\SKSE\Plugins" mkdir "%MODDIR%\SKSE\Plugins"
copy /y "build\Release\FactionWardrobe.dll" "%MODDIR%\SKSE\Plugins\" >> "%LOG%" 2>&1
if exist "build\Release\FactionWardrobe.pdb" copy /y "build\Release\FactionWardrobe.pdb" "%MODDIR%\SKSE\Plugins\" >> "%LOG%" 2>&1
if not exist "%MODDIR%\SKSE\Plugins\FactionWardrobe.json" copy /y "FactionWardrobe.json" "%MODDIR%\SKSE\Plugins\" >> "%LOG%" 2>&1
copy /y "esp\FactionWardrobe.esp" "%MODDIR%\" >> "%LOG%" 2>&1
echo [OK] Built and copied to "%MODDIR%" >> "%LOG%"
echo.
echo  Done. The plugin is in the MO2 mod "Faction Wardrobe". Enable it in MO2 if it is new.
echo.
pause
exit /b 0

:fail
echo.
echo  Build FAILED - see build.log (Claude can read it).
echo.
pause
exit /b 1
