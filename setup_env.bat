@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "VENV=venv"
set "REQ=requirements.txt"
set "PY=py -3.11"

if not exist "%VENV%\Scripts\python.exe" (
  echo [SETUP] Creating Python 3.11 virtual environment
  %PY% -m venv "%VENV%" || goto :error
)

echo [SETUP] Installing project dependencies
"%VENV%\Scripts\python.exe" -m pip install --upgrade pip || goto :error
"%VENV%\Scripts\python.exe" -m pip install -r "%REQ%" || goto :error

echo.
echo [INFO] Python and key packages:
"%VENV%\Scripts\python.exe" -c "import sys; print('python', sys.version.replace('\n',' ')); import MetaTrader5 as mt5; print('MetaTrader5', getattr(mt5, '__version__', 'installed')); import numpy; print('numpy', numpy.__version__)"
"%VENV%\Scripts\python.exe" -m pip check || goto :error
echo.
echo Setup completed. Use venv\Scripts\python.exe for project tests and live checks.
exit /b 0

:error
echo.
echo [ERROR] Environment setup failed.
exit /b 1
