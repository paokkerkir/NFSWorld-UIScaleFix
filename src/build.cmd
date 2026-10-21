@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
cd /d "%~dp0"
if not exist ..\build mkdir ..\build
cl /nologo /O2 /MT /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /LD NFSWHUDScale.cpp /Fo..\build\ /Fe..\build\NFSW.HUDScale.asi /link /DLL user32.lib || exit /b 1
copy /y NFSW.HUDScale.ini ..\build\ >nul
echo Built ..\build\NFSW.HUDScale.asi
