# EchoVRMusic

Plays whatever your PC is playing (Spotify, YouTube, anything) **out of Echo VR's own in-map
speakers**, in place of the stock music. It runs inside the game, so your music goes through
Echo's sound engine and comes from speakers placed around each map.

No virtual cable is needed. Play music on Windows as normal.

## Install

Run **`EchoVRMusicSetup.exe`** with Echo VR closed and press **Install**. It:

- finds Echo VR (or lets you pick its `bin\win10` folder),
- creates `bin\win10\plugins\` if it's missing,
- sets up the plugin loader `dbgcore.dll`:

  | `dbgcore.dll` in `bin\win10` | what happens |
  | --- | --- |
  | none | the loader is downloaded (from the latest [EchoXR Hands](https://github.com/heisthecat31/EchoXR-Hands/releases) release) and installed |
  | already a plugin loader | kept |
  | anything else | moved to `plugins\dbgcoreoriginal.dll` (it still loads from there), then the loader is installed |

- writes `plugins\EchoVRMusic.dll`, which is carried inside the setup exe.

The app's **Uninstall** removes `EchoVRMusic.dll` and leaves the loader, because other plugins
use it.

## Settings

Change them on the app's **Settings** tab while you play: the plugin picks up each change within
half a second. They're stored in `HKCU\Software\EchoVRMusic`, so there's no settings file.

| setting | default | what it does |
| --- | --- | --- |
| Music on the speakers (`Enabled`) | on | also **Ctrl+Alt+M** in game |
| Volume (`Gain`) | 100% | |
| Silent beyond (`Cutoff`) | 30 m | a speaker can't be heard from further away than this |
| Full volume within (`FullVolume`) | 3 m | |
| Nearest speakers only (`Nearest`) | all | only the N closest speakers play |
| Music comes from (`Source`) | all of Windows | `system`, or `process:<exe>` for one app |
| Pause Echo's own music (`MuteStock`) | on | |
| Distance fade (`Shaping`) | on | the cut-off and full-volume settings above |
| Stereo | on | speakers take turns playing left and right |
| Follow me on other maps (`Follow2D`) | on | maps without speakers play beside your head |
| Echo's fade range (`Scale`) | 1.00x | stretches Echo's own (gentle) distance fade |
| Buffer (`LatencyMs`) | 120 ms | raise it if the music crackles |

**Why distance fade:** Echo's voice sound, which the plugin plays through, fades very gently: it's
still audible 40–50 m away. With many speakers that blends into one "everywhere" sound. The
plugin works out your distance to every speaker each frame and fades each one itself: full
volume within *Full volume within*, silent at *Silent beyond*.

## Speakers

Built into the DLL (`src/builtin_data.h`):

| map | level | speakers |
| --- | --- | --- |
| Arena | `mpl_arena_a` (music `0x4edefef9`) | 18: Echo Speaker System's layout, moved 7.5 m in |
| Dyson | `mpl_combat_dyson` | 17 |
| Surge | `mpl_combat_gauss` | 26 |
| Combustion | `mpl_combat_combustion` | 19 |
| Fission | `mpl_combat_fission` | 24 |

The lobby uses its own 9 music speakers. Other maps play the music beside your head.

The combat maps' speaker files are in `maps\` (Echo coordinates, metres: x, y up, z). Their war
room speakers sit around (−200, −9, −200) and (150, −9, 200), the two pre-game rooms.

### Adding speakers to a map (for new builds)

1. Import the map with lone_echo_blender (1:1, Y-up → Z-up on).
2. Install `blender\echovrmusic_speakers.py` as an add-on.
3. In the 3D view sidebar (N), open **EchoVRMusic** and set **Level**, e.g. `mpl_combat_dyson`.
4. Put the 3D cursor where a speaker goes and press **Add Speaker**.
5. **Export** writes this project's `maps\<level>.txt`. **Import** loads it back.
6. Run `python tools\gen_builtin.py`, then `setup\build_setup.bat`: the speakers are built into
   the DLL.

## How it works

1. Echo's sound engine (Wwise 2018) is built into `echovr.exe` and its API is exported. The
   plugin hooks `PostEvent`, the position calls, `RenderAudio`, and Echo's level load.
2. When the game starts its music, the plugin pauses it and puts one game object per speaker
   at the speaker positions for the current map.
3. On each object it plays Echo's voice-chat sound, which uses Wwise's Audio Input source, and
   registers the stream with Echo's `CAudioInputCallbackRegistry`, as voice chat does. Wwise
   pulls PCM from the plugin, which captures Windows audio with WASAPI process loopback
   (everything except Echo, so there's no feedback). All speakers read one shared playhead, so
   they stay in sync.

The addresses and structures are in [docs/REVERSING.md](docs/REVERSING.md). The log is
`plugins\EchoVRMusic.log`.

## Limits

- **One game build:** the current `echovr.exe` (35,397,120 bytes, May 2023). The plugin checks
  the bytes it relies on and does nothing on any other build.
- **Hearing the music twice?** Windows still plays it normally too. Send the music app to an
  output you're not listening to (Windows Settings → System → Sound → App volume and device
  preferences). It's still captured, because the capture follows the app, not the device.
  Muting the app in the Volume Mixer does **not** work: that silences the capture as well.
- Audio Input streams are mono. Stereo comes from speakers alternating left and right.

## Build

| command | builds |
| --- | --- |
| `build.bat` | `out\EchoVRMusic.dll` |
| `setup\build_setup.bat` | the DLL, then `out\EchoVRMusicSetup.exe` with the DLL inside |
| `python tools\gen_builtin.py` | `src\builtin_data.h` from `maps\*.txt` and the arena layout |
| `python tools\gen_icon.py` | `setup\echovrmusic.ico` |
| `python tools\find_music_events.py` | lists music and Audio Input events in extracted soundbanks |
