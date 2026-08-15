@echo off
setlocal
cd /d "%~dp0"

rem ============================================================
rem  dsh-bridge one-click launcher
rem  Starts the LAN bridge that connects the Switch app to the
rem  DeepSeek Harness running on this PC (127.0.0.1:3080).
rem  Close this window to stop the bridge.
rem ============================================================

set "NODE=C:\Program Files\nodejs\node.exe"
if not exist "%NODE%" set "NODE=node"

echo.
echo  ============================================
echo   dsh-bridge - DeepSeek Harness LAN bridge
echo   Listening on 0.0.0.0:8765  -^>  http://127.0.0.1:3080
echo.

rem Try to show this PC's LAN IP so you know what to enter on the Switch
for /f %%i in ('powershell -NoProfile -Command "(Get-NetIPAddress -AddressFamily IPv4 ^| Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } ^| Select-Object -First 1 -ExpandProperty IPAddress)"') do set LANIP=%%i
if defined LANIP (
    echo   On the Switch, set harness_base_url to:
    echo       http://%LANIP%:8765
) else (
    echo   Run "ipconfig" to find this PC's LAN IP, then set:
    echo       harness_base_url = http://^<LAN-IP^>:8765
)
echo.
echo   First run: allow access when Windows Firewall asks.
echo   LAN/trusted networks only - never expose to the internet.
echo  ============================================
echo.

"%NODE%" bridge\dsh-bridge.js --host 0.0.0.0 --port 8765

echo.
echo  dsh-bridge exited.
pause
