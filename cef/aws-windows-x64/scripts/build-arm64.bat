@echo off
call "%~dp0build.bat" arm64
exit /b %ERRORLEVEL%
