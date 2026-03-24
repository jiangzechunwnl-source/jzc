@echo off
REM 关闭命令回显，减少批处理自身输出干扰

REM TTMS Web（C 后端）：与 web+bat 版同端口、同 API
chcp 65001 >nul
REM 切换控制台代码页为 UTF-8，便于 printf 中文

cd /d "%~dp0"
REM 进入本 bat 所在目录（项目根），保证相对路径编译与运行正确

set SRC=src
REM 源码根目录名，供 gcc -I 使用

set INC=-I%SRC% -I%SRC%\View -I%SRC%\Service -I%SRC%\Persistence -I%SRC%\Common
REM 头文件搜索路径

set GCC=
REM 待检测到的 gcc 可执行文件路径

where gcc >nul 2>&1
REM 系统 PATH 中是否有 gcc

if %ERRORLEVEL% EQU 0 set GCC=gcc
REM 若存在则直接用 gcc 命令名

if "%GCC%"=="" if exist "%~dp0tools\mingw64\bin\gcc.exe" set "GCC=%~dp0tools\mingw64\bin\gcc.exe"
REM 项目自带 MinGW 路径

if "%GCC%"=="" if exist "%ProgramFiles(x86)%\Dev-Cpp\MinGW64\bin\gcc.exe" set "GCC=%ProgramFiles(x86)%\Dev-Cpp\MinGW64\bin\gcc.exe"
REM Dev-C++ 自带 64 位 MinGW

if "%GCC%"=="" if exist "%ProgramFiles%\JetBrains\CLion 2025.2.3\bin\mingw\bin\gcc.exe" set "GCC=%ProgramFiles%\JetBrains\CLion 2025.2.3\bin\mingw\bin\gcc.exe"
REM CLion 捆绑 MinGW（路径按本机安装可能不同）

if "%GCC%"=="" (
    echo gcc not found
    pause
    exit /b 1
)
REM 仍未找到 gcc 则报错退出

REM 若旧版 TTMS_Web.exe 仍在运行，链接器无法覆盖 exe（Permission denied）
taskkill /F /IM TTMS_Web.exe >nul 2>&1
if %ERRORLEVEL% EQU 0 echo 已结束旧的 TTMS_Web.exe 进程，以便重新编译。

echo Compiling TTMS_Web.exe...
"%GCC%" -std=c99 -finput-charset=UTF-8 -fexec-charset=UTF-8 -o TTMS_Web.exe web_server.c %SRC%\Persistence\Data_Store.c %SRC%\Persistence\Studio_Persist.c %SRC%\Persistence\Seat_Persist.c %SRC%\Persistence\Play_Persist.c %SRC%\Persistence\Schedule_Persist.c %SRC%\Persistence\Ticket_Persist.c %SRC%\Persistence\Account_Persist.c %SRC%\Persistence\EntityKey_Persist.c %SRC%\Service\Studio.c %SRC%\Service\Seat.c %SRC%\Service\Play.c %SRC%\Service\Schedule.c %SRC%\Service\Ticket.c %SRC%\Service\Account.c %INC% -lws2_32
REM 编译 web_server.c 及业务层源码，链接 ws2_32（Winsock）

if %ERRORLEVEL% NEQ 0 (
    echo Build failed
    pause
    exit /b 1
)
REM 编译失败则暂停以便查看错误

echo.
echo 内置管理员: admin / admin
echo 静态图片目录: static\  （background.png、1.png~8.png 等）
echo Starting http://127.0.0.1:8765/
start "" "http://127.0.0.1:8765/index.html"
REM 默认浏览器打开前端页面

TTMS_Web.exe
REM 启动 HTTP 服务（阻塞至窗口关闭）

pause
REM 服务退出后暂停，方便查看控制台输出
