@echo off
REM ========================================================================
REM  TTMS console launcher (MinGW gcc -> TTMS.exe, then run)
REM  Sources: TTMS.c, src\View, Service, Persistence, Common
REM  Data file: data.txt in same folder as TTMS.exe
REM  Note: keep REM lines ASCII-only so cmd.exe parses this file reliably
REM ========================================================================

cd /d "%~dp0"
chcp 936 >nul 2>nul

set SRC=src
set INC=-I%SRC% -I%SRC%\View -I%SRC%\Service -I%SRC%\Persistence -I%SRC%\Common

set GCC=
where gcc >nul 2>&1
if %ERRORLEVEL% EQU 0 set GCC=gcc
if "%GCC%"=="" if exist "%~dp0tools\mingw64\bin\gcc.exe" set "GCC=%~dp0tools\mingw64\bin\gcc.exe"
if "%GCC%"=="" if exist "%ProgramFiles(x86)%\Dev-Cpp\MinGW64\bin\gcc.exe" set "GCC=%ProgramFiles(x86)%\Dev-Cpp\MinGW64\bin\gcc.exe"
if "%GCC%"=="" if exist "%ProgramFiles%\JetBrains\CLion 2025.2.3\bin\mingw\bin\gcc.exe" set "GCC=%ProgramFiles%\JetBrains\CLion 2025.2.3\bin\mingw\bin\gcc.exe"
if "%GCC%"=="" (
    echo Compile failed: gcc not found
    pause
    exit /b 1
)

taskkill /f /im TTMS.exe >nul 2>nul
ping 127.0.0.1 -n 2 >nul

echo Compiling...
"%GCC%" -std=c99 -finput-charset=UTF-8 -fexec-charset=GBK -o TTMS.exe TTMS.c %SRC%\View\Main_Menu.c %SRC%\View\Studio_UI.c %SRC%\View\Seat_UI.c %SRC%\View\Play_UI.c %SRC%\View\Ticket_UI.c %SRC%\View\Query_UI.c %SRC%\View\Ranking_UI.c %SRC%\View\Account_UI.c %SRC%\Service\Studio.c %SRC%\Service\Seat.c %SRC%\Service\Play.c %SRC%\Service\Schedule.c %SRC%\Service\Ticket.c %SRC%\Service\Account.c %SRC%\Persistence\Studio_Persist.c %SRC%\Persistence\Seat_Persist.c %SRC%\Persistence\Play_Persist.c %SRC%\Persistence\Schedule_Persist.c %SRC%\Persistence\Ticket_Persist.c %SRC%\Persistence\Account_Persist.c %SRC%\Persistence\EntityKey_Persist.c %SRC%\Persistence\Data_Store.c %INC%

if %ERRORLEVEL% EQU 0 (
    echo Success!
    TTMS.exe
) else (
    echo Failed
)

echo.
pause
