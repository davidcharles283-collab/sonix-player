#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
temp=$(mktemp -d)
trap 'rm -rf "$temp"' EXIT
${CC:-cc} ${CFLAGS:-} -std=gnu11 -pthread -ffunction-sections -fdata-sections \
    -c src/system/db/sqlite3_impl.c -o "$temp/sqlite3.o"
# Link only the tested functions so the test doesn't need a network or UI.
${CC:-cc} ${CFLAGS:-} -std=gnu11 -Wall -Wextra -Werror -pthread -I. -I lvgl \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    tests/podcast-resume.c src/system/streaming/podcast.c src/system/streaming/podcastrss.c \
    src/system/streaming/podcaststate.c src/system/streaming/podcastdl.c src/system/core/utils.c \
    "$temp/sqlite3.o" -lm -o "$temp/test"
"$temp/test" "$temp"
