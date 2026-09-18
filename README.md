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
| Whoever you want, audible | Any combination of machines heard at once, with a volume each. |
| Whoever is still playing | Hide the machines nobody is watching; the rest take the screen. |
| Four on a link cable | Mario Kart, Kirby, anything that links. Grouped automatically by cartridge. |
| Four to a GameCube | Four Swords Adventures. All four multiboot from Dolphin over TCP. |
| One to a GameCube | Pac-Man Vs., the Tingle Tuner, Metroid Prime's Fusion link. |
| Any mix of the above | Two on a cable, one on the GameCube, one playing alone. |
| Four Archipelago slots | Point a quadrant at a patch file; the randomiser client is run for you. |

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
./build/gba-quad-link --bios path/to/gba_bios.bin
```

Then press **F1** and pick cartridges. Everything is in that menu, including a
folder browser for finding your library, and it is all remembered — so after
the first run there is nothing to pass on the command line at all, which is
what makes the AppImage and a Steam shortcut work.

| Flag | |
|---|---|
| `--bios PATH` | a real GBA BIOS dump |
| `--rom-dir D` | where your cartridges live; `.gba`, `.zip` and `.7z` all work. Remembered. |
| `--host H` | the machine Dolphin is running on. Remembered. |
| `--players 1-4` | |
| `--player N ...` | opens a section: `--rom` and `--link` after it apply to that player alone |
| `--link cable\|dolphin` | |
| `--audio-player N` | hear only this quadrant. The menu can hear any combination. |
| `--fullscreen` `--scale N` `--integer-scale` | |
| `--verbose` `--log-sio` | mGBA's log, or only what it says about the serial port |

No BIOS and no ROMs are included, and none ever will be. A BIOS dump goes beside
the binary, in `~/.local/share/gba-quad-link/`, or wherever `--bios` points.

### Keys

| | |
|---|---|
| **F1**, **F2**, or **Select+Start** | the menu |
| **F3** | mirror one player's controls to all four |
| **Escape** | quit |

One menu, three tabs — **Games** for cartridges, cable groups and Dolphin,
**Audio** for who is heard and how loudly, **Controls** for bindings and which
pad drives which quadrant. Select+Start works from a pad because Game Mode has
no keyboard, and a player whose buttons are wrong needs a way in that does not
depend on the bindings being right.

**F3** is for walking four machines through the same menus at once. Turn it off
before anyone has to choose a character.

### Sound

One machine is heard by default, because four unrelated soundtracks layered
over each other is genuinely unpleasant and no amount of mixing quality fixes
that. It is a default rather than a rule: two people playing the same game is
not noise, and wanting your own machine louder than your neighbour's is an
ordinary thing to want. The Audio tab has a switch and a volume per player, and
an **only** button for one click back to one machine.

The mix is paced by the least-supplied machine, so nobody is padded with
silence to keep up with whoever is furthest ahead — that padding is audible as
chopping. A machine that has genuinely stopped is left out rather than allowed
to hold up the rest. The drop counter on that tab should read zero; anything
else means audio is being discarded.

### Without a Dolphin

`tools/mock_dolphin.py` stands in for Dolphin's SI ports — cycle slices, JOY
commands, and on request a stall with the sockets left open. Enough to develop
the link against; it cannot multiboot, so it can only ever prove our end
behaves.

## Archipelago

Four people in the same multiworld, on one screen, without anybody opening a
terminal. Point a quadrant at a patch file and everything else happens on its
own: the base cartridge is found, the randomiser client is started hidden, and
the patched game boots in that quadrant.

### What you have to do

Three things, once each.

**1. Install Archipelago.** The Archipelago tab offers to fetch it: **Download
Archipelago**, about 90MB, unpacked into
`~/.local/share/gba-quad-link/archipelago` and adopted as soon as it lands.

It is a button rather than something that happens on its own, because 90MB is
a lot to spend on somebody's connection because they opened a tab to see what
was on it, and because what arrives is a program that then gets run.

Once it is installed, the tab names the version and offers **Update to X** when
a newer release exists. An update keeps what is yours — `host.yaml` with each
world's cartridge path, community `.apworld` files, generated seeds and your
player configuration — and takes everything else from the new release. The new
copy is staged beside the old one and only swapped in once it has unpacked and
been migrated, so a failed or cancelled update leaves the working install
untouched. It is refused while a client is running, since that would replace
the copy the client is running from.

To use a copy you already have instead, put it anywhere below one of:

```
$GQL_AP_DIR
~/.local/share/gba-quad-link/archipelago
~/Archipelago
~/Applications/Archipelago
./Archipelago
```

It is found by looking for `ArchipelagoBizHawkClient`, including one level down
in `squashfs-root/opt/Archipelago`, so an unpacked AppImage works as it lands.
The Archipelago tab says which copy it found.

**2. Own the base cartridge.** Put it in your ROM library folder (set on the
Games tab). It is found by checksum, so the filename does not matter, and it may
be inside a `.zip` or `.7z`. Some worlds accept more than one acceptable dump —
Castlevania: Circle of the Moon names two — and any of them will do.

**3. Get a patch file.** Either download your slot's patch from the room page,
or generate a seed locally with `ArchipelagoGenerate`. Patches default to
`~/.local/share/gba-quad-link/patches`, and the picker can browse anywhere.

Patches can also be named up front, which is useful for a Steam shortcut that
always starts the same multiworld:

```sh
gba-quad-link --players 4 --ap-server localhost:38281 \
  --player 1 --patch seed_P1_Borf.apemerald \
  --player 2 --patch seed_P2_Vega.apemerald \
  --player 3 --patch seed_P3_Ridley.apmzm \
  --player 4 --patch seed_P4_Samus.apmzm
```

### What happens by itself

Once you choose a patch for a player, in order:

1. The patch's `archipelago.json` manifest is read — game, slot name, server
   address, and the checksum of the cartridge it expects.
2. Your library is searched for that checksum, looking inside archives, with
   cartridges whose names resemble the game tried first. Anything extracted is
   cached, so this is slow at most once.
3. The Archipelago world for that game is located and its cartridge path is
   written into `host.yaml`, which is the setting the client would otherwise
   open a file dialog to ask you for.
4. The client is started hidden, one per player, and told where to connect by
   writing to its standard input.
5. The patched cartridge it produces is booted in that quadrant, and that
   quadrant starts answering the connector socket the client is looking for.

A patch generated locally carries no server address; put one in the **Server**
box on the Archipelago tab and it is used for every slot. A patch downloaded
from a room carries its own, and the box is ignored.

Four slots at once is tested, not assumed: four clients, two games, all four
joining the same multiworld from one window.

While you are playing, each quadrant's status column shows its Archipelago
slot name and the last item that went out or came in — the menu is the one
place nobody is looking mid-game, and a client that has quietly stopped is
otherwise invisible.

Each player's client output appears on the tab, interleaved in the order things
actually happened, which is the only useful way to read four connections at
once. **Clear** drops a player out of Archipelago entirely and kills their
client.

### Games Archipelago does not officially support

These work the same way, because nothing here knows the name of any game. All
the game-specific knowledge — which addresses hold what, how a check is
detected — lives in the world, inside the Archipelago client process. This
program only implements the generic connector protocol that every GBA world
talks: read, write, guard, lock, and a handful of identifying requests.

So a community world is installed the way Archipelago installs any world — drop
its `.apworld` into `custom_worlds/` — and then it is just another patch file.
*Metroid: Zero Mission* was added this way and needed no changes here.

The one thing that can be missing is a memory region no world has needed yet.
This build provides `EWRAM`, `IWRAM`, `ROM`, `Save RAM` and `System Bus`; a
world asking for anything else — `VRAM`, `PALRAM`, `Combined WRAM` — gets a
named error rather than a silent stall, and the Archipelago tab shows which
domain was wanted. Adding one is a few lines in `region_for()` in
`src/gba_instance.cpp`.

A GBA world that does not use Archipelago's BizHawk connector at all is out of
reach, as is any game that is not a GBA game.

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

**The screen belongs to whoever is using it.** Four people start an evening and
three go to bed; the one still playing should not be left in a quarter of a
television because of who they started with. The arrangement follows how many
machines are on screen — one takes the window, two split it, three or four take
the grid — and which way to split two is measured rather than chosen, because
side by side wins on every widescreen and stacked wins on 4:3.

Hiding a machine does not stop it. Someone stepping out for ten minutes comes
back to their game where they left it, still linked and still in position on
the cable, exactly as they would to a handheld left on the sofa. Stopping it
would take it off the cable and renumber everybody still playing, which is a
strange thing to do to four people because one of them went to make tea.

**Four screens tile to 3:2**, so a 16:9 display has about eleven percent spare
at each side. That carries a status column per side — player, link state, frame
rate, pad — because what goes wrong here is mostly one of the four quietly not
being connected, and that is otherwise invisible. Below 96px there is no room
for a legible line, so they are dropped and the screens take the space.

**Nobody waits for the sound card.** Each machine fills a ring buffer from its
own thread and the host thread mixes what is switched on. A machine linked to a
GameCube is already being told when it may run; the sound card would be a
second master pulling the other way. A machine that has run ahead drops its
oldest audio rather than block, because it should be heard where it is now.

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

**An Archipelago client that hangs holds up the ones behind it.** Game clients
find their emulator by taking the first port that answers between 43055 and
43059, so those ports are handed out one at a time — with two open at once, the
second client's connection sits in a backlog nobody is listening to and times
out instead of trying the next port. A client that dies drops out of the queue
by itself. One that hangs says so after twenty seconds; **Clear** gets rid of it.

Which client lands on which quadrant does not matter: a client takes its
identity from the cartridge it finds, not from the patch it was started with, so
four clients across four quadrants come out right in any order.

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
