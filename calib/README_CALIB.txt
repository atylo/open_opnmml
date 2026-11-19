Calibration kit - where does the "tilt" between the game recordings and the port come from?
==========================================================================================

RESULT (experiments done): the tilt comes from the Windows sound enhancements (SoundAlive), not from the game or the port.  A plain WAV
played without the game shows the same tilt with enhancements ON; with enhancements OFF the game's output equals the port's output at every
tested frequency, with DS mode on and off.  The kit stays useful for checking any future recording (use compare_recording.py first, then
compare_detail.py).  The text below is the plan that was followed.

What we know
  The port's output is bit-identical to the original plugin's, and the plugin adds nothing between its mixer and the sound buffer.  Your two
  recordings (bgm01_opm, bgm03_fm3) still differ from the port's render by a smooth, stable filter (recorded_tilt.txt):
      about 0.92 x below 300 Hz, rising from 400 Hz to 1.20 x at 2.5 kHz, about 1.23 x from 4 to 16 kHz, falling again above 18 kHz.
  Both songs, both channels and both halves of each song give the same curve (spread about 2 %).  The DC offset is kept (0.90 x), so the
  recording chain is digital, not an analogue loop.  The clocks differ by about 57 ppm, which is normal for two audio clocks.
  Something between the plugin and the recording does this.  The experiments below find out what.

The files
  make_calib.py         writes the three files below (already done, you do not need to run it)
  calib_fm3.mus         test song for the GAME (PC-88 dialect, 57 s, no loop): FM sine tones from 65 Hz to 16 kHz, PSG square waves on
                        the same notes, PSG noise.  Name it like a normal song of the PC-88 machine you used for bgm03_fm3, for example
                        bgm01_fm3.mus (keep the original safe!).
  calib_ours_pc88.wav   what the port renders for calib_fm3.mus: the reference for experiments 2 and 3
  control_tones.wav     the same kind of signal as an ordinary WAV file (exact sines): the reference for experiment 1
  compare_recording.py  compares a recording with its reference:   python compare_recording.py RECORDING.wav REFERENCE.wav
                        (needs Python with numpy; you can also just send me the recordings and I run it)
  recorded_tilt.txt     the curve measured on your two songs, to compare the results with

The experiments (record each one the SAME way you recorded the songs; write down how that is)
  1. CONTROL, no game:  play control_tones.wav in an ordinary media player on the same PC, to the same output device, at the same volumes, and
     record it with the same program and settings.  Reference: control_tones.wav.
        shows the 0.92 -> 1.23 shape  => the recording chain (Windows, sound driver or recorder) does it.  The game and the port are innocent.
        flat                          => the game adds it; go on with 2 and 3.
  2. GAME, DS mode ON (the game's default):  play calib_fm3.mus in the game and record it.  Reference: calib_ours_pc88.wav.
  3. GAME, DS mode OFF:  the same.
        2 and 3 differ => the game's own sound path (not the plugin) matters, and the one that shows the shape is the cause.
        2 and 3 equal  => it is after the game: Windows, the driver or the recorder (experiment 1 should then show it too).

Things to write down for each recording (they matter)
  - the recording program, and what it records (Stereo Mix, loopback of the output device, a virtual cable, a microphone ...)
  - the Windows playback format of the output device (Control Panel > Sound > device > Properties > Advanced: for example 48000 Hz, 16 bit)
    and whether "audio enhancements" / "loudness equalization" / any equaliser are on (turn them off if you can, and note it)
  - the game's volume setting and the Windows volume; do not change them during a recording
  - start the recording a few seconds before the sound and stop a few seconds after the end (the file needs about 60 s)
  Send me the recordings (WAV, 44.1 kHz, 16 bit if possible; if your recorder uses 48 kHz tell me) and these notes.
