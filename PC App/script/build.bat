@echo off
set "SCRIPT=xboxrgb.pyw"
set "NAME=XBOX RGB Control"
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
