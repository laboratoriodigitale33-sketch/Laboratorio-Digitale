#!/bin/bash
# compile.sh - Compilazione diffrazione.c -> diffrazione.wasm + diffrazione.js
# Richiede: emcc (Emscripten) nel PATH
#
# Uso:
#   chmod +x compile.sh
#   ./compile.sh
#
# Output: diffrazione.js e diffrazione.wasm nella cartella corrente

set -e

EXPORTED_FUNCTIONS='[
  "_build_double_slit",
  "_build_grating",
  "_build_circular_obstacle",
  "_propagate_asm",
  "_compute_source_observables",
  "_get_intensity",
  "_get_phase",
  "_get_amplitude",
  "_get_field_re",
  "_get_field_im",
  "_get_N",
  "_malloc",
  "_free"
]'

emcc diffrazione.c \
  -O3 \
  -std=c99 \
  -msimd128 \
  -s WASM=1 \
  -s MODULARIZE=1 \
  -s EXPORT_NAME="DiffractionModule" \
  -s EXPORTED_FUNCTIONS="$(echo $EXPORTED_FUNCTIONS | tr -d ' \n')" \
  -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","HEAPF32","getValue"]' \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s INITIAL_MEMORY=67108864 \
  -s ENVIRONMENT='web' \
  -s NO_EXIT_RUNTIME=1 \
  -lm \
  -o diffrazione.js

echo ""
echo "Compilazione completata:"
echo "  diffrazione.js   ($(du -sh diffrazione.js | cut -f1))"
echo "  diffrazione.wasm ($(du -sh diffrazione.wasm | cut -f1))"
echo ""
echo "Metti diffrazione.js, diffrazione.wasm e diffrazione.html"
echo "nella stessa cartella e apri con un server HTTP locale:"
echo "  python3 -m http.server 8080"
