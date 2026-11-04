@echo off
setlocal
cd /d "%~dp0"

echo ============================================================
echo  PartyUp server
echo ============================================================
echo  Keep this window open while the server is running.
echo  On first start, look for these lines in the output below:
echo      BOOTSTRAP_TOKEN=...   admin / bootstrap token
echo      INVITE_CODE=...       invite code to share with friends
echo  Press Ctrl+C to stop the server.
echo ============================================================
echo.

partyup.exe serve --data "%~dp0data" --http 0.0.0.0:8080

echo.
echo Server stopped. Press any key to close this window.
pause >nul
