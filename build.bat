@echo off
setlocal
set SRC=src\main.c src\logger.c src\json.c src\http.c src\config.c src\providers.c src\poller.c src\ws.c src\alerts.c src\taskbar.c src\widget.c src\ui_settings.c

if /I "%1"=="mingw" goto mingw
if /I "%1"=="msvc" goto msvc

where cl >nul 2>nul
if %ERRORLEVEL%==0 goto msvc
where gcc >nul 2>nul
if %ERRORLEVEL%==0 goto mingw

:msvc
where cl >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    for %%V in (Community Professional Enterprise BuildTools) do (
        if exist "C:\Program Files\Microsoft Visual Studio\2022\%%V\VC\Auxiliary\Build\vcvars64.bat" (
            call "C:\Program Files\Microsoft Visual Studio\2022\%%V\VC\Auxiliary\Build\vcvars64.bat" >nul
            goto msvc_go
        )
    )
    echo ERROR: Visual Studio cl.exe not found. Run from Developer Command Prompt or pass "mingw".
    exit /b 1
)
:msvc_go
rc /nologo /fo traycoin.res res\traycoin.rc || exit /b 1
cl /nologo /utf-8 /O1 /W3 /D_CRT_SECURE_NO_WARNINGS /Isrc %SRC% traycoin.res /Fe:TrayCoin.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib oleaut32.lib comctl32.lib comdlg32.lib winhttp.lib || exit /b 1
del /q *.obj traycoin.res 2>nul
echo Built TrayCoin.exe (MSVC)
exit /b 0

:mingw
windres res\traycoin.rc -I res -O coff -o traycoin.res || exit /b 1
gcc -finput-charset=UTF-8 -fexec-charset=UTF-8 -Os -s -mwindows -Wall -Wextra -Isrc -ffunction-sections -fdata-sections -Wl,--gc-sections %SRC% traycoin.res -o TrayCoin.exe -luser32 -lgdi32 -lshell32 -ladvapi32 -lole32 -loleaut32 -lcomctl32 -lcomdlg32 -lwinhttp || exit /b 1
del /q traycoin.res 2>nul
echo Built TrayCoin.exe (MinGW)
exit /b 0
