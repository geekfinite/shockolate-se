@echo off
rem ---------------------------------------------------------------------------------------
rem  Shockolate SE -- Windows launcher for build.sh
rem
rem  Double-click this file, or run it from a normal cmd/PowerShell prompt:
rem      build.cmd                 native toolchain
rem      build.cmd --arch 64       force the 64-bit toolchain
rem      build.cmd --verify        verify cached archives
rem      build.cmd --offline       build with no network access at all
rem
rem  It locates MSYS2, selects the MINGW32/MINGW64 environment, and runs build.sh inside it
rem  with the correct PATH.  build.sh itself resolves its own directory, so the repository
rem  can live anywhere.
rem ---------------------------------------------------------------------------------------
setlocal

set "REPO_DIR=%~dp0"
cd /d "%REPO_DIR%"

rem --- locate MSYS2 -----------------------------------------------------------------------
if not defined MSYS2_ROOT (
	for %%D in ("L:\msys64" "C:\msys64" "D:\msys64" "E:\msys64" "%SystemDrive%\msys64" "%USERPROFILE%\msys64") do (
		if not defined MSYS2_ROOT if exist "%%~D\usr\bin\bash.exe" set "MSYS2_ROOT=%%~D"
	)
)
if not defined MSYS2_ROOT (
	echo.
	echo ERROR: MSYS2 was not found.
	echo        Install it from https://www.msys2.org/ or set MSYS2_ROOT, for example:
	echo            set MSYS2_ROOT=D:\msys64
	echo.
	if "%~1"=="" pause
	exit /b 1
)

rem --- choose the toolchain from --arch ---------------------------------------------------
set "WANT_ARCH="
echo %* | findstr /C:"--arch=64" >nul && set "WANT_ARCH=64"
echo %* | findstr /C:"--arch 64" >nul && set "WANT_ARCH=64"
echo %* | findstr /C:"--arch=32" >nul && set "WANT_ARCH=32"
echo %* | findstr /C:"--arch 32" >nul && set "WANT_ARCH=32"

if "%WANT_ARCH%"=="64" goto :arch64
if "%WANT_ARCH%"=="32" goto :arch32

rem No --arch given: build.sh detects the target from the active compiler, so just pick the
rem environment that actually has a compiler, preferring the one matching the prebuilt
rem MSVCRT SDL packages (32-bit) when both are installed.
if exist "%MSYS2_ROOT%\mingw32\bin\gcc.exe" goto :arch32
if exist "%MSYS2_ROOT%\mingw64\bin\gcc.exe" goto :arch64
echo.
echo ERROR: neither MINGW32 nor MINGW64 has a compiler installed.
echo        Run this once, then try again:
echo            %MSYS2_ROOT%\mingw32.exe   -^>   pacman -S mingw-w64-i686-toolchain
echo.
if "%~1"=="" pause
exit /b 1

:arch32
set "MSYSTEM=MINGW32"
set "MSYS_PREFIX=%MSYS2_ROOT%\mingw32"
goto :run

:arch64
if not exist "%MSYS2_ROOT%\mingw64\bin\gcc.exe" (
	echo.
	echo ERROR: the 64-bit toolchain is not installed in %MSYS2_ROOT%\mingw64.
	echo        Install it from the MINGW64 shell:
	echo            pacman -S mingw-w64-x86_64-toolchain
	echo.
	if "%~1"=="" pause
	exit /b 1
)
set "MSYSTEM=MINGW64"
set "MSYS_PREFIX=%MSYS2_ROOT%\mingw64"
goto :run

:run
if not exist "%MSYS2_ROOT%\usr\bin\bash.exe" (
	echo ERROR: %MSYS2_ROOT%\usr\bin\bash.exe is missing.
	if "%~1"=="" pause
	exit /b 1
)
if not exist "%REPO_DIR%build.sh" (
	echo ERROR: build.sh was not found next to this launcher.
	if "%~1"=="" pause
	exit /b 1
)

echo === Shockolate SE: MSYSTEM=%MSYSTEM% ===
rem A login shell so /etc/profile sets up the MSYSTEM-appropriate PATH for us.
"%MSYS2_ROOT%\usr\bin\bash.exe" -lc "./build.sh %*"
set "RC=%ERRORLEVEL%"

if not "%RC%"=="0" (
	echo.
	echo Build failed with exit code %RC%.
)
if "%~1"=="" pause
exit /b %RC%
