@echo off
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" ( echo [ERREUR] vswhere introuvable & exit /b 1 )
set "VSINSTALL="
for /f "usebackq tokens=*" %%i in (`^""%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath^"`) do set "VSINSTALL=%%i"
if not defined VSINSTALL ( echo [ERREUR] Visual Studio avec outils C++ introuvable & exit /b 1 )
set "VCVARSALL=%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARSALL%" ( echo [ERREUR] vcvarsall introuvable & exit /b 1 )

call "%VCVARSALL%" amd64 >nul 2>&1
if errorlevel 1 ( echo [ERREUR] Initialisation de l'environnement MSVC & exit /b 1 )
cd /d "%~dp0"

del /q *.old ExplorerTint.exp ExplorerTint.lib 2>nul
if exist ExplorerTint.dll ren ExplorerTint.dll ExplorerTint.%RANDOM%.old
if exist obj rmdir /s /q obj
mkdir obj

rc /nologo /fo obj\ExplorerTint.res ExplorerTint.rc
if errorlevel 1 ( echo [ERREUR] rc.exe & exit /b 1 )

cl /nologo /LD /MT /EHsc /O2 /W3 /std:c++17 /utf-8 /DUNICODE /D_UNICODE /Foobj\ ^
   src\Common.cpp src\Settings.cpp src\GitStatus.cpp src\ClassicList.cpp src\ViewTint.cpp src\Bho.cpp src\DllMain.cpp ^
   obj\ExplorerTint.res /FeExplorerTint.dll /link /DEF:ExplorerTint.def
if errorlevel 1 ( echo [ERREUR] Compilation echouee & exit /b 1 )

echo.
echo OK : %CD%\ExplorerTint.dll
exit /b 0
