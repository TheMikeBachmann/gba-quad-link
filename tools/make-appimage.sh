#!/usr/bin/env bash
# Package the four-machine build as a single-file AppImage.
#
# What goes in: the binary, and the handful of libraries a random Linux box
# might not have or might have too old.
#
# What does not go in, ever: the GBA BIOS. The previous project bundled its
# BIOS and ROM, which made one file that ran anywhere and also made that file
# undistributable. This one contains nothing but code that can be published, and
# keeping it that way is worth more than the convenience. The binary looks for
# a BIOS beside the .AppImage, in ~/.local/share/gba-quad-link/, or wherever
# --bios points.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

out="$root/GBAQuadLink-x86_64.AppImage"
while [ $# -gt 0 ]; do
    case "$1" in
        --out) out="$2"; shift ;;
        *) echo "usage: $0 [--out PATH]" >&2; exit 2 ;;
    esac
    shift
done

echo "==> building"
cmake --build build --target gba-quad-link -j"$(nproc)" >/dev/null
bin="$root/build/gba-quad-link"
[ -x "$bin" ] || { echo "no binary at $bin" >&2; exit 1; }

appdir="$root/build/AppDir"
rm -rf "$appdir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/lib"

echo "==> assembling AppDir"
cp "$bin" "$appdir/usr/bin/gba-quad-link"
cp "$root/packaging/AppRun" "$appdir/AppRun"
cp "$root/packaging/gba-quad-link.desktop" "$appdir/gba-quad-link.desktop"
cp "$root/packaging/gba-quad-link.png" "$appdir/gba-quad-link.png"
chmod +x "$appdir/AppRun"

# Libraries a host may not have, or may have too old. Everything else — libc,
# libm, and whatever SDL dlopens for Wayland, X11, ALSA and PipeWire — comes
# from the host on purpose: those have to match the machine it runs on.
for soname in libSDL2-2.0.so.0 libstdc++.so.6 libgcc_s.so.1; do
    path="$(ldd "$bin" | awk -v s="$soname" '$1 == s { print $3 }')"
    if [ -n "$path" ] && [ -e "$path" ]; then
        cp -L "$path" "$appdir/usr/lib/$soname"
        echo "    bundled $soname"
    else
        echo "    WARNING: $soname not found, leaving it to the host" >&2
    fi
done

# Two downloads, both cached after the first run.
tool="$root/build/appimagetool-x86_64.AppImage"
if [ ! -x "$tool" ]; then
    echo "==> fetching appimagetool"
    curl -fsSL -o "$tool" \
        https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage
    chmod +x "$tool"
fi

# The runtime is the small ELF prepended to the squashfs that mounts it at
# launch. appimagetool's default one links libfuse2 dynamically, which SteamOS
# does not ship — the image builds fine and then refuses to start with
# "dlopen(): error loading libfuse.so.2". This one statically links libfuse3
# and needs nothing from the host.
runtime="$root/build/runtime-x86_64"
if [ ! -x "$runtime" ]; then
    echo "==> fetching static AppImage runtime"
    curl -fsSL -o "$runtime" \
        https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64
    chmod +x "$runtime"
fi

echo "==> packaging"
# --appimage-extract-and-run: appimagetool has the same libfuse2 problem it
# hands its output, so unpack it to a temp dir rather than mounting it.
ARCH=x86_64 "$tool" --appimage-extract-and-run \
    --runtime-file "$runtime" "$appdir" "$out" 2>&1 | sed 's/^/    /'

echo
echo "==> $out"
ls -lh "$out" | awk '{ print "    " $5, $9 }'
echo
echo "    Put a GBA BIOS dump beside it, or in ~/.local/share/gba-quad-link/."
echo "    Then:  $(basename "$out") --host <dolphin-machine>"
