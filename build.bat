@echo off
REM ---------------------------------------------------------------------------
REM build.bat —— DeskTidy 的 mingw 构建脚本。
REM
REM 本机环境（已实测确认）：
REM   Qt     : C:\Qt\6.11.1\mingw_64
REM   编译器 : C:\Qt\Tools\mingw1310_64\bin
REM   构建器 : C:\Qt\Tools\Ninja\ninja.exe
REM
REM 用法：
REM   build.bat            正常构建（输出到 build\）
REM   build.bat clean      先清空 build\ 再构建
REM ---------------------------------------------------------------------------

setlocal

set QT_DIR=C:\Qt\6.11.1\mingw_64
set MINGW_DIR=C:\Qt\Tools\mingw1310_64\bin
set NINJA_DIR=C:\Qt\Tools\Ninja

set PATH=%QT_DIR%\bin;%MINGW_DIR%;%NINJA_DIR%;%PATH%

cd /d "%~dp0"

if /i "%~1"=="clean" (
    echo [DeskTidy] 清空 build 目录...
    if exist build rmdir /s /q build
)

if not exist build mkdir build
cd build

echo [DeskTidy] 生成构建文件...
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=%QT_DIR% ..
if errorlevel 1 goto :fail

echo [DeskTidy] 编译...
cmake --build .
if errorlevel 1 goto :fail

echo.
echo [DeskTidy] 构建成功 -> build\DeskTidy.exe
exit /b 0

:fail
echo.
echo [DeskTidy] 构建失败，请看上方错误输出。
exit /b 1
