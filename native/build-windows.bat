@echo off
setlocal enabledelayedexpansion

if "%CI_COMMIT_TAG%"=="" (
    echo ERROR: CI_COMMIT_TAG is not set. This job must run from a tag pipeline.
    exit /b 1
)

IF EXIST build (
    rmdir /s /q build
)

mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --config Release
cd Release

set "VERSION=%CI_COMMIT_TAG%"
set "ARCH=%PROCESSOR_ARCHITECTURE%"
set "ZIP_NAME=JustCefNative-windows-%ARCH%-%VERSION%.zip"

powershell Compress-Archive -Path * -DestinationPath "%ZIP_NAME%"

set "AWS_ACCESS_KEY_ID=%CF_R2_ACCESS_KEY_ID%"
set "AWS_SECRET_ACCESS_KEY=%CF_R2_SECRET_ACCESS_KEY%"
set "AWS_DEFAULT_REGION=auto"

aws s3 cp "%ZIP_NAME%" "s3://%CF_R2_BUCKET%/justcef/%VERSION%/%ZIP_NAME%" --endpoint-url "https://%CF_R2_ACCOUNT_ID%.r2.cloudflarestorage.com"

if errorlevel 1 exit /b 1

echo Uploaded: https://static.grayjay.app/dotcefnative/%VERSION%/%ZIP_NAME%