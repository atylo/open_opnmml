# opnmml - open-source player for the `.mus` MML music format

A from-scratch C++ port of a game music plugin (an "OPN MML decoder"): it reads `.mus` MML text files
and renders them to audio, emulating the sound hardware of the PC-88 (YM2203 FM + PSG), Sharp X1 (YM2151 FM + PSG)
and MZ-2000 (PWM). It is written from a decompile of the original plugin, with its read-only data tables, and checked
against recordings of the original plugin. No code of the original host application and no Windows API is needed.

## Build and use

    make                      # in an MSYS2 MinGW64 shell; produces mmlplay, mmltrace, mmldump
    mmlplay --help            # the everyday options;   mmlplay -d --help   also lists the debug-mode options

    mmlplay song.mus song.wav                         # 60 s, machine taken from the file
    mmlplay song.mus song.wav -t 120 -v 24            # 120 s, quieter
    mmlplay song.mus psg.wav -t 30 -d -m 0x7          # debug mode: mute channels A,B,C (FM on PC-88): hear only the PSG
    mmldump song.mus -l                               # what the compiler made of the file
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
2. **Per-slot state.** The DLL keeps a slot's tone and envelope tables when another file is compiled into the same slot;
   this port compiles each file from scratch (to be changed with the multi-song features, see TODO.md).
3. Output is sample-exact only for a given render block size (`-b`, default 128, which is what the DLL's DirectSound path uses): the 1 ms
   sequencer stepping inside a block shifts note onsets by a few samples when the block size changes. It sounds the same.
4. In-game recordings of the original differ from this engine's output by a smooth frequency tilt (about -10% below 500 Hz,
   +20% above 2.7 kHz), the same in both example songs. Applying it makes the apparent "PSG 1.18x louder" in the PC-88 recording
   disappear (PSG 0.99, FM 1.01). It is not in the engine (which is identical to the DLL's) and not in the game scripts supplied;
   it comes from the capture chain (Windows/driver enhancements or the game's audio path). Recordings are therefore good for
   pitch, timing and character, not for absolute spectrum.
5. A few oddities of the original are kept on purpose (see comments): e.g. OPN register 0x27 (channel-3 mode) never
   takes effect, a trailing `//` comment on a channel line joins it to the next line, the amplitude LFO's delay is
   consumed by the first note.
