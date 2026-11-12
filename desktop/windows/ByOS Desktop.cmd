@echo off
rem ByOS Desktop for Windows: starts the local runner and opens the app in its own window.
rem Needs Node.js 20 or newer and Java 17 or newer. adb (Android platform-tools) is needed for
rem USB connections to the HiBy.
setlocal
cd /d "%~dp0"

where node >nul 2>nul
if errorlevel 1 (
  echo ByOS Desktop needs Node.js 20 or newer.
  echo Install the LTS version from https://nodejs.org and start ByOS Desktop again.
  pause
  exit /b 1
)
for /f "delims=" %%v in ('node -p "process.versions.node.split('.')[0]"') do set NODEMAJOR=%%v
if %NODEMAJOR% LSS 20 (
  echo ByOS Desktop needs Node.js 20 or newer. This computer has Node.js %NODEMAJOR%.
  echo Install the LTS version from https://nodejs.org and start ByOS Desktop again.
  pause
  exit /b 1
)

set "JAVACMD=java"
if defined JAVA_HOME set "JAVACMD=%JAVA_HOME%\bin\java"
"%JAVACMD%" -version >nul 2>nul
if errorlevel 1 (
  echo ByOS Desktop needs Java 17 or newer.
  echo Install it from https://adoptium.net ^(Temurin 21 LTS^) and start ByOS Desktop again.
  pause
  exit /b 1
)

set "ADBARG="
where adb >nul 2>nul
if errorlevel 1 (
  if exist "%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe" (
    set ADBARG=--adb "%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe"
  ) else if exist "%~dp0platform-tools\adb.exe" (
    set ADBARG=--adb "%~dp0platform-tools\adb.exe"
  ) else (
    echo Note: adb was not found. To reach the HiBy over USB, download Google's
    echo "SDK Platform-Tools for Windows" from https://developer.android.com/tools/releases/platform-tools
    echo and unzip its platform-tools folder next to this file.
    echo.
  )
)

node "%~dp0app\server.mjs" --open --cli "%~dp0byos-cli.jar" %ADBARG% %*
if errorlevel 1 pause
