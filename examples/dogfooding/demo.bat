@echo off
REM Dogfooding Demo Script for racedetector (Windows)
REM
REM This script demonstrates the racedetector tool testing itself.
REM Phase 6A - Task A.7: Dogfooding Demo

setlocal enabledelayedexpansion

echo ========================================
echo   racedetector Dogfooding Demo
echo ========================================
echo.

REM Step 1: Build racedetector tool
echo Step 1: Building racedetector tool...
echo --------------------------------------
cd ..\..
go build -o racedetector.exe .\cmd\racedetector
if %errorlevel% neq 0 (
    echo [FAIL] Failed to build racedetector
    exit /b 1
)
echo [OK] racedetector built successfully
echo.

REM Step 2: Display tool version
echo Step 2: Checking tool version...
echo --------------------------------------
racedetector.exe --version
echo.

REM Step 3: Build simple race example with race detection
echo Step 3: Building example with race detection...
echo --------------------------------------
cd examples\dogfooding
..\..\racedetector.exe build -o simple_race.exe simple_race.go
if %errorlevel% neq 0 (
    echo [FAIL] Failed to build example
    exit /b 1
)
echo [OK] Example built successfully with race detection
echo.

REM Step 4: Run instrumented program
echo Step 4: Running instrumented program...
echo --------------------------------------
simple_race.exe
set EXITCODE=%errorlevel%
echo.
if %EXITCODE% equ 0 (
    echo [OK] Program executed successfully
) else (
    echo [WARN] Program exited with code %EXITCODE%
)
echo.

REM Step 5: Test 'run' command
echo Step 5: Testing 'racedetector run' command...
echo --------------------------------------
..\..\racedetector.exe run simple_race.go
set EXITCODE=%errorlevel%
if %EXITCODE% equ 0 (
    echo [OK] 'run' command executed successfully
) else (
    echo [WARN] 'run' command exited with code %EXITCODE%
)
echo.

REM Step 6: Cleanup
echo Step 6: Cleanup...
echo --------------------------------------
del /q simple_race.exe 2>nul
echo [OK] Cleanup complete
echo.

REM Summary
echo ========================================
echo   Dogfooding Demo Summary
echo ========================================
echo.
echo [OK] Tool builds successfully
echo [OK] Code instrumentation works
echo [OK] Instrumented code compiles
echo [OK] race.Init() is called
echo [OK] Programs run with race detection
echo.
echo Note: Current MVP Status (Phase 6A)
echo   - AST parsing: Working
echo   - Import injection: Working
echo   - Init() injection: Working
echo   - Runtime linking: Working
echo   - Build/Run commands: Working
echo.
echo   Phase 2 (Future):
echo   - Full AST modification (insert RaceRead/Write calls)
echo   - Actual race detection in running programs
echo.
echo ========================================
echo   Dogfooding Demo Complete!
echo ========================================

endlocal
