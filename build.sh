#!/bin/sh
set -e
cd "$(dirname "$0")/src"
python3 - <<'PY'
src=open('interp.hlsl').read()
out=['#pragma once\nstatic const char g_hlsl[] =\n']
for line in src.split('\n'):
    out.append('    "'+line.replace('\\','\\\\').replace('"','\\"')+'\\n"\n')
out.append(';\n')
open('shader_src.h','w').write(''.join(out))
PY
CXX=${CXX:-x86_64-w64-mingw32-g++}
mkdir -p ../dist
$CXX -O2 -std=c++17 -shared -static -static-libgcc -static-libstdc++ \
    -Wall -Wno-unknown-pragmas -fno-exceptions -fno-rtti \
    -o ../dist/dinput8.dll okami60.cpp dinput8.def -ld3d11 -ldxguid -luuid
x86_64-w64-mingw32-strip ../dist/dinput8.dll 2>/dev/null || strip ../dist/dinput8.dll || true
echo built ../dist/dinput8.dll
