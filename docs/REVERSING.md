# How EchoVRMusic hooks Echo's audio

Everything here is for the current `echovr.exe` (35,397,120 bytes, May 2023), image base
`0x140000000`. Addresses are absolute; the plugin uses them as RVAs.

## 1. Wwise is linked in and exported

Echo uses **Wwise 2018** (`Powered by WWISE © 2006 - 2018`, bank version 134), statically linked.
`echovr.exe` exports **308 `AK::` functions** (for `OculusSpatializerWwise.dll`), so the plugin
finds them with `GetProcAddress(GetModuleHandle(NULL), "<mangled name>")`. No signatures are
needed for these.

| function | address |
| --- | --- |
| `AK::SoundEngine::PostEvent(AkUniqueID, ...)` | `0x14138f240` |
| `AK::SoundEngine::PostEvent(const char*, ...)` | `0x14138f480` (`GetIDFromString`, then an internal post, not the ID export) |
| `AK::SoundEngine::RegisterGameObj` (both overloads) | `0x141391b90` |
| `AK::SoundEngine::UnregisterGameObj` | `0x141395670` |
| `AK::SoundEngine::SetPosition` | `0x141393e20` |
| `AK::SoundEngine::SetMultiplePositions` (AkTransform / AkChannelEmitter) | `0x141393a60` / `0x141393a40` |
| `AK::SoundEngine::RenderAudio` | `0x141391fb0` |
| `AK::SoundEngine::Query::GetPosition` | `0x141396f10` |
| `AK::SoundEngine::Query::GetPlayingIDsFromGameObject` | `0x141396ee0` |
| `AK::SoundEngine::SetScalingFactor` | `0x1413942a0` |
| `AK::SoundEngine::ExecuteActionOnPlayingID` | `0x14138d160` |

Both `RegisterGameObj` overloads resolve to one function, so its `name` argument is garbage when
the one-argument form is called. The plugin only keeps it when it reads as a printable string.

## 2. Music banks and events

From the soundbanks extracted in `EchoVR-Audio-Editor/BNK` (`tools/find_music_events.py`):

| bank ID | file | content | music Play events |
| --- | --- | --- | --- |
| `0x98f91c32` | `12220d6560c7f5d3` | arena music, 74 tracks | `0xcbcdb503` |
| `0xb96c33b5` | `f602dc76162d8299` | lobby music, 61 tracks | `0x2f22c384`, `0xb1b06cfb` |
| `0x69b27427` | `8120754777bde147` | 66 tracks, 10 attenuations (positioned music) | 9 events |
| `0x2d28fc86` | `64a2a62b905b38b1` | music | `0x4edefef9` |
| `0xcc62f7a0` | `ad7fe1c8682708b9` | music and sounds | `0x0e3c34d1`, `0x8dcb9269` |

`12220d6560c7f5d3` is also the `mp_arena_music` script symbol
(`lone_echo_blender/docs/LAUNCHERS.md` §4.4).

Which game object each event is posted on, and whether that object has one position or several
(`SetMultiplePositions`), is not recorded in the data. The plugin learns it at run time, and
`LogEvents = 1` writes it to the log.

## 3. The Audio Input path (voice chat)

Bank `0x957359ff` (`ad7fe1c8732d0fbc`, no media) has 5 sounds whose source plugin is
`0x00C80002`, Wwise's **Audio Input** source (company 0, plugin 200). Wwise pulls PCM for
those from a callback. Play events:

| event | plays |
| --- | --- |
| `0xf5b8b08d` | sound `0x208bfab1` (default `VoiceEvent`) |
| `0xf2fc21cc` | switch container `0x3b1d9383` (holds Audio Input sounds) |

Echo wraps the callbacks in `NRadEngine::CAudioInputCallbackRegistry` (RTTI type descriptor
RVA `0x204b940`, vtable `0x1416ea350`):

- The singleton pointer is the global at **`0x1420A2FE0`**. The object has a mutex at `+0x38`,
  and three arrays sorted by playing ID: delegates (32 B entries) at `+0x40`, formats (20 B)
  at `+0x80`, and flags (8 B) at `+0xC0`.
- **vtable slot 6, `0x14020ca30`**:
  `Register(this, AkPlayingID id, const Delegate* d /*24 B*/, const Format* f /*16 B*/)`.
  It inserts the entry, then calls `0x1414667f0` =
  **`SetAudioInputCallbacks(Execute = 0x140205440, GetFormat = 0x140205540, GetGain = NULL)`**
  (that writes the globals `0x1420F50C0/C8/D0`).
- **vtable slot 7, `0x140211e00`**: `Unregister(this, AkPlayingID id)`.
- **Execute `0x140205440`** `(AkPlayingID, AkAudioBuffer* buf)`: it sets `buf->eState` (`+0x0C`)
  to `AK_NoDataReady` and `uValidFrames` (`+0x12`) to 0, then looks up the ID and calls
  `delegate.fn(&delegate, buf->pData, buf->MaxFrames /*+0x10*/)`. A return of `n > 0` gives
  `uValidFrames = n` and `AK_DataReady`. 0 gives `AK_NoDataReady`, and -1 gives `0xFFFF` frames.
- **GetFormat `0x140205540`** `(AkPlayingID, AkAudioFormat&)`: it takes `sampleRate` from
  format `+0`, `bitsPerSample` from `+4`, and `typeID` from `+8 & 3`. The channel mask is
  hardcoded to `0x4` (front centre), so the stream is **mono**.
- The delegate is `{void* a; void* b; int64 (*fn)(Delegate*, void* pcm, uint16 maxFrames)}`.

Echo's own voice chat registers it at `0x140d16747`: format `{rate, 16, 0}` (**int16 mono**),
and delegate `{object, context, 0x140c85b50}`.

The plugin does the same with its own delegate. All its Wwise calls run inside its
`RenderAudio` hook, so `PostEvent` and `Register` happen before the RenderAudio that starts the
voice, and `GetFormat` always finds the ID.

## 4. Level load and map music

`0x1404fe050` loads a level: `(this, CSymbol64 level, ...)`. It stores the symbol at
`this+0x58` (the same hook as `leveldetect.log`). The symbol is the engine's 64-bit name hash
(`lone_echo_blender/scripts/le_symbol_names.py`); for example, `mpl_arenacombat` =
`0xdb696852ed977bc0`.

Seen in game:

| level | music | plays on |
| --- | --- | --- |
| main menu | `0x8dcb9269` | `"Pooled Emitter 1024"` at the origin |
| lobby | 9 events, bank `0x69b27427` | 9 `"Pooled Emitter"`s, each with one position (the real speakers) |
| `mpl_arena_a` | `0x4edefef9` | `"2D Emitter"`, flat |
| Dyson (combat) | `0x2f22c384` | `"2D Emitter"`, flat |

Wwise ignores a 2D sound's position, so flat music has no speaker positions to copy.
`AkTransform` is laid out as `front, top, position` (Wwise 2017+).

## 5. Still unknown

- Which objects the stock music plays on in each map, and how many positions each has. Run with
  `LogEvents = 1` to find out.
- The attenuation radius of the voice sound. Voice attenuations in bank `0x957359ff` go out to
  about 40–70 m. `Scale` (`SetScalingFactor`) stretches it for speakers further away.
- Whether the voice bus is ducked or affected by the in-game voice-chat volume slider.
