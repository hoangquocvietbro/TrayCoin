#!/bin/sh
set -e
CC=${CC:-x86_64-w64-mingw32-gcc}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
SRC="src/main.c src/logger.c src/json.c src/http.c src/config.c src/providers.c src/poller.c src/ws.c src/alerts.c src/taskbar.c src/widget.c src/ui_settings.c"
"$WINDRES" res/traycoin.rc -I res -O coff -o traycoin.res
"$CC" -finput-charset=UTF-8 -fexec-charset=UTF-8 -Os -s -mwindows -Wall -Isrc -ffunction-sections -fdata-sections -Wl,--gc-sections $SRC traycoin.res -o TrayCoin.exe -luser32 -lgdi32 -lshell32 -ladvapi32 -lole32 -loleaut32 -lcomctl32 -lcomdlg32 -lwinhttp
rm -f traycoin.res
echo "Built TrayCoin.exe"
