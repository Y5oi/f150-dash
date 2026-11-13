#!/bin/sh
# Build the PC preview against a local LVGL 9.5 checkout and render all pages.
#   LVGL_DIR=/path/to/lvgl ./build.sh [out_dir]
set -e
cd "$(dirname "$0")"
: "${LVGL_DIR:?set LVGL_DIR to an lvgl v9.5.0 checkout}"
OUT="${1:-./out}"; mkdir -p "$OUT" build
if [ ! -f build/liblvgl.a ]; then
  find "$LVGL_DIR/src" -name '*.c' | xargs -P 8 -I{} sh -c 'cc -c -O1 -DLV_CONF_INCLUDE_SIMPLE -I. -I"$0" "{}" -o "build/$(echo {} | md5sum | cut -c1-12).o"' "$LVGL_DIR"
  ar rcs build/liblvgl.a build/*.o
fi
for f in ../main/fonts/*.c; do cc -c -O1 -DLV_LVGL_H_INCLUDE_SIMPLE -DLV_CONF_INCLUDE_SIMPLE -I. -I"$LVGL_DIR" "$f" -o "build/$(basename $f .c).fo"; done
c++ -std=c++17 -O1 -DLV_CONF_INCLUDE_SIMPLE -I. -I"$LVGL_DIR" preview.cpp ../main/dash_ui.cpp ../main/dash_core.cpp build/*.fo build/liblvgl.a -lm -o build/preview
./build/preview "$OUT"
