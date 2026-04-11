@echo off
setlocal EnableExtensions

for %%I in ("%~dp0..\..") do set "REPO_ROOT=%%~fI"
set /p CEF_BRANCH=<"%REPO_ROOT%\cef.branch"
if not defined CEF_BRANCH (
  echo Failed to read CEF branch from "%REPO_ROOT%\cef.branch"
  exit /b 1
)
set "CHECKOUT_ARG="
set "CEF_CHECKOUT_NORMALIZED="
if defined CEF_CHECKOUT (
  set "CEF_CHECKOUT_NORMALIZED=%CEF_CHECKOUT%"
  call set "CEF_CHECKOUT_NORMALIZED=%%CEF_CHECKOUT_NORMALIZED:*+g=%%"
  for /f "tokens=1 delims=+" %%J in ("%CEF_CHECKOUT_NORMALIZED%") do set "CEF_CHECKOUT_NORMALIZED=%%J"
  if /I "%CEF_CHECKOUT_NORMALIZED:~0,1%"=="g" set "CEF_CHECKOUT_NORMALIZED=%CEF_CHECKOUT_NORMALIZED:~1%"
  set "CHECKOUT_ARG=--checkout=%CEF_CHECKOUT_NORMALIZED%"
)

set "GYP_MSVS_VERSION=2022"
set "CEF_ARCHIVE_FORMAT=tar.bz2"
set "CEF_CUSTOM_PATCH_SCRIPT=%REPO_ROOT%\patches\apply_cef_patches.py"

call :check_pagefile
if errorlevel 1 exit /b 1

set "BUILD_X64=0"
set "BUILD_ARM64=0"

if "%~1"=="" (
  set "BUILD_X64=1"
  set "BUILD_ARM64=1"
  goto args_done
)

:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="all" (
  set "BUILD_X64=1"
  set "BUILD_ARM64=1"
) else if /I "%~1"=="x64" (
  set "BUILD_X64=1"
) else if /I "%~1"=="arm64" (
  set "BUILD_ARM64=1"
) else (
  echo Usage: %~nx0 [x64] [arm64] [all]
  exit /b 1
)
shift
goto parse_args

:args_done
if "%BUILD_X64%"=="1" (
  call :run_build x64
  if errorlevel 1 exit /b 1
)

if "%BUILD_ARM64%"=="1" (
  call :run_build arm64
  if errorlevel 1 exit /b 1
)

exit /b 0

:run_build
setlocal
set "ARCH=%~1"
set "ARCH_ARG="
set "CEF_ENABLE_ARM64="
set "GN_DEFINES="

if /I "%ARCH%"=="x64" (
  set "ARCH_ARG=--x64-build"
  set "GN_DEFINES=is_official_build=true proprietary_codecs=true ffmpeg_branding=Chrome is_component_build=false is_debug=false enable_widevine=true enable_printing=true enable_cdm_host_verification=true angle_enable_vulkan_validation_layers=false dawn_enable_vulkan_validation_layers=false dawn_use_built_dxc=false"
) else if /I "%ARCH%"=="arm64" (
  set "ARCH_ARG=--arm64-build"
  set "CEF_ENABLE_ARM64=1"
  set "GN_DEFINES=is_official_build=true proprietary_codecs=true ffmpeg_branding=Chrome is_debug=false chrome_pgo_phase=0 use_thin_lto=false enable_widevine=true enable_printing=true enable_cdm_host_verification=true angle_enable_vulkan_validation_layers=false dawn_enable_vulkan_validation_layers=false dawn_use_built_dxc=false"
) else (
  echo Unsupported architecture: %ARCH%
  exit /b 1
)

echo ==^> Starting %ARCH% build for branch %CEF_BRANCH%
if defined CEF_CHECKOUT echo ==^> Pinning CEF checkout to %CEF_CHECKOUT_NORMALIZED% ^(from %CEF_CHECKOUT%^)
python "%REPO_ROOT%\automate\automate-git.py" --branch=%CEF_BRANCH% --download-dir=c:\code\chromium_git --depot-tools-dir=c:\code\depot_tools --minimal-distrib-only --build-target=cefsimple --force-clean --force-build --with-pgo-profiles --no-debug-build %ARCH_ARG% %CHECKOUT_ARG%
set "RESULT=%ERRORLEVEL%"
endlocal & exit /b %RESULT%

:check_pagefile
setlocal EnableExtensions
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$requiredMb = 32768; " ^
  "$sum = (Get-CimInstance -ClassName Win32_PageFileUsage | Measure-Object -Property AllocatedBaseSize -Sum).Sum; " ^
  "$allocatedMb = if ($null -eq $sum) { 0 } else { [int]$sum }; " ^
  "if ($allocatedMb -lt $requiredMb) { Write-Host ('Active page file is {0} MB; at least {1} MB is required. If setup.bat already configured it, reboot once and retry.' -f $allocatedMb, $requiredMb); exit 1 }; " ^
  "Write-Host ('==> Active page file: {0} MB' -f $allocatedMb)"
if errorlevel 1 (
  endlocal & exit /b 1
)
endlocal & exit /b 0
