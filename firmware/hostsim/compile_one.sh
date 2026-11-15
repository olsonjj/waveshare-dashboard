#!/bin/bash
# usage: compile_one.sh <src.c> <out.o>
exec clang -c "$1" -O1 -DLV_CONF_INCLUDE_SIMPLE -Ihostsim -Ilib -Ilib/lvgl -Isrc \
    -Wno-unused-function -o "$2"
