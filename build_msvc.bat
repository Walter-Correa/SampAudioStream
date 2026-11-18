@echo off
setlocal enabledelayedexpansion

echo ========================================================
echo  SampAudioStream - Building with MSVC (Visual Studio)
echo ========================================================

cd /d "%~dp0"

:: Auto-detect CMake in PATH, WinGet, or Visual Studio
where cmake >nul 2>nul
if errorlevel 1 (
    for /d %%D in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\Kitware.CMake_*") do (
        for /d %%C in ("%%D\cmake-*\bin") do (
            if exist "%%C\cmake.exe" set "PATH=%%C;!PATH!"
        )
    )
)
where cmake >nul 2>nul
if errorlevel 1 (
    if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
        set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;!PATH!"
    ) else if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
        set "PATH=%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;!PATH!"
    )
)

where cmake >nul 2>nul
if errorlevel 1 (
    echo [ERROR] CMake could not be found. Please ensure CMake or Visual Studio is installed.
    exit /b 1
)

:: 1. Configure & Build Win32 (32-bit for SA-MP & open.mp)
echo [1/3] Configuring CMake (Win32)...
cmake -B "build-win32" -G "Visual Studio 17 2022" -A Win32
if errorlevel 1 (
    echo [ERROR] CMake configuration failed.
    exit /b %errorlevel%
)

echo [2/3] Compiling Release binaries...
cmake --build "build-win32" --config Release --parallel
if errorlevel 1 (
    echo [ERROR] Compilation failed.
    exit /b %errorlevel%
)

:: 2. Run unit tests
ctest --test-dir "build-win32" -C Release -R core_tests --output-on-failure
if errorlevel 1 (
    echo [ERROR] Core unit tests failed.
    exit /b %errorlevel%
)

:: 3. Package and create Zip archive
echo [3/3] Creating standalone zip package...
set "STAGE_DIR=package-msvc"
if exist "%STAGE_DIR%" rmdir /s /q "%STAGE_DIR%"
if not exist "dist" mkdir "dist"

mkdir "%STAGE_DIR%\plugins"
mkdir "%STAGE_DIR%\components"
mkdir "%STAGE_DIR%\include"
mkdir "%STAGE_DIR%\filterscripts"

set "DLL_PATH=build-win32\Release\SampAudioStream.dll"
if not exist "%DLL_PATH%" (
    echo [ERROR] Expected binary "%DLL_PATH%" not found!
    exit /b 1
)

copy /Y "%DLL_PATH%" "%STAGE_DIR%\plugins\SampAudioStream.dll" >nul
copy /Y "%DLL_PATH%" "%STAGE_DIR%\components\SampAudioStream.dll" >nul

if exist "SampAudioStream.inc" copy /Y "SampAudioStream.inc" "%STAGE_DIR%\include\SampAudioStream.inc" >nul
if exist "FS_SampAudioStream_Demo.pwn" copy /Y "FS_SampAudioStream_Demo.pwn" "%STAGE_DIR%\filterscripts\FS_SampAudioStream_Demo.pwn" >nul
if exist "FS_SampAudioStream_Demo.amx" copy /Y "FS_SampAudioStream_Demo.amx" "%STAGE_DIR%\filterscripts\FS_SampAudioStream_Demo.amx" >nul
if exist "LICENSE" copy /Y "LICENSE" "%STAGE_DIR%\LICENSE" >nul

if exist "dist\SampAudioStream-Windows.zip" del /f /q "dist\SampAudioStream-Windows.zip"
where tar.exe >nul 2>nul
if not errorlevel 1 (
    tar.exe -a -c -f "dist\SampAudioStream-Windows.zip" -C "%STAGE_DIR%" *
) else (
    powershell -Command "Import-Module Microsoft.PowerShell.Archive; Compress-Archive -Path '%STAGE_DIR%\*' -DestinationPath 'dist\SampAudioStream-Windows.zip' -Force"
)

if exist "%STAGE_DIR%" rmdir /s /q "%STAGE_DIR%"

echo [SUCCESS] Package created: dist\SampAudioStream-Windows.zip

:: Optional deployment if target path is passed as argument or env var
set "TARGET_DIR=%~1"
if "%TARGET_DIR%"=="" set "TARGET_DIR=%OMP_SERVER_DIR%"

if not "%TARGET_DIR%"=="" (
    if exist "%TARGET_DIR%" (
        echo.
        echo Deploying to target directory: %TARGET_DIR%
        if exist "%TARGET_DIR%\components" copy /Y "%DLL_PATH%" "%TARGET_DIR%\components\SampAudioStream.dll" >nul
        if exist "%TARGET_DIR%\plugins" copy /Y "%DLL_PATH%" "%TARGET_DIR%\plugins\SampAudioStream.dll" >nul
        if exist "%TARGET_DIR%\pawno\include" copy /Y "SampAudioStream.inc" "%TARGET_DIR%\pawno\include\SampAudioStream.inc" >nul
        if exist "%TARGET_DIR%\qawno\include" copy /Y "SampAudioStream.inc" "%TARGET_DIR%\qawno\include\SampAudioStream.inc" >nul
        echo Deployment complete.
    )
)

echo ========================================================
echo  Build finished successfully!
echo ========================================================
