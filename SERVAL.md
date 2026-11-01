# serval-emu: mGBA for Serval Engine's debugger

This is a fork of [mGBA](https://github.com/mgba-emu/mgba) (Mozilla Public
License 2.0), on its `serval` branch, based on mGBA's master. It adds
`serval-emu`, a small SDL2 frontend that runs one GBA game and is the
emulator end of [Serval Engine](https://github.com/StevenGann/Serval-Engine)'s
debug link: protocol 1.0 and mailbox v1, specified in Serval Engine's
[docs/debug-link.md](https://github.com/StevenGann/Serval-Engine/blob/main/docs/debug-link.md).
Studio Advance, the editor for Serval Engine games, starts it to run and
debug a game; anyone can use it with Serval Engine's reference client,
`tools/svlink.py`.

Everything else is mGBA as it is upstream. mGBA's own frontends, its GDB
stub and its debuggers are unchanged.

## What it adds

| File | What |
| --- | --- |
| `include/mgba/internal/gba/serval-link.h` | New. The two registers Serval Engine's agent finds the endpoint by (presence at 0x04FFF800, the doorbell at 0x04FFF804), and the hook a frontend installs |
| `src/gba/io.c` | **Modified.** Reads and writes of those two registers go to the hook when one is installed; without one, nothing changes (open bus, stub writes) |
| `include/mgba/internal/gba/gba.h`, `src/gba/gba.c` | **Modified.** `struct GBA` holds the hook (`servalLink`), `NULL` at start |
| `src/platform/serval/link.c`, `link.h` | New. The endpoint: the TCP connection it dials to the debugger, the handshake, the endpoint's own messages (`READ_MEM`, `WRITE_MEM`, `MACHINE_PAUSE`, `MACHINE_RESUME`, `LOG`, `GAME_RESET`, `AGENT_FOUND`), and the agent's mailbox in EWRAM: the agent's frames carried both ways unread, the machine stopped right after the doorbell's write while the agent is parked, and run again only to deliver a request |
| `src/platform/serval/main.c` | New. `serval-emu`: the SDL2 window, sound, keyboard and gamepads, frame pacing at the GBA's 59.73 frames a second, mGBA's log printed and forwarded as `LOG` |
| `src/platform/serval/CMakeLists.txt`, `CMakeLists.txt` | New, and **modified**: the `BUILD_SERVAL` option (off by default) |
| `.github/workflows/serval-emu.yml` | New. Builds for Linux, Windows and macOS (universal), runs Serval Engine's debug link tests against the build, and releases on a tag `serval-emu-vX.Y.Z` |

As the MPL requires, the source of every file above, modified or new, is
published in this repository, under the same license.

## Using it

```sh
serval-emu [options] game.gba

  --link HOST:PORT     dial the debugger (Studio Advance) at HOST:PORT
  --link-token TOKEN   the debug session's token, sent in HELLO
  --scale N            the window's size, N times the GBA's screen (default 3)
  --headless           no window and no sound (for tests)
  --unpaced            run as fast as possible, not at the GBA's speed
  --frames N           quit after N frames
  --keys K             scripted buttons, FRAME:KEYS:LENGTH,... (A B s S R L U D r l)
  --version            print the version and quit
```

Keys: the arrows, X = A, Z = B, A = L, S = R, Enter = Start, Backspace =
Select; a gamepad's buttons in the GBA's places.

Without `--link` it is a plain GBA emulator for one game: the agent in a debug
build of a Serval Engine game still finds the endpoint, and stays idle with
no debugger. The game's save is kept beside it, as mGBA keeps it. mGBA's own
settings are not read, and no BIOS is loaded: mGBA's built-in replacement
runs the BIOS calls.

To debug from the command line with Serval Engine's reference client:

```sh
SERVAL_EMU=path/to/serval-emu tools/svlink.py build/gba-debug/examples/fireflies.gba \
    hello pause "globals 0 8" entities "break fireflies.lua:122" continue stack
```

## Building

`serval-emu` needs CMake, a C compiler and SDL 2 (2.0.18 or later):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_SERVAL=ON -DBUILD_QT=OFF -DBUILD_SDL=OFF -DM_CORE_GB=OFF \
  -DUSE_FFMPEG=OFF -DUSE_ELF=OFF -DUSE_EDITLINE=OFF -DUSE_DISCORD_RPC=OFF \
  -DUSE_LUA=OFF -DENABLE_SCRIPTING=OFF -DUSE_FREETYPE=OFF -DUSE_JSON_C=OFF \
  -DUSE_LIBZIP=OFF -DUSE_MINIZIP=OFF -DUSE_LZMA=OFF -DUSE_SQLITE3=OFF -DUSE_EPOXY=OFF
cmake --build build --target serval-emu
```

The released builds turn off everything the debugger doesn't use, and every
option that would bring in LGPL or GPL code (FFmpeg, libelf, libedit), so
Studio Advance can ship `serval-emu` beside it. They link SDL 2 statically and
use mGBA's vendored zlib and libpng (`.github/workflows/serval-emu.yml` has
the exact options). `src/platform/serval/NOTICES.md` lists what a build
contains.

## Releasing

Set `SERVAL_EMU_VERSION` in `src/platform/serval/main.c`, then push the tag
`serval-emu-vX.Y.Z`. The workflow checks that they agree, builds the three
packages (`serval-emu-X.Y.Z-<platform>.zip`: the program, the MPL, SDL's
license, the notices, this file and the source's address) and attaches them
with their SHA-256 sums to a draft release.
