# VoiceBridge

**Work in progress.** The part that talks to games is finished and tested. The part that speaks is being written now.

VoiceBridge lets Windows games and mods that speak through the NVDA screen reader speak with one of Windows' natural voices instead, such as Microsoft Jenny, whenever NVDA is not running. It is for players who like Narrator's natural voices. Narrator cannot do this itself: it reads out another program's announcements only while that program is in front, and game mods usually speak from the background.

Built by a blind developer and an AI.

## How it works

- Games and mods reach NVDA through its controller interface, a local RPC endpoint. NV Access's `nvdaControllerClient` DLL, Tolk and PRISM all use it.
- While NVDA is not running, VoiceBridge answers on that endpoint and speaks the text itself through SAPI 5. The natural voices reach SAPI through [NaturalVoiceSAPIAdapter](https://github.com/gexgd0419/NaturalVoiceSAPIAdapter).
- VoiceBridge coordinates handover through NVDA’s desktop startup mutex. When the NVDA launcher requests it, the supervisor waits for the bridge’s RPC process to exit and then releases the mutex. VoiceBridge comes back after NVDA quits.
- No game or mod files are changed.
- The voice follows Narrator's chosen voice and speed, read from Narrator's settings, so you change them in Narrator. Narrator does not need to be running. If that voice cannot be reached through SAPI, VoiceBridge uses Jenny, then the default voice.

## Status

- **Done:** the controller front end. It serves NVDA's `NvdaController` and `NvdaController3` interfaces, steps aside for a real NVDA and comes back when NVDA quits. It has been tested silently on Windows 11 with NV Access's client DLL, PRISM 0.18.3, and 32-bit and 64-bit clients. The handover fixtures and performance measurements are described in the [front-end review](REVIEW-front.md).
- **Being written:** the voice. The aims are to start speaking within about 30 milliseconds, to interrupt at once, to cut the silence natural voices put at the start of every phrase (about 110 milliseconds), and to keep recent phrases in memory.
- **Not yet tried:** an installed trial with real games.

`NvdaController2` (SSML speech and NVDA's process ID) is not served yet.

## What it works with

The stock NVDA controller DLL and PRISM v0.18.3 discover the bridge over RPC. Programs that use these directly can send it speech.

Upstream Tolk also requires a window with NVDA’s exact class and title. Games using that version of Tolk need real NVDA for its NVDA back end. VoiceBridge uses a different window identity because the Windows sign-in launcher for NVDA exits when it sees that window. [Tolk’s detection code](https://github.com/dkager/tolk/blob/e5149f0cb6ef9b941673017e0e7b7c409e485fbe/src/ScreenReaderDriverNVDA.cpp#L54-L57) · [NVDA’s sign-in check](https://github.com/nvaccess/nvda/blob/b896c9c44c86206b212a2d02bfeca0b1b2e80944/source/nvda.pyw#L140-L175)

## Build

VoiceBridge is cross-compiled on a Mac with MinGW-w64 and WIDL. Homebrew's `mingw-w64` package has both.

```sh
brew install mingw-w64
```

```sh
./build.sh
```

This makes `build/VoiceBridge.exe`, a static 64-bit Windows program. The default build is silent: it writes what it would say to its log, which is what the tests use. Once the voice part is in, `SPEAKER=sapi ./build.sh` builds the speaking version. `./build.sh clean` empties the build folder.

## Settings

`VoiceBridge.ini` sits beside `VoiceBridge.exe`. Restart VoiceBridge after changing it.

- **voice:** `name` is part of a SAPI voice's name (Jenny by default), and `fallback` is the voice to use if that one is missing. `rate` runs from -10 to 10 and `volume` from 0 to 100. With `followNarrator=1`, Narrator's voice and speed are used wherever they can be read.
- **audio:** `keepAwakeSeconds` is how long the sound stream stays open after speech, so that an idle VoiceBridge does not keep the computer awake.
- **cache:** `megabytes` is the size of the phrase cache; 0 turns it off.
- **log:** `level` is error, info or debug. Spoken text is never written to the log unless `text=1`.
- **nvda:** `processNames` lists NVDA's program names, so that VoiceBridge can tell when NVDA is running.

## Install

VoiceBridge runs as a scheduled task that starts 20 seconds after you sign in, in your own session, with ordinary rights. The delay lets an NVDA that starts at sign-in go first.

The scripts in `tools/` install it from a Mac over ssh, into `%LOCALAPPDATA%\VoiceBridge`. They were written for one Windows handheld, reached over ssh as `puck`.

To install it and start the task:

```sh
./tools/install-on-puck.sh
```

To see the task's state and its latest messages:

```sh
./tools/install-on-puck.sh --check
```

To stop VoiceBridge and remove the task and its files:

```sh
./tools/install-on-puck.sh --uninstall
```

On the Windows machine itself, `VoiceBridge.exe --stop` stops a running VoiceBridge.

## Tests

`tests/front/run-on-puck.sh` builds the silent version and runs the front end's tests on the Windows machine over ssh. The tests make no sound and show nothing, and the script removes everything it copied when it ends. For now it takes NV Access's controller client DLL, PRISM 0.18.3 and a small ssh helper from folders next to this one.

## Licence

VoiceBridge is under the MIT licence; see [LICENSE](LICENSE). One file, `src/front/controller.idl`, comes from NVDA and stays under the GNU Lesser General Public License 2.1; see [THIRD-PARTY.md](THIRD-PARTY.md).

NVDA is by NV Access. NaturalVoiceSAPIAdapter is by gexgd0419. Tolk is by Davy Kager, and PRISM by Ethin Probst.
