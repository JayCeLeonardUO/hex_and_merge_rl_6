#!/bin/bash
cd "$(dirname "$0")"

# System cmake (3.22) is too old for this project; use the pipx one
CMAKE="${CMAKE:-$HOME/.local/bin/cmake}"
[[ -x "$CMAKE" ]] || CMAKE=cmake

TARGET="raylib-game-template"
DEBUG_MODE="${1:-}"

# Force a relink even when nothing changed, so the PRE_BUILD card-export
# sync and POST_BUILD resource copy always run
rm -f "build/$TARGET/$TARGET"

TMUX_DEBUG_SESSION="hex-gdbgui-debug"
TMUX_RUN_SESSION="hex-game-run"

if [[ "$DEBUG_MODE" == "gdb" ]]; then
    "$CMAKE" -B build -DCMAKE_BUILD_TYPE=Debug && "$CMAKE" --build build || exit 1
    gdb ./build/"$TARGET"/"$TARGET"
elif [[ "$DEBUG_MODE" == "gdbgui" ]]; then
    "$CMAKE" -B build -DCMAKE_BUILD_TYPE=Debug && "$CMAKE" --build build || exit 1

    for pid in $(pgrep -f "python.*gdbgui" 2>/dev/null); do
        kill "$pid" 2>/dev/null || true
    done
    sleep 0.3

    tmux kill-session -t "$TMUX_DEBUG_SESSION" 2>/dev/null || true
    sleep 0.2
    tmux new-session -d -s "$TMUX_DEBUG_SESSION" -c "build/$TARGET" "gdbgui ./$TARGET"

    echo "gdbgui started in tmux session '$TMUX_DEBUG_SESSION'"
    echo "Open http://127.0.0.1:5000 in your browser"
    exit 0
else
    "$CMAKE" -B build && "$CMAKE" --build build || exit 1

    tmux kill-session -t "$TMUX_RUN_SESSION" 2>/dev/null || true
    sleep 0.2
    # cwd must be the binary dir so the game finds resources/
    tmux new-session -d -s "$TMUX_RUN_SESSION" -c "$PWD/build/$TARGET" "./$TARGET"

    echo "Running in tmux session '$TMUX_RUN_SESSION'"
    exit 0
fi
