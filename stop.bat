@echo off
taskkill /im iPodStats.exe /f >nul 2>&1 && echo iPodStats stopped. || echo iPodStats was not running.
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='powershell.exe'\" | Where-Object CommandLine -like '*media.ps1*' | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }"
pause

