@echo off
setlocal
cd /d "%~dp0"
echo Starting Robot Voice Control (.NET 8)...
echo Open this computer's LAN address on your phone: http://COMPUTER-IP:5080
 echo.
dotnet --version >nul 2>&1
if errorlevel 1 (
  echo .NET 8 SDK was not found. Install the .NET 8 SDK, then run this file again.
  echo https://dotnet.microsoft.com/download/dotnet/8.0
  pause
  exit /b 1
)
dotnet restore
if errorlevel 1 (
  echo Restore failed. Check your internet connection and NuGet access.
  pause
  exit /b 1
)
dotnet run
pause
