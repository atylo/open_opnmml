verify/ - checking the port against the ORIGINAL plugin DLL
============================================================

These tools run code from the real DLL inside a CPU emulator (Unicorn) - no host application, no Windows - and compare it with the
port. The DLL's compiler, sequencer, chip and mixer are plain computation, so they run fine when called directly by
address (the sub_XXXXXXXX numbers). Only the host-facing code (V2Link, the TJS glue) is never run.

Setup:   pip install unicorn pefile        (Python 3, g++ on the PATH)
         export ORIGINAL_DLL=/path/to/the/original.dll     (default: original.dll in the project folder, i.e. one above verify/)
         Run the tools from this folder; they build their small C++ helpers on first use.

What is faked: the few KERNEL32 calls (critical sections, heap), a fake thread block, operator new/delete (bump allocator),
sprintf/printf (the DLL's own copies need C-runtime start-up state), and the C runtime's per-thread-data lookup, so that the
DLL's own rand() runs with its default seed 1.

Tools
-----
emu.py            the emulator wrapper (maps the DLL at 0x10000000, patches its imports, calls functions with thiscall/cdecl)
engine.py         the DLL's mixer as a Python object: compile / play / render, and dump_song() to read back what the DLL's
                  compiler stored (bytecode, tones, envelopes)
chip_diff.py      the chip (Opx4) against the DLL's chip, sample by sample
                    chip_diff.py random --steps 300 --seed 7        random register writes in all 6 chip modes
                    chip_diff.py song FILE.mus --ms 6000            the register stream of a real song
                    chip_diff.py state --mode 0 --steps 60          compare the WHOLE chip state after every command
mixer_diff.py     the whole engine (compiler + sequencer + chip + mixer) on one song:  mixer_diff.py FILE.mus --device 4
compile_fuzz.py   random MML programs through both compilers (verdict, error line, and complete compiled output)
                    compile_fuzz.py --valid --count 600 --seed 11      mostly-valid programs (exercises the accept path)
                    compile_fuzz.py --count 800 --seed 21              corrupted programs
compile_boundary.py  every command argument at edge values (negative, 0, limits, 32-bit overflow); use --part N/4 to split
song_fuzz.py      random songs played in both engines on a random device; audio compared sample for sample

Results at the time of writing (all against the DLL supplied with the project)
-------------------------------------------------------------------------------
chip              6 chip modes x 300 random steps: identical samples; whole state identical after every command (modes 0, 1, 4);
                  both example songs, 6 s each: identical
whole engine      both example songs, 15 s each, on their own machine: identical 16-bit output;
                  later: all 18 example songs, 8 s each, on their own machine (PC-88 / X1 / MZ): identical 16-bit output;
                  the volume gain is identical for all 513 volume values
compiler          4,515 boundary programs + 7,000 random programs: identical verdict, identical error line (about 5,000 compared) and
                  identical compiled output.  (An earlier version of the test never actually compared the error lines because its
                  helper did not pass the port's message through; that was found and fixed afterwards.)
song fuzz         ~100 random songs (every bytecode command exercised) on random devices: identical audio

Differences found by this testing, and fixed in the port
---------------------------------------------------------
1. The mixer constructor sets the chip's output gain to 2048 through a call the decompiler had mislabelled as a library function
   (sub_100195B0). The port now does the same (this was the "FM is twice as loud as the decompile" mystery).
2. Ten argument variables of the command compiler are `unsigned` in the original, so negative values are rejected for
   relative volume ( ) ), EA, LFO type and delay, P, R step, v and y. The port typed them as signed.
3. The decompiler printed the real prototype of the OPN register writer wrongly (5 arguments instead of 3), which only affected
   the test harness.

Original-plugin bugs the port deliberately does NOT copy
--------------------------------------------------------
* A pitch bend on a note whose length works out to <= 0 divides by zero and crashes the plugin; the port rejects the file.
* A stray ']' with no '[' indexes the loop stack at -1 and can crash the plugin; the port rejects the file.
* A bend with no target note letter ("c_") scans past the end of the line (undefined behaviour); the port rejects it.

Multi-song slots (added later)
------------------------------
slot_diff.py       drives the DLL's slot functions (compile 0x10023060, play 0x10022D30, fade-stop 0x10022CE0, device, tempo, title)
                   and the port's Mixer (slotscript.cpp, text commands on stdin) and compares ALL observable slot state after every
                   operation (code vectors, tones, envelopes, 64x11 start offsets, sequencer runtime state, audio).
                   --scenarios : 18 hand-written checks;  --count N --seed S : random operation sequences (random mute masks, volumes,
                   stop+reset, device changes);  --fade-tempo : quick fade-out and tempo;  --cut : stop+reset on its own;
                   --priority N : songs in all four slots on overlapping channels (the lowest slot playing a channel keeps its voice).
                   The comparison was mutation-tested (port ignoring `num`, port dropping inherited tones: both are caught).
helpers            compdump.cpp, mixerscript.cpp (now uses Mixer::loadSong/playSong), chipscript.cpp (needs Opx4::stateImage()).
Known, expected differences are listed at the top of slot_diff.py and in DESIGN.md section 6.

Song length
-----------
songlen_diff.py    runs the DLL's own step function (no audio) tick by tick and watches the channel objects, and compares the moment of
                   every event (a channel has notes, passes its L, reaches its end, goes round the loop a second time, is cut by a fade-out)
                   with what the port's measurement (songlen.cpp, shown by mmldump and mmlplay) says, via lenscript.cpp.
                   All 18 example songs and 12 special cases (tempo changes, F fade-out, different loop lengths, lead-ins, very slow and fast
                   tempo, L as the first byte, rests only ...): identical to the millisecond.  --cases runs only the special cases.
                   Mutation-tested (end one millisecond late, second trip counted wrongly, fade-out not recognised): all caught.

