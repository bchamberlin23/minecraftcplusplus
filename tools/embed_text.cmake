# cmake -DIN=<file> -DOUT=<header> -DNAME=<symbol> -P embed_text.cmake
# Writes a C++ header with the file's bytes as a NUL-terminated char array (shader sources compiled at run time).
file(READ ${IN} HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BYTES "${HEX}")
file(WRITE ${OUT} "// Generated from ${IN} by tools/embed_text.cmake\n#pragma once\nstatic const char ${NAME}[] = {${BYTES}0};\n")
