@echo off
setlocal
echo Compiling CSSO Tema 4 targets...

rem Find vcvars64 if not already in developer prompt
where cl.exe >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    if exist "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    )
)

echo [1/3] Building t4_worker.exe...
cl /nologo /W4 /std:c++17 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /O2 t4_worker.cpp t4_lib.cpp /Fe:t4_worker.exe /link shell32.lib

echo [2/3] Building t4_coord.exe...
cl /nologo /W4 /std:c++17 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /O2 t4_coord.cpp t4_engine.cpp t4_lib.cpp /Fe:t4_coord.exe /link shell32.lib

echo [3/3] Building t4_gui.exe...
cl /nologo /W4 /std:c++17 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /O2 t4_gui.cpp t4_engine.cpp t4_lib.cpp /Fe:t4_gui.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:t4_gui.manifest user32.lib gdi32.lib comctl32.lib ole32.lib uuid.lib shell32.lib

echo Done! Executables generated in current folder:
echo   - t4_worker.exe
echo   - t4_coord.exe
echo   - t4_gui.exe
