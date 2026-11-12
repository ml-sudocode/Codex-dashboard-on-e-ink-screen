#!/bin/sh
set -eu
app=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic -g \
    -fsanitize=undefined -fno-sanitize-recover=all -I"$app" \
    "$app/board_feed.c" "$app/board_alert.c" "$app/tests/test_board.c" -o "$build/test_board"
"$build/test_board" "$@"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic -g \
    -fsanitize=undefined -fno-sanitize-recover=all -I"$app" \
    "$app/board_feed.c" "$app/board_stream.c" "$app/tests/test_stream.c" -o "$build/test_stream"
"$build/test_stream"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic -g \
    -fsanitize=undefined -fno-sanitize-recover=all -I"$app/tests/stubs" -I"$app" \
    "$app/board_feed.c" "$app/board_alert.c" "$app/board_stream.c" "$app/board_usb.c" \
    "$app/tests/test_runtime.c" -o "$build/test_runtime"
"$build/test_runtime"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic -g \
    -fsanitize=undefined -fno-sanitize-recover=all -I"$app/tests/stubs" -I"$app" \
    -I"$app/../../components/os/include" \
    "$app/board_feed.c" "$app/board_view.c" "$app/tests/test_view.c" -o "$build/test_view"
"$build/test_view"
