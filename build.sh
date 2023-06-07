emcc -O3 -s WASM=1 -sALLOW_MEMORY_GROWTH=1 -sEXPORTED_FUNCTIONS='["_acropalypse_recover", "_malloc", "_free"]' -sEXPORTED_RUNTIME_METHODS='["cwrap"]' -Wno-deprecated-non-prototype acropwasm.c zlib/inflate.c zlib/crc32.c zlib/adler32.c zlib/zutil.c zlib/compress.c zlib/inftrees.c zlib/deflate.c zlib/trees.c zlib/inffast.c -o acropalypse.js