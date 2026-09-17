# gba-quad-link

Four Game Boy Advance screens in one window, each with its own controller, all
four linked over TCP to [Dolphin](https://dolphin-emu.org/) running a GameCube
game on another machine.

The use case it is built for is *The Legend of Zelda: Four Swords Adventures* —
Dolphin on a PC, this on a Steam Machine wired to the TV, four people on the
couch with the GBA screens they are supposed to have.

> **Status: milestone one.** A single libmgba core runs in a window with
> controller input and audio. The Dolphin socket client and the four-way
> compositor are not written yet. See [Milestones](#milestones).

## Building

```sh
git clone --recurse-submodules git@github.com:TheMikeBachmann/gba-quad-link.git
cd gba-quad-link
cmake -S . -B build -G Ninja
cmake --build build
```

Needs GCC or Clang, CMake 3.20+, Ninja and SDL2. Everything else — the
emulation core and the menu toolkit — is a submodule.

If you cloned without `--recurse-submodules`:

```sh
git submodule update --init --depth 1
```

## Running

```sh
./build/gba-quad-link --rom path/to/game.gba --bios path/to/gba_bios.bin
```

| Flag | |
|---|---|
| `--rom PATH` | the ROM to boot |
| `--no-rom` | boot the BIOS with an empty cartridge slot (see below) |
| `--bios PATH` | a real GBA BIOS dump |
| `--scale N` | initial window scale |
| `--fullscreen` | |
| `--integer-scale` | whole-pixel scaling; crisper, but leaves a border |
| `--verbose` | mGBA's full log rather than warnings and errors |

No ROM or BIOS is included and none ever will be. Put your own in `roms/` and
`bios/`; both directories are ignored by git.

### Testing controllers

**Controller changes have to be tested in Game Mode**, through
`tools/gba-quad-link.sh` added to Steam as a non-Steam game. SDL hides Steam
Input's virtual gamepads from processes it does not believe are Steam games,
and on SteamOS those virtual pads are how a paired controller reaches an
application at all. From a desktop terminal you only ever see plain USB pads,
so a working desktop test proves nothing about the thing people will use.

## How it fits together

Each GBA runs on its own thread and owns its core outright; the host thread
never touches one. Finished frames are latched into a small locked buffer that
the host thread copies out and draws at its own rate.

That indirection is the whole design. Dolphin is the timing master — it hands
out cycle slices over the clock socket and a core runs exactly what it is
granted, then blocks waiting for more, inside mGBA's SIO driver, for up to half
a second at a time. The window has to keep drawing through that, and the other
three cores have to keep running.

### The protocol

Dolphin listens; the GBA clients dial out. Two TCP sockets per GBA — data on
54970, clock on 49420 — and Dolphin accepts up to four connections on the same
pair, assigning them to SI slots **in connection order**. In Dolphin, set each
SI port's device to "GBA (TCP)".

libmgba already implements the GBA end of this, in
`src/gba/sio/dolphin.c`, so this project wires that driver up rather than
reimplementing it.

### Four Swords Adventures needs a real BIOS

FSA does not run from a GBA cartridge. It downloads a program to each GBA over
the JOY bus at the start of play, and that handshake lives in the official GBA
boot ROM. mGBA's HLE BIOS is a small set of SWI stubs with none of it. So the
configuration for actual play is a real BIOS dump and **no** ROM — `--no-rom`
— which leaves each core sitting where a real GBA with an empty slot sits,
waiting to be handed a program.

## Milestones

- [x] **One core.** A libmgba core in an SDL window, booting a commercial ROM,
      with controller input and clean audio.
- [ ] **The link.** The Dolphin socket client, clock-driven, against a real
      Dolphin instance. One core first.
- [ ] **Four.** The 2×2 compositor, per-player controller assignment and the
      controls menu.
- [ ] **Packaging.** AppImage with the static runtime, and a Steam shortcut.

## Notes for anyone reading the code

Three things that look like they should have been copied from a working
four-GBA app and could not be:

- **Geometry.** Four 240×160 screens tile to 480×320, which is 3:2 and
  pillarboxes on a 16:9 TV. The plan is to spend the side gutters on per-player
  link status rather than black them out.
- **Audio rate.** There is no constant to hardcode. A GBA's output rate is
  whatever the running program's `SOUNDBIAS` resolution field says — 32768 Hz
  from reset, doubling per step to 262144 Hz, changeable by a register write at
  any moment. The sound card gets a fixed 48000 Hz and mGBA's resampler absorbs
  the difference.
- **Pacing.** A standalone core paces itself to the console refresh. Under
  Dolphin that inverts and the clock socket drives, so the pacing is a fallback
  for unlinked cores rather than the normal path.

## Licence

MPL 2.0, the same licence as mGBA, so there is no compatibility question
anywhere in the tree. See [LICENSE](LICENSE).

Submodules keep their own: [mGBA](https://github.com/mgba-emu/mgba) is MPL 2.0,
[Dear ImGui](https://github.com/ocornut/imgui) is MIT.
