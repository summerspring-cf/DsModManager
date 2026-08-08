@echo off
rem DsCppModManager build -- MSVC /MD Release ONLY.
rem (Why: see ue4ss_abi.hpp header comment. Debug STL layout differs -> crash.)
rem NOTE: keep this file ASCII-only. cmd.exe reads batch in cp949 and
rem       UTF-8 Korean comments break parsing.
setlocal
cd /d "%~dp0"

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

if not exist UE4SS.def (
    python ..\..\tools\make_ue4ss_def.py || exit /b 1
)
if not exist UE4SS.lib (
    lib /nologo /def:UE4SS.def /machine:x64 /out:UE4SS.lib || exit /b 1
)

cl /nologo /std:c++latest /utf-8 /EHsc /MD /O2 /W3 /D NDEBUG /LD main.cpp /Fe:main.dll /link UE4SS.lib user32.lib shell32.lib
if errorlevel 1 exit /b 1

echo.
echo == build OK: main.dll
endlocal
