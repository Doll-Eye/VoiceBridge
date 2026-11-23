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

While serving, VoiceBridge creates a hidden window with the class and title that Tolk looks for. The window never takes focus. When Windows starts NVDA automatically for accessibility, for example after the lock screen, NVDA sees this window and closes again, just as it does when NVDA is already running. Starting NVDA yourself, for example with Ctrl+Alt+N, always takes over. Tools that run `nvda --check-running` will be told that NVDA is running while VoiceBridge serves.

If Windows is set to start NVDA automatically after sign-in, VoiceBridge gives it up to 120 seconds from sign-in to appear by default. If NVDA appears during that wait, VoiceBridge waits for it to quit before serving. Otherwise VoiceBridge starts serving when the wait ends. If Windows denies access to the sign-in setting, VoiceBridge gives NVDA the same time to start. If Windows cannot report when sign-in occurred, the wait starts when VoiceBridge starts.

## Status

- The controller front end serves NVDA's `NvdaController` and `NvdaController3` interfaces, steps aside for NVDA and comes back when NVDA quits. It passed silent tests on Windows 11 with GTA11Y Revo's 64-bit Tolk DLL, NV Access's controller client, PRISM 0.18.3, and 32-bit and 64-bit test clients. [Handover and sign-in tests](tests/front/run.ps1)
- The voice speaks through SAPI and WASAPI. It cuts the silence natural voices put at the start of every phrase (about 110 milliseconds) down to 15 milliseconds, stops at once when interrupted, and keeps recent phrases in memory. If the voice engine hangs or crashes, the supervisor starts a new server, which speaks each phrase that was still waiting once. It passed silent tests on Windows 11 with Jenny and the classic David voice, including forced hangs and crashes. [Voice tests](tests/voice/speaker-test.cpp)
- **Not yet tried:** an installed trial with real games.

`NvdaController2` (SSML speech and NVDA's process ID) is not served yet.

## What it works with

The stock NVDA controller DLL and PRISM v0.18.3 discover the bridge over RPC. Programs that use these directly can send it speech.

Games and mods using Tolk can speak through VoiceBridge when NV Access's controller client DLL is beside the game.

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
- **nvda:** `processNames` lists NVDA's program names, so that VoiceBridge can tell when NVDA is running. `signInWaitSeconds` sets the number of seconds from sign-in to give NVDA to start. The range is 0 to 600, the default is 120, and 0 disables the wait. `startsAtSignIn=auto` reads the Windows sign-in setting. Use `yes` or `no` to override it for tests. Invalid values use the defaults.

## Install

VoiceBridge runs as a scheduled task that starts 20 seconds after you sign in, in your own session, with ordinary rights. The sign-in wait counts from when you signed in.

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

`tests/front/run-on-puck.sh` builds the silent front tests and runs them on the Windows machine over SSH. The full run also tests supervisor recovery with natural Jenny, sending its audio to a null output. The runner waits for games to close, runs in session zero and removes the files it copied. The compatibility tests use the Tolk and NVDA controller client DLLs from GTA11Y Revo and PRISM 0.18.3 from the D4 Assist checkout.

## Licence

VoiceBridge is under the MIT licence; see [LICENSE](LICENSE). One file, `src/front/controller.idl`, comes from NVDA and stays under the GNU Lesser General Public License 2.1; see [THIRD-PARTY.md](THIRD-PARTY.md).

NVDA is by NV Access. NaturalVoiceSAPIAdapter is by gexgd0419. Tolk is by Davy Kager, and PRISM by Ethin Probst.
