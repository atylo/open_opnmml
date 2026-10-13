# opnmml - open-source player for the wuopn.dll `.mus` music format

A from-scratch C++ port of the Kirikiri 2 plugin `wuopn.dll` (an "OPN MML decoder"): it reads `.mus` MML text files in
non-standard or well-known syntax and renders them to audio, emulating the sound hardware of the PC-88 (YM2203 FM + PSG), Sharp X1 (YM2151 FM + PSG)
and MZ-2000 (PWM). It is written from the Hex-Rays decompile of the DLL, with its `.rdata` tables, and checked
against recordings of the original plugin. No Kirikiri code or Windows API is needed.

## Build and use

    make                      # in an MSYS2 MinGW64 shell; produces mmlplay, mmltrace, mmldump
    mmlplay --help            # full list of options

    mmlplay song.mus song.wav                         # 60 s, machine taken from the file
    mmlplay song.mus song.wav -t 120 -v 24            # 120 s, quieter
    mmlplay song.mus psg.wav -t 30 -m 0x7             # mute channels A,B,C (FM on PC-88): hear only the PSG
    mmldump song.mus -l                               # what the compiler made of the file
    mmltrace song.mus -t 3000                         # chip register writes of the first 3 seconds

Options are `-x value` (letter, space, value only: `-v 40`, not `-v volume=40`); long forms `--volume 40` and
`--volume=40` work too. Wrong or out-of-range values stop with an explanation and a non-zero exit status.

## How it works (presumably)

    song.mus --compile()--> Song --Mixer::play()--> Sequencer --register writes--> Opx4 chip --> samples --> WAV
    (mmlcomp.cpp)                   (mixer.cpp)      (seq.cpp)                      (opx4.cpp)

| File | Role |
|---|---|
| `mmlcomp.cpp/.hpp` | MML compiler: text to FM tone records, volume envelopes and per-channel bytecode. **The header has the MML syntax reference.** |
| `seq.cpp/.hpp` | Sequencer: plays the bytecode (tempo, loops, LFOs, envelopes, bend...) and turns notes into chip register writes. Steps once per millisecond. |
| `opx4.cpp/.hpp` | The chip: 4-operator FM (OPN 3 ch or OPM 8 ch) plus a 3-voice PSG. State layout and every register are documented in the header. |
| `mixer.cpp/.hpp` | Top level: chip + 4 sequencer slots, 1 ms chunking, master volume, 16-bit clipping. |
| `chiptables.inc`, `tables.inc` | Lookup tables extracted from the DLL, each documented with its original address. |
| `mmlplay.cpp`, `mmltrace.cpp`, `mmldump.cpp`, `cli.hpp` | The command-line tools and their shared argument parser. |

Function names in comments such as `[sub_1001A6E0]` are the Hex-Rays names in `wuopn.cpp`, so any function can be compared
against the decompile. The chip keeps the original's flat state layout (same dword numbers) on purpose; the names
(`OP_PHASE`, `CH_FREQUENCY`...) are listed in `opx4.hpp`.

## What has been verified (Mostly)

* **Compiler and sequencer**: all register writes over 40 s were compared before and after every refactoring;
  note timing matches the tick arithmetic of the DLL (e.g. 48 ticks at tempo 76 = 792 ms).
* **Chip**: a differential fuzz test drove the renamed chip and its predecessor with random register writes in all six
  chip modes (600 steps x 6 modes, byte-identical output), and unrestricted random writes (3000 steps x 6 modes) run
  clean under AddressSanitizer/UBSan. Every table read is bounds-checked.
* **Against the real plugin**: two recordings of the original (PC-88 and X1 files, 44.1 kHz). Pitch matches to four
  digits, loop periods match, overall level 1.00 / 0.96, spectra track frame by frame (median correlation 0.77 / 0.85).

## Known differences from the original

1. **FM level calibration (not from the decompile).** The DLL sets the chip output gain to 1024, but recordings of the
   original fit an FM level twice as high (PSG unchanged), in both OPN and OPM modes. `mixer.hpp` applies 2048
   (`kFmOutputGain`); `mmlplay -g 1024` gives the value as decompiled. The cause is not yet known.
2. PSG on PC-88 measures about 1.18x louder in the original; brightness above 1 kHz is about 20% higher in the original.
3. The noise generators use the C runtime's `rand()`. This port reproduces MSVC's sequence with seed 1; the real seed
   depends on what ran earlier in the game process (`-s` changes it).
4. Output is sample-exact only for a given render block size (`-b`, default 1024): the 1 ms sequencer stepping inside a
   block shifts note onsets by a few samples when the block size changes. It sounds the same.
5. MZ/PWM mode is ported but untested against a recording. Chip modes 1-3 exist in the DLL but nothing in the game
   selects them.
6. A few oddities of the original are kept on purpose (see comments): e.g. OPN register 0x27 (channel-3 mode) never
   takes effect, a trailing `//` comment on a channel line joins it to the next line, the amplitude LFO's delay is
   consumed by the first note.
