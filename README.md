# opnmml - open-source compiler and player for a special MML format

A from-scratch C++ port of a game music plugin (an "OPN MML decoder"): it reads `.mus` MML text files
and renders them to audio, emulating the sound hardware of the PC-88 (YM2203 FM + PSG), Sharp X1 (YM2151 FM + PSG)
and MZ-2000 (PWM). It is written from a decompile of the original plugin, with its read-only data tables, and checked
against recordings of the original plugin. No code of the original host application and no Windows API is needed.

## Build and use

    make                      # in an MSYS2 MinGW64 shell; produces mmlplay, mmltrace, mmldump
    mmlplay --help            # the everyday options;   mmlplay -d --help   also lists the debug-mode options

    mmlplay song.mus song.wav                         # exactly one pass of the song (no looping, no fade-out), machine taken from the file
    mmlplay song.mus song.wav -t 120 -v 24            # 120 s (the song keeps looping), quieter
    mmlplay song.mus psg.wav -t 30 -d -m 0x7          # debug mode: mute channels A,B,C (FM on PC-88): hear only the PSG
    mmldump song.mus -l                               # what the compiler made of the file, and how long the song is
    mmltrace song.mus -t 3000                         # chip register writes of the first 3 seconds

Options are `-x value` (letter, space, value only: `-v 40`, not `-v volume=40`); long forms `--volume 40` and
`--volume=40` work too. Wrong or out-of-range values stop with an explanation and a non-zero exit status.

## How it works

    song.mus --compile()--> Song --Mixer::play()--> Sequencer --register writes--> Opx4 chip --> samples --> WAV
    (mmlcomp.cpp)                   (mixer.cpp)      (seq.cpp)                      (opx4.cpp)

| File | Role |
|---|---|
| `mmlcomp.cpp/.hpp` | MML compiler: text to FM tone records, volume envelopes and per-channel bytecode. **The header has the MML syntax reference.** |
| `seq.cpp/.hpp` | Sequencer: plays the bytecode (tempo, loops, LFOs, envelopes, bend...) and turns notes into chip register writes. Steps once per millisecond. |
| `opx4.cpp/.hpp` | The chip: 4-operator FM (OPN 3 ch or OPM 8 ch) plus a 3-voice PSG. State layout and every register are documented in the header. |
| `mixer.cpp/.hpp` | Top level: chip + 4 sequencer slots, 1 ms chunking, master volume, 16-bit clipping. |
| `chiptables.inc`, `tables.inc` | Lookup tables extracted from the original, each documented with its original address. |
| `songlen.cpp` | Song length: a silent dry run of the sequencer that finds the end of the first pass, the loop start and the loop length. |
| `mmlplay.cpp`, `mmltrace.cpp`, `mmldump.cpp`, `cli.hpp` | The command-line tools and their shared argument parser. |

Function names in comments such as `[sub_1001A6E0]` are the names of the original functions (the number is their address
in the original), so any function can be compared against the original code. The chip keeps the original's flat state layout (same dword numbers) on purpose; the names
(`OP_PHASE`, `CH_FREQUENCY`...) are listed in `opx4.hpp`.

## What has been verified

Everything below was compared with the ORIGINAL plugin DLL by running its own code in a CPU emulator (see
`verify/README.txt`), not only against recordings:

* **Chip**: identical samples to the DLL's chip in all six chip modes (random register writes), and the whole internal
  state identical after every command; both example songs identical for 6 s.
* **Whole engine** (compiler + sequencer + chip + mixer): both example songs identical for 15 s on their own machine.
* **Compiler**: 4,515 boundary-value programs and about 7,000 random programs give the same accept/reject verdict, error
  line and compiled output (bytecode, tone and envelope tables) as the DLL's compiler.
* **Random songs**: about 100 random songs using every bytecode command, on random devices (also combinations the game never
  uses), give identical audio.
* **Recordings of the game itself** (Windows sound enhancements switched off, see `calib/`): a test song of 64 FM and PSG tones has the same
  level to three decimals (DS mode on and off) and the same tuning (0.0 ppm); the PSG square waves are sample-for-sample identical in 9 or
  10 of 13 tones.  A whole song (`bgm05_fm3`, 98 s) runs at the same speed to 0.03 ppm, every frequency band has the ratio 1.000 and the waveforms
  correlate at 0.994 in 100 ms windows.  (The speed match also shows that the game really has the tempo rounding described below.)
* **The public release of a song** (`bgm01_x1`): same notes, timing and tuning; it differs only by post-production (louder, a gentle equaliser,
  no DC offset, a fade-out).  With a gain, an equaliser and a DC filter applied to our render the level and tone agree within 0.2 dB.
* Sanitizer runs (AddressSanitizer/UBSan) and corrupted-input tests: no crashes.

## Differences from the original

Things this port does differently ON PURPOSE:

1. **Bugs of the original that are not copied.**  A pitch bend on a note of length 0, a stray `]` and a bend with no target note make the original
   crash or read past the line; this port rejects such files with an error.  A tempo of 0 or below is ignored (the original divides by zero).  The
   original clears the wrong title when a song is loaded (the title of song 11 x N instead of N, which corrupts memory for N >= 6); the port clears
   the right one.  A song that fails to compile leaves the slot as it was (the original keeps half-compiled data).  An out-of-range slot is an error.
2. **Additions that the original does not have:** play a song once instead of looping it (`Sequencer::setLooping`), the measurement of a song's length
   (`songlen.cpp`), the one-pass output of `mmlplay` with a natural release tail, the debug mode with its options, and warnings when the device and the
   file do not fit (a PC-88 file on the X1 chip, music on channels that are silent on the MZ chip).  The master volume only accepts 0 to 512 (the
   original takes any number).
3. **Not ported:** everything that only talks to the host program (the script class, the sound decoder interface, DirectSound).
4. Output is sample-exact only for a given render block size (`-b`, default 128, which is what the DLL's DirectSound path uses): the 1 ms sequencer
   stepping inside a block shifts note onsets by a few samples when the block size changes.  It sounds the same.

## Kept on purpose (the same as the original, even where it looks odd)

Real `.mus` files were written against these behaviours, so they are copied exactly:

* **The tempo is rounded down:** songs run 0.3 to 2 % slower than the nominal tempo (9 % at `t8`; `t1` never advances), and every song starts after
  about 11 ms of silence.  (`MML_REFERENCE.md`, section 7.)
* **Language oddities:** a trailing `//` comment on a channel line joins it to the next line; the octave range is checked after every command; the delay
  of an amplitude LFO is used up by the first note; lowercase or indented channel lines and a last line without a newline are ignored silently; an
  unused song number plays the first song of the slot; in the X1 dialect a tone's DT2 value lands in the AMS byte (section 12 of the reference).
* **Chip oddities:** OPN register 0x27 (channel-3 mode) never takes effect; FM channels D-H of the X1 chip are cut at key-off like PSG voices; on the
  MZ chip all FM channels are silent.
* **Chip features that are not there:** the second register port of the OPNA (YM2608: FM channels 4-6, ADPCM, rhythm sounds) and SSG-EG, an FM envelope
  mode of the OPN family (registers 0x90-0x9F; writes are remembered and ignored).  The PC-88 dialect has 3 FM + 3 PSG channels, the X1 dialect 8 FM + 3 PSG.
* **Noise:** the PSG noise is a fixed-seed shift register that keeps stepping while the chip runs, and the table behind the X1 chip's noise and the
  random LFO is built with the Visual C++ `rand()` formula, identical to the original's table.  Where in the noise sequence a note starts depends on
  tiny timing differences, so noise is never sample-exact against a recording.

## Known limits of the comparison with recordings

A recording made with Windows sound enhancements ON (here: SoundAlive) shows a smooth tilt, about 0.92x below 300 Hz and 1.23x above 4 kHz; the same
tilt appears on a plain WAV file played without the game, so it is the sound-card setting and not the game or the engine.  Record with the
enhancements off (see `calib/README_CALIB.txt`).
