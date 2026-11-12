#!/bin/bash
# Builds the weather window emulator (one self-contained HTML page) with Emscripten.
#   EMCC=/path/to/emcc tests/emu/build.sh <out-dir>
# Needs the fonts/basemap/logo C file from cmake/embed_fonts.cmake (made here).
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-build-emu}
EMCC=${EMCC:-em++}
mkdir -p "$OUT"
cmake -DSRC=third_party/fonts -DOUT="$OUT/wx_fonts.c" -P cmake/embed_fonts.cmake > /dev/null
SRCS="tests/emu/emu.cpp src/wxui.cpp src/wxmodel.cpp src/metar.cpp src/hazards.cpp src/geo.cpp src/opera.cpp src/radar.cpp src/viewer.cpp src/png.cpp src/inflate.cpp src/sources.cpp src/windsaloft.cpp src/windshear.cpp src/turbulence.cpp src/config.cpp src/json_mini.cpp
      third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp"
EXIST=""
for f in $SRCS; do [ -f "$f" ] && EXIST="$EXIST $f"; done
"${EMCC%++}cc" -c -O2 "$OUT/wx_fonts.c" -o "$OUT/wx_fonts.o"   # the embedded data, as C (em++ would compile it as C++)
$EMCC -std=c++17 -O2 -Isrc -Ithird_party/imgui -DWX_EMU $EXIST "$OUT/wx_fonts.o" \
  -sUSE_WEBGL2=1 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sALLOW_MEMORY_GROWTH=1 -sERROR_ON_UNDEFINED_SYMBOLS=0 -sSINGLE_FILE=1 \
  -sEXPORTED_FUNCTIONS=_main,_emu_mouse,_emu_wheel,_emu_key,_emu_char,_emu_wants_text,_emu_scale,_emu_pause,_emu_teleport,_emu_set_alt,_emu_alt \
  -sENVIRONMENT=web -o "$OUT/emu.js"
python3 - "$OUT" <<'PY'
import sys
out = sys.argv[1]
shell = open('tests/emu/shell.html').read()
js = open(out + '/emu.js').read()
open(out + '/index.html', 'w').write(shell.replace('{{{ SCRIPT }}}', '<script>\n' + js.replace('</script', '<\\/script') + '\n</script>'))
print(out + '/index.html')
PY
