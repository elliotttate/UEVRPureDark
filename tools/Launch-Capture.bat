@echo off
setlocal enabledelayedexpansion
REM ============================================================
REM  UEVR + RenderDoc Capture - one-click launcher
REM
REM  USE EITHER WAY:
REM   * Drag your game's REAL exe onto this .bat, OR
REM   * Edit GAME_EXE below, then double-click this .bat.
REM ============================================================

REM ---- EDIT THESE (or just drag an exe onto this file) -------
REM Point at the BIG render exe under ...\Binaries\Win64\
REM (e.g. <Game>-Win64-Shipping.exe, or UnrealGame-Win64-Shipping.exe),
REM NOT a small <Game>.exe launcher stub at the install root.
set "GAME_EXE=C:\Path\To\YourGame-Win64-Shipping.exe"

REM Optional: folder the game must start from (leave blank if unsure).
set "GAME_CWD="

REM Optional: game command-line args (UE uses single-dash flags).
set "GAME_ARGS=-dx12"

REM Optional: if the game won't boot under the default path, set this to
REM e.g. 8000 to load RenderDoc first, let D3D12 start, then inject UEVR.
set "DEFER_MS="
REM ------------------------------------------------------------

REM Run from this script's own folder so the launcher finds its DLLs.
cd /d "%~dp0"

REM Drag-and-drop: an exe path dropped onto this .bat wins.
if not "%~1"=="" set "GAME_EXE=%~1"

if not exist "UEVRRenderDocLauncher.exe" (
  echo [ERROR] UEVRRenderDocLauncher.exe not found next to this .bat.
  echo Keep all the unzipped files together and run this from that folder.
  echo.
  pause
  exit /b 1
)

if not exist "%GAME_EXE%" (
  echo [ERROR] Game exe not found:
  echo     %GAME_EXE%
  echo.
  echo Edit GAME_EXE in this .bat, or drag your game's real exe onto it.
  echo Use the big exe under Binaries\Win64 -- NOT a small launcher stub.
  echo.
  pause
  exit /b 1
)

set "CMD=UEVRRenderDocLauncher.exe --exe "%GAME_EXE%""
if not "%GAME_CWD%"=="" set "CMD=!CMD! --cwd "%GAME_CWD%""
if not "%DEFER_MS%"=="" set "CMD=!CMD! --defer-backend-ms %DEFER_MS%"
if not "%GAME_ARGS%"=="" set "CMD=!CMD! --args "%GAME_ARGS%""
set "CMD=!CMD! --wait"

echo Running: !CMD!
echo.
!CMD!

echo.
echo ------------------------------------------------------------
echo Launcher exited. While the game is running and drawing frames,
echo open PowerShell in this folder and run:  .\Capture-RenderDoc.ps1
echo ------------------------------------------------------------
pause
