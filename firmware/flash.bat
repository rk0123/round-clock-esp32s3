@echo off
REM ============================================================
REM  Build + flash to the ESP32-S3-Touch-LCD-1.85B
REM  Usage: flash.bat          (defaults to COM12)
REM         flash.bat COM5     (override port)
REM ============================================================
setlocal
set CLI=C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe
REM %~dp0 ends with a backslash; quoting it escapes the quote, so normalize it
for %%I in ("%~dp0.") do set "SKETCH=%%~fI"
set BUILD=%~dp0.build
set FQBN=esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,CDCOnBoot=cdc,CPUFreq=240,UploadSpeed=921600,PartitionScheme=app3M_fat9M_16MB
set PORT=%1
if "%PORT%"=="" set PORT=COM12

echo === [1/2] Building ===
"%CLI%" compile --fqbn "%FQBN%" --build-path "%BUILD%" "%SKETCH%"
if errorlevel 1 ( echo [FAIL] build failed & exit /b 1 )

echo.
echo === [2/2] Flashing %PORT% ===
"%CLI%" upload --fqbn "%FQBN%" --port %PORT% --build-path "%BUILD%" "%SKETCH%"
if errorlevel 1 ( echo [FAIL] upload failed & exit /b 1 )

echo.
echo [DONE] Open a serial monitor on %PORT% (USB CDC, any baudrate).
echo        Type HELP to see runtime config commands.
endlocal
