@echo off
setlocal
cd /d "%~dp0"

echo Issuing a new invite code (data dir: %~dp0data)
echo.

partyup.exe invite --data "%~dp0data"

echo.
echo Press any key to close this window.
pause >nul
