@echo off
setlocal enabledelayedexpansion

if "%CI_COMMIT_TAG%"=="" (
    echo ERROR: CI_COMMIT_TAG is not set. This job must run from a tag pipeline.
    exit /b 1
)

set "TARGET_ARCH=%~1"
if "%TARGET_ARCH%"=="" set "TARGET_ARCH=%PROCESSOR_ARCHITECTURE%"

set "CMAKE_ARCH="
set "ZIP_ARCH="
if /I "%TARGET_ARCH%"=="AMD64" (
    set "CMAKE_ARCH=x64"
    set "ZIP_ARCH=AMD64"
) else if /I "%TARGET_ARCH%"=="x64" (
    set "CMAKE_ARCH=x64"
    set "ZIP_ARCH=AMD64"
) else if /I "%TARGET_ARCH%"=="ARM64" (
    set "CMAKE_ARCH=ARM64"
    set "ZIP_ARCH=ARM64"
) else (
    echo ERROR: Unsupported Windows target architecture: %TARGET_ARCH%
    echo Usage: build-windows.bat [x64^|arm64]
    exit /b 1
)

IF EXIST build (
    rmdir /s /q build
)

mkdir build
cd build
cmake -A %CMAKE_ARCH% -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --config Release
cd Release

set "VERSION=%CI_COMMIT_TAG%"
set "ZIP_NAME=JustCefNative-windows-%ZIP_ARCH%.zip"

if exist "%ZIP_NAME%" del /q "%ZIP_NAME%"
dir
echo zip -x "%ZIP_NAME%" -x "justcefnative.exp" -x "justcefnative.lib" -r "%ZIP_NAME%" .
zip -x "%ZIP_NAME%" -x "justcefnative.exp" -x "justcefnative.lib" -r "%ZIP_NAME%" .
if errorlevel 1 exit /b 1

set "AWS_ACCESS_KEY_ID=%CF_R2_ACCESS_KEY_ID%"
set "AWS_SECRET_ACCESS_KEY=%CF_R2_SECRET_ACCESS_KEY%"
set "AWS_DEFAULT_REGION=auto"

aws s3 cp "%ZIP_NAME%" "s3://%CF_R2_BUCKET%/justcef/%VERSION%/%ZIP_NAME%" --endpoint-url "https://%CF_R2_ACCOUNT_ID%.r2.cloudflarestorage.com"

if errorlevel 1 exit /b 1

echo Uploaded: https://static.grayjay.app/justcef/%VERSION%/%ZIP_NAME%
