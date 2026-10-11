@echo off
rem eqcorrect - MinGW-w64 (MSYS2 など) ビルド。-static で DLL 依存をなくす。
rem AVX2 を使わない場合は -mavx2 を削除してください。
g++ -std=c++17 -O3 -Wall -Wextra -mavx2 -static -o eqcorrect.exe src\main.cpp
if errorlevel 1 (echo ビルド失敗 & exit /b 1)
echo ビルド成功: eqcorrect.exe
