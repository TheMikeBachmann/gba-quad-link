# gba-quad-link

Four Game Boy Advance screens in one window, each with its own controller —
playing separate games, linked to each other over an emulated link cable, or
linked over the network to [Dolphin](https://dolphin-emu.org/) running a
GameCube game on another machine. In any combination, at the same time.

The use case it was built for is *The Legend of Zelda: Four Swords Adventures*
— Dolphin on a PC, this on a machine wired to the TV, four people on the couch
with the GBA screens they are supposed to have. It turned out to be more
generally useful than that.

## What it does

|  |  |
|---|---|
| Four separate games | Each quadrant takes its own cartridge and keeps its own battery save. |
| Four on a link cable | Mario Kart, Kirby, anything that links. Grouped automatically by cartridge. |
| Four to a GameCube | Four Swords Adventures. All four multiboot from Dolphin over TCP. |
| One to a GameCube | Pac-Man Vs., the Tingle Tuner, Metroid Prime's Fusion link. |
| Any mix of the above | Two on a cable, one on the GameCube, one playing alone. |

Verified against real Dolphin and real games, not just in theory: Four Swords
Adventures multiboots all four quadrants and runs at 59.94fps on both sides;
Pac-Man Vs. multiboots one; four Mario Karts find each other over the cable and
race.

## Building

```sh
git clone --recurse-submodules git@github.com:TheMikeBachmann/gba-quad-link.git
cd gba-quad-link
cmake -S . -B build -G Ninja
cmake --build build
```

GCC or Clang, CMake 3.20+, Ninja, SDL2. The emulation core and the menu toolkit
are submodules; nothing else is needed.

## Running

```sh
./build/gba-quad-link --bios path/to/gba_bios.bin --rom-dir path/to/roms
```

Then press **F2** and choose cartridges. Everything else has a sensible default.

| Flag | |
|---|---|
| `--bios PATH` | a real GBA BIOS dump |
| `--rom-dir D` | where your cartridges live; `.gba`, `.zip` and `.7z` all work |
| `--host H` | the machine Dolphin is running on |
| `--players 1-4` | |
| `--player N ...` | opens a section: `--rom` and `--link` after it apply to that player alone |
| `--link cable\|dolphin` | |
| `--audio-player N` | which quadrant is heard (default 1) |
| `--fullscreen` `--scale N` `--integer-scale` | |
| `--verbose` `--log-sio` | mGBA's log, or only what it says about the serial port |

No BIOS and no ROMs are included, and none ever will be. A BIOS dump goes beside
the binary, in `~/.local/share/gba-quad-link/`, or wherever `--bios` points.

### Keys

| | |
|---|---|
| **F1** | controls — bind any button to a key or a pad, per player |
| **F2** | setup — cartridges, cable groups, Dolphin |
| **F3** | mirror one player's controls to all four |
| **Select+Start** | opens both menus from a pad, for Game Mode |

**F3** is for walking four machines through the same menus at once. Turn it off
before anyone has to choose a character.

### Without a Dolphin

`tools/mock_dolphin.py` stands in for Dolphin's SI ports — cycle slices, JOY
commands, and on request a stall with the sockets left open. Enough to develop
the link against; it cannot multiboot, so it can only ever prove our end
behaves.

## How it works

Each GBA runs on its own thread and owns its core outright. The host thread
never touches one: it writes keys, copies finished frames out under a lock, and
draws at its own rate. That separation is the whole design, because a guest can
block for a long time and the window has to stay alive and the other three have
to keep running.

**Nothing paces itself if something else is pacing it.** A machine on its own
is held to 59.7275fps. A machine on a GameCube runs exactly the cycles Dolphin
grants and is not paced here at all — Dolphin blocks the thread it emulates the
GameCube on until the guest answers, so a guest sleeping politely to look
punctual costs the host two thirds of its frame rate. On a cable only the parent
is paced, and mGBA's coordinator holds the rest in step with it.

**Cables are per group, not per room.** A cable carries one parent, at position
zero, and only the parent starts transfers — so one cable shared by everyone
means whichever pair does not own position zero never links. Machines are
grouped by cartridge, because you play with the people playing your game, with
an A/B/C/D override for the cases where that is wrong: the Mario Advance games
all link to play Mario Bros., Pokémon versions trade between themselves, and
single-pak multiplayer has one cartridge between four people.

**Four screens tile to 3:2.** A 16:9 display has about eleven percent spare at
each side, and it carries a status column per side — player, link state, frame
rate, pad — because what goes wrong here is mostly one of the four quietly not
being connected, and that is otherwise invisible. Below 96px there is no room
for a legible line, so they are dropped and the screens take the space.

## Known limitations

**Joining a live cable interrupts it.** Two people linked, a third loads the
same game, and both get a communication error. This is authentic: a console
powered on next to a live cable is on the bus immediately, the device count
changes, and the game is right to complain. Press Start to re-handshake. Loading
everyone's cartridge before anyone opens a link menu avoids it entirely.

It cannot be worked around by waiting until a guest "wants" the link, because
that is not observable — Mario Kart opens its serial port during boot and holds
it for the entire time it sits on the title screen. Measured: 1700 unbroken
frames of link mode with nobody playing.

**Controllers need Game Mode.** SDL hides Steam Input's virtual gamepads from
processes it does not believe are Steam games, and on SteamOS those virtual pads
are how a paired controller reaches an application at all. Add
`tools/gba-quad-link.sh` as a non-Steam game. Directly connected USB pads work
from a desktop terminal.

**EmuDeck may reset Dolphin's SI ports.** Its launcher deploys a config with
every port set to a standard controller, so a GBA (TCP) setting made through
EmuDeck's Dolphin may not survive the next launch. Starting Dolphin with
`flatpak run org.DolphinEmu.dolphin-emu` avoids it.

## Packaging

```sh
./tools/make-appimage.sh
```

Bundles the binary and the three libraries a host might have too old. Uses the
static `type2-runtime`, because appimagetool's default links libfuse2
dynamically and SteamOS does not ship it.

## Licence

MPL 2.0, the same licence as mGBA, so there is no compatibility question
anywhere in the tree. See [LICENSE](LICENSE).

Submodules keep their own: [mGBA](https://github.com/mgba-emu/mgba) is MPL 2.0,
[Dear ImGui](https://github.com/ocornut/imgui) is MIT.
