@echo off
setlocal EnableExtensions
set "REBOOT_REQUIRED=0"

REM Install chocolatey
powershell -NoProfile -InputFormat None -ExecutionPolicy Bypass -Command "Set-ExecutionPolicy Bypass -Scope Process -Force; [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.ServicePointManager]::SecurityProtocol -bor 3072; iex ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))"
if errorlevel 1 exit /b 1

REM Refresh environment
call C:\ProgramData\chocolatey\bin\refreshenv.cmd
if errorlevel 1 exit /b 1

REM Enable Chocolatey global confirmation
choco feature enable -n allowGlobalConfirmation
if errorlevel 1 exit /b 1

REM Install Git
choco install git -y
if errorlevel 1 exit /b 1

REM Refresh environment
call C:\ProgramData\chocolatey\bin\refreshenv.cmd
if errorlevel 1 exit /b 1

REM Check git version
git --version
if errorlevel 1 exit /b 1

REM Apply depot_tools recommended Git settings for Chromium checkouts.
git config --global core.autocrlf false
if errorlevel 1 exit /b 1
git config --global core.filemode false
if errorlevel 1 exit /b 1
git config --global core.fscache true
if errorlevel 1 exit /b 1
git config --global core.preloadindex true
if errorlevel 1 exit /b 1

REM Install Python 3.11
choco install python311 -y
if errorlevel 1 exit /b 1

REM Install CMake
choco install cmake -y
if errorlevel 1 exit /b 1

REM Refresh environment
call C:\ProgramData\chocolatey\bin\refreshenv.cmd
if errorlevel 1 exit /b 1

REM Install VS Community
curl -L -o vs_Community.exe https://download.visualstudio.microsoft.com/download/pr/69e24482-3b48-44d3-af65-51f866a08313/14d102a0ef8816239ecf4ccaa417f0d517787d637dd61a2f69914845899e3b78/vs_Community.exe
if errorlevel 1 exit /b 1
vs_Community.exe --passive --wait --norestart --config C:\code\wdk.vsconfig --add Microsoft.VisualStudio.Component.Windows11SDK.26100
if errorlevel 1 exit /b 1

REM Install the latest serviced Windows 11 SDK release, not just the base 26100.0 component.
curl -L -o winsdksetup.exe https://go.microsoft.com/fwlink/?linkid=2349110
if errorlevel 1 exit /b 1
winsdksetup.exe /quiet /norestart /ceip off /features OptionId.DesktopCPPx64 OptionId.DesktopCPParm64 OptionId.SigningTools OptionId.WindowsDesktopDebuggers /log "%USERPROFILE%\Desktop\sdk-install.log"
if errorlevel 1 exit /b 1

REM Configure a persistent 32 GiB page file for large Chromium link steps.
REM Page file settings are applied at system startup, so a reboot may be required
REM before the full configured size shows up in Win32_PageFileUsage.
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$desiredMb = 32768; " ^
  "$pageFilePath = 'C:\pagefile.sys'; " ^
  "$computerSystem = Get-CimInstance -ClassName Win32_ComputerSystem; " ^
  "if ($computerSystem.AutomaticManagedPagefile) { Set-CimInstance -InputObject $computerSystem -Property @{AutomaticManagedPagefile = $false} | Out-Null }; " ^
  "$pageSetting = Get-CimInstance -ClassName Win32_PageFileSetting | Where-Object { $_.Name -ieq $pageFilePath } | Select-Object -First 1; " ^
  "if (-not $pageSetting) { New-CimInstance -ClassName Win32_PageFileSetting -Property @{Name = $pageFilePath; InitialSize = $desiredMb; MaximumSize = $desiredMb} | Out-Null } else { $initial = [Math]::Max([int]$pageSetting.InitialSize, $desiredMb); $maximum = [Math]::Max([int]$pageSetting.MaximumSize, $desiredMb); Set-CimInstance -InputObject $pageSetting -Property @{InitialSize = $initial; MaximumSize = $maximum} | Out-Null }; " ^
  "$sum = (Get-CimInstance -ClassName Win32_PageFileUsage | Measure-Object -Property AllocatedBaseSize -Sum).Sum; " ^
  "$allocatedMb = if ($null -eq $sum) { 0 } else { [int]$sum }; " ^
  "if ($allocatedMb -lt $desiredMb) { Write-Host ('Configured page file settings. Current active page file is {0} MB and requires a reboot before {1} MB is available.' -f $allocatedMb, $desiredMb); exit 2 } else { Write-Host ('Active page file: {0} MB' -f $allocatedMb); exit 0 }"
set "PAGEFILE_STATUS=%ERRORLEVEL%"
if "%PAGEFILE_STATUS%"=="1" exit /b 1
if "%PAGEFILE_STATUS%"=="2" set "REBOOT_REQUIRED=1"

REM Clone repos
cd C:/code
git clone https://github.com/chromiumembedded/cef.git chromium_git/cef
if errorlevel 1 exit /b 1
git clone https://github.com/chromium/chromium.git chromium_git/chromium/src
if errorlevel 1 exit /b 1
git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git depot_tools
if errorlevel 1 exit /b 1

if "%REBOOT_REQUIRED%"=="1" (
  echo Rebooting in 30 seconds to activate the configured 32 GiB page file.
  shutdown /r /t 30 /f /c "Completing CEF builder setup: activating 32 GiB page file"
  if errorlevel 1 exit /b 1
)
