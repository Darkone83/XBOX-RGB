@echo off
set "SCRIPT=rgb_sim.pyw"
set "NAME=XBOX RGB Sim"

pyinstaller ^
  --onefile ^
  --windowed ^
  --noconsole ^
  --clean ^
  --name "%NAME%" ^
  "%SCRIPT%"
