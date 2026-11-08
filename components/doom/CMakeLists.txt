# doomgeneric (github.com/ozkl/doomgeneric @ e257a09, GPL-2, see LICENSE): the Doom engine. Its platform layer is
# main/app_doom.c. Changes to the engine are marked "badge:".
file(GLOB srcs *.c)
idf_component_register(SRCS ${srcs} LDFRAGMENTS linker.lf)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -w)
