@echo off
setlocal enabledelayedexpansion
rem Workspace wrapper around GSPC 0.3.0: its depfiles contain Windows
rem extended-length prefixes (\\?\D:\...) that CMake's Ninja depfile
rem transform cannot handle. Strip the prefix after a successful run.
set "DEPFILE="
set "PREV="
for %%A in (%*) do (
  if /I "!PREV!"=="--depfile" set "DEPFILE=%%~A"
  set "PREV=%%~A"
)
"C:\Users\0\.cache\esp-mosaico\gspc\0.3.0\gspc.exe" %*
set RC=%ERRORLEVEL%
if not "%DEPFILE%"=="" if %RC%==0 (
  "D:\ESP\espressif-tools\python_env\idf6.2_py3.11_env\Scripts\python.exe" -c "import sys,pathlib;p=pathlib.Path(sys.argv[1]);t=p.read_text(encoding='utf-8');p.write_text(t.replace('\\\\','\\').replace('\\\\?\\',''),encoding='utf-8')" "%DEPFILE%"
)
exit /b %RC%
