#!/usr/bin/env bash
# Launcher for "Add a Non-Steam Game".
#
# Steam decides the working directory and the asset paths here are relative, so
# the first job is to put the shell back in the repo. Point Steam at this
# script rather than at build/gba-quad-link and the shortcut keeps working
# wherever it was created from.
#
# Running under Steam is not just convenience. SDL hides Steam Input's virtual
# gamepads (Valve 28de:11ff) from processes it does not believe are Steam
# games, and those virtual pads are how a paired controller reaches an
# application at all. Launched from Steam they show up and can take a quadrant;
# launched from a desktop terminal, only plain USB pads do. Every controller
# question has to be answered in Game Mode or the answer is not worth having.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

bin="$root/build/gba-quad-link"
if [ ! -x "$bin" ]; then
    echo "gba-quad-link: $bin is missing — build it with" >&2
    echo "  cmake -S . -B build -G Ninja && cmake --build build" >&2
    exit 1
fi

# Fullscreen with four machines by default: Game Mode has no window manager to
# resize against and no keyboard to press anything with. Arguments are
# appended, so a shortcut can still override — in particular --host, which is
# the machine Dolphin is running on. Set GQL_HOST and it is passed for you.
if [ -n "${GQL_HOST:-}" ]; then
    exec "$bin" --players 4 --fullscreen --host "$GQL_HOST" "$@"
fi
exec "$bin" --players 4 --fullscreen "$@"
