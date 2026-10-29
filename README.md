# opnmml - open-source player for the `.mus` MML music format

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
* **Recordings of the original** (PC-88 and X1 files): pitch matches to four digits, loop periods match, overall level 1.00 / 0.96.
* Sanitizer runs (AddressSanitizer/UBSan) and corrupted-input tests: no crashes.

## Known differences from the original

1. **Bugs of the original that are not copied.** A pitch bend on a note of length 0, a stray `]`, and a bend with no target
   note make the original plugin crash or read past the line; this port rejects such files with an error.
2. **Slots.** The engine keeps up to 64 songs per slot that share tones and envelopes, like the original (`Mixer::loadSong`, checked
   against the original DLL); `mmlplay` itself plays one file at a time.
3. Output is sample-exact only for a given render block size (`-b`, default 128, which is what the DLL's DirectSound path uses): the 1 ms
   sequencer stepping inside a block shifts note onsets by a few samples when the block size changes. It sounds the same.
4. **Recordings of the game.**  With all Windows sound enhancements switched off, a recording of the game agrees with this engine at every tested
   frequency: the level of 64 FM and PSG tones is identical to three decimals (DS mode on and off), the tuning is identical (0.0 ppm), and
   the PSG square waves are sample-for-sample identical in 9 or 10 of 13 tones (the others are one sample off at the edges).  The PSG noise is
   the same sequence as ours, starting at a different place.  A recording made with the enhancements ON (here: SoundAlive) shows a smooth
   tilt, about 0.92x below 300 Hz and 1.23x above 4 kHz; the same tilt appears on a plain WAV file played without the game, so it is the
   sound-card setting and not the game or the engine.  Record with enhancements off (see `calib/`).
5. **Chip features that are not there** (the original does not have them either, and the language cannot reach them): the second register port of
   the OPNA (YM2608: FM channels 4-6, ADPCM, rhythm sounds), and SSG-EG, an FM envelope mode of the OPN family (registers 0x90-0x9F; writes are
   remembered and ignored).  The PC-88 dialect has 3 FM + 3 PSG channels, the X1 dialect 8 FM + 3 PSG.
6. A few oddities of the original are kept on purpose (see comments): e.g. OPN register 0x27 (channel-3 mode) never
   takes effect, a trailing `//` comment on a channel line joins it to the next line, the amplitude LFO's delay is
   consumed by the first note.
