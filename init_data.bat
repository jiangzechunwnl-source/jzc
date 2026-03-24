@echo off
REM ========================================================================
REM  初始化 data.txt 数据文件（一次性工具）
REM  功能：编译并运行 init_data.c，生成默认演出厅与管理员账户后删除临时 exe
REM ========================================================================

cd /d "%~dp0"
chcp 936 >nul 2>nul

set GCC=
where gcc >nul 2>&1
if %ERRORLEVEL% EQU 0 set GCC=gcc
if "%GCC%"=="" if exist "%~dp0tools\mingw64\bin\gcc.exe" set "GCC=%~dp0tools\mingw64\bin\gcc.exe"
if "%GCC%"=="" if exist "%ProgramFiles(x86)%\Dev-Cpp\MinGW64\bin\gcc.exe" set "GCC=%ProgramFiles(x86)%\Dev-Cpp\MinGW64\bin\gcc.exe"
if "%GCC%"=="" (
    echo gcc not found. Run setup.bat first.
    pause
    exit /b 1
)

"%GCC%" -o init_data.exe init_data.c
if %ERRORLEVEL% NEQ 0 (
    echo Compile failed
    pause
    exit /b 1
)

init_data.exe
del init_data.exe 2>nul
echo.
pause
