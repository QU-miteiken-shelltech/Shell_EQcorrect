@echo off
rem eqcorrect - MSVC ビルド。 「x64 Native Tools Command Prompt for VS」から実行してください。
rem /utf-8 : ソース内の日本語文字列を正しく扱うために必須   /arch:AVX2 : AVX2 版 (古いCPUでは行を /arch:AVX2 なしに変更)
cl /nologo /std:c++17 /O2 /EHsc /utf-8 /W3 /arch:AVX2 src\main.cpp /Fe:eqcorrect.exe
if errorlevel 1 (echo ビルド失敗 & exit /b 1)
echo ビルド成功: eqcorrect.exe
