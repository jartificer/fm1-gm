# FM1-GM

Custom firmware that turns the M-VAVE FM-1 into a General MIDI sound module and 16-track sequencer.

- **128 General MIDI programs** (the full GM1 sound set, fixed, not editable) and a **GM drum kit** (notes 35–81) on channel 10.
- **16 tracks, each on any of the 16 MIDI channels**, with program, volume, pan, reverb, transpose and key velocity per track.
- **16 songs saved on the device**, power-safe (two copies per slot, CRC-checked).
- Real-time recording with overdub, quantize and count-in, plus step recording, undo, mute and solo.
- MIDI in (USB and the TRS jack) plays the sounds; MIDI out and clock out over USB are optional.

It is built on [Felucca](https://github.com/hugelton/Felucca)'s FM-1 platform, through [X0X](https://github.com/charlesvestal/fm1-x0x) and [FoMni](https://github.com/charlesvestal/fm1-fomni). The sound engine, the sequencer, the song storage and the screens are new.

> **Status: not yet tested on hardware.** Everything runs and is tested in the host simulator (`tests/run_tests.sh`), and CI builds the device package. Install it only if you are ready to use the recovery steps below.

## The sounds

The sounds are samples: a wavetable made from the Fluid R3 GM SoundFont by Frank Wen (MIT License). The full SoundFont is 141 MiB and the FM-1 has about 440 KiB of flash left after the firmware, so `tools/sf2_reduce.py` cuts it down to `assets/soundfont/fm1gm.sf2` (1.5 MiB, a normal SoundFont you can play in any SoundFont player): for each program one velocity layer and up to three mono samples spread over the keyboard (two for most), each at the lowest sample rate its spectrum needs and cut to its attack plus a short, crossfaded loop. The build stores the samples as 4-bit ADPCM, 381 KiB in all.

Each note plays its program's sample for that key with the SoundFont's own settings: volume envelope, low-pass filter and filter envelope, vibrato and tremolo, tuning and level. All 128 programs have their own samples, apart from the few that FluidR3 itself shares: the three acoustic pianos, Accordion and Tango Accordion, Strings and Slow Strings, Saw Lead and Bass & Lead.

The drum kit is FluidR3's standard kit, notes 35–81. Each drum keeps its sample until it has died away by 36 dB (at most a quarter second); cymbals and other long drums then loop and fade. Open and closed hi-hats choke each other, as do the other groups FluidR3 marks.

A shared reverb follows CC91 (channel and track reverb send).

GM controllers: note on/off, program change, pitch bend (with RPN 0 bend range), CC1 modulation, CC7 volume, CC10 pan, CC11 expression, CC64 sustain, CC91 reverb, CC120/121/123.

## Using it

The screen has five views. A view's button again flips its four knobs to their second page.

| Button | |
|---|---|
| HOME | **Song** view: piano roll of the selected track. Knobs: tempo, bars, quantize, click |
| EDIT | **Track** view. Knobs: family, program, volume, pan / channel, reverb, transpose, key velocity |
| FX | **Mix** view: 16 strips. Knobs: the volumes of the selected track's group of four (1–4, 5–8, …) |
| SEL | **Songs** view: the 16 slots. SEL again loads the highlighted slot |
| GLO | **Setup**. Knobs: MIDI in mode, MIDI out, clock out, count-in / voices, click volume, lights |
| PLAY | Play / stop |
| REC | Arm recording (overdub, with a one-bar count-in when it is on) |
| SEQ | Step recording on / off (stopped) |
| SAVE | Save to the current slot; in the Songs view, to the highlighted slot (a used slot asks twice) |
| ENV | Mute the selected track |
| LFO | Solo the selected track; in step recording, step back |
| ARP | Undo / redo the last recording or erase; hold 1 s to erase the track; in step recording, a rest |
| OCT- / OCT+ | Keyboard octave, −3..+3 |

Encoders: **SELECT** picks the track (or the slot in the Songs view), **ALGORITHM** the program of the selected track, **PRESETS** the tempo.

The keys play the selected track. On a drum track (channel 10) the lowest key is note 35 (Acoustic Bass Drum) and the keys go up the kit.

Loading a different song while the current one has changes asks for a second SEL.

**MIDI in** has two modes, set in Setup. TRACK (the default) plays whatever arrives on the selected track, with its sound, so a keyboard records into the track you are on. MULTI plays each channel as sent, as a GM module; channel 10 is drums. MIDI Start and Stop start and stop the sequencer.

### Recording

Press REC, then PLAY (or PLAY then REC). The click counts in a bar, then the loop plays and what you play is recorded into the selected track, quantized to the Quantize setting (OFF, 1/4 … 1/32). Recording loops and overdubs. ARP undoes the last pass.

Step recording: stop, press SEQ, then play a note (or a chord) to place it at the step and move on by the quantize value. ARP inserts a rest, LFO steps back, SEQ finishes.

## Defaults chosen where the hardware or the request was open

- **128 programs, not 127.** GM1 has 128 (0–127), so all of them are here.
- **Polyphony: 16 melodic + 8 drum voices**, GM1's minimum. Setup → Voices allows 8..24 melodic voices. If the render runs over 85 % of its time budget, the quietest voice is dropped rather than letting audio break up.
- **Song length:** 1–128 bars of 4/4, 48 PPQN, up to 1280 notes per song across all tracks, 30–300 BPM.
- **Key velocity:** the FM-1 keys are not velocity-sensitive, so each track has a fixed key velocity (default 100); MIDI in keeps its own velocity.
- **No chorus.** CC93 is accepted and ignored, to keep CPU for voices.
- **One velocity layer.** MIDI velocity sets the level only; the samples are FluidR3's velocity-100 layer.
- **No external clock sync.** MIDI Start and Stop are followed, the tempo is the song's own.
- **Notes only are recorded** (no controller or bend automation); per-track volume, pan and reverb are part of the song.
- **Flash map.** The 16 songs sit at 0x97000 + slot × 0x4000 (two copies of 8 KiB each), panel calibration at 0xDC000 and settings at 0xFC000. The stock firmware's patches at 0xEA000 and the update staging area at 0xE0000 are not touched.
- **Package identity FM-1_600** (release builds FM-1_6XXYYZZ), apart from FoMni (FM-1_8…) and X0X (FM-1_9…).

## Installing

Get `fm1gm.fwsc` from the CI artifacts (Actions → build) or build it yourself (see [BUILDING.md](BUILDING.md)). Then, with the FM-1 connected over USB:

```
pip install mido python-rtmidi
python3 tools/fm1_install.py fm1gm.fwsc
```

This works from the stock firmware or any Felucca-based firmware. To go back, install M-VAVE's stock package the same way, or with M-VAVE's updater.

Holding OCT- and OCT+ for 5 seconds puts FM1-GM into update mode, for an installer that cannot reach it otherwise.

### Recovery

After two failed starts in a row the firmware comes up in a safe mode with USB on and no audio, so an installer can reach it; PLAY tries the firmware again. After four, it enters the chip's own update mode, the last resort: `tools/fm1_rescue.py` writes the stock firmware back through it (see the script's header and [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter)).

## Developing

```
tests/run_tests.sh          # host tests and the simulator scenarios (needs cc and Python with Pillow)
sh host/build_host.sh       # the firmware app on your machine: build/host/gm_host SCRIPT OUTDIR
./build.sh                  # the device package (JieLi toolchain and AC79 SDK: BUILDING.md)
```

`build/host/gm_host` runs a scenario script (`tests/scenarios/*.gm`: key presses, knob turns, waits, expectations) against the whole app, and writes screenshots and audio.

## License

GPL-3.0-only, as Felucca, X0X and FoMni. See [LICENSING.md](LICENSING.md).
