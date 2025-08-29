@echo off
set "SCRIPT=xboxrgb_flash.pyw"
set "NAME=XBOX RGB Flasher"
set "ICON=dc.ico"

pyinstaller ^
  --onefile ^
  --windowed ^
  --noconsole ^
  --clean ^
  --name "%NAME%" ^
  --icon "%ICON%" ^
  --add-data "%ICON%;." ^
  "%SCRIPT%"
