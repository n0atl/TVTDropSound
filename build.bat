@echo off
rem Build DropSound.tvtp for x86 and x64
rem Output: bin\x86\DropSound.tvtp  bin\x64\DropSound.tvtp
setlocal

set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`%VSWHERE% -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (
	echo Visual Studio C++ tools not found.
	exit /b 1
)

set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
cd /d "%~dp0"
call :build x86 || exit /b 1
call :build x64 || exit /b 1
echo.
echo Build succeeded.
exit /b 0

:build
setlocal
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" %1 >nul || exit /b 1
set OUT=bin\%1
set OBJ=obj\%1
if not exist %OUT% mkdir %OUT%
if not exist %OBJ% mkdir %OBJ%
rc /nologo /fo %OBJ%\DropSound.res DropSound.rc || exit /b 1
cl /nologo /c /O2 /MT /EHsc /W3 /utf-8 /std:c++17 /DUNICODE /D_UNICODE /DNDEBUG /Fo%OBJ%\ DropSound.cpp || exit /b 1
link /nologo /DLL /DEF:DropSound.def /OUT:%OUT%\DropSound.tvtp %OBJ%\DropSound.obj %OBJ%\DropSound.res user32.lib winmm.lib comdlg32.lib advapi32.lib || exit /b 1
echo [%1] %OUT%\DropSound.tvtp
exit /b 0
