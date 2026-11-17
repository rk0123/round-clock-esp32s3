@echo off
REM ============================================================
REM  Build RoundClock firmware with Arduino IDE's bundled arduino-cli
REM  Usage: build.bat
REM ============================================================
setlocal
set CLI=C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe
REM %~dp0 ends with a backslash; quoting it escapes the quote, so normalize it
for %%I in ("%~dp0.") do set "SKETCH=%%~fI"
set BUILD=%~dp0.build
set FQBN=esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,CDCOnBoot=cdc,CPUFreq=240,UploadSpeed=921600,PartitionScheme=app3M_fat9M_16MB

if not exist "%CLI%" (
  echo [ERROR] arduino-cli not found: %CLI%
  exit /b 1
)
echo Building %SKETCH%
"%CLI%" compile --fqbn "%FQBN%" --build-path "%BUILD%" "%SKETCH%"
if errorlevel 1 ( echo [FAIL] build failed & exit /b 1 )
echo.
echo [OK] artifacts in %BUILD%
dir /b "%BUILD%\*.bin"
endlocal
