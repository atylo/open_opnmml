# The `.mus` MML language - reference

This describes the music language read by the original plugin that this project ports: what each command means, what
numbers are allowed, and what actually happens to them when the music is compiled and played.

How to read it.  Everything here was taken from the port's compiler (`mmlcomp.cpp`) and sequencer (`seq.cpp`), which
produce the same output as the original plugin (checked by `verify/`).  Statements marked **[tested]** were also
confirmed by compiling small files; statements marked **[code]** come from reading the code and were not checked by
ear.  Byte values are in hexadecimal.

Contents

1. File structure
2. Dialects and machines
3. Channels - what the choice of letter changes
4. Notes, rests and lengths
5. Pitch: octave, key signature, transpose, detune, bend, portamento
6. Volume and expression: v, relative volume, envelopes, release, LFOs, pan, fade
7. Tempo
8. Loops and the song loop point
9. Instruments: FM tones, PSG tones, volume envelopes
10. Raw chip registers: `y`
11. Limits and error messages
12. Known quirks of the original (kept on purpose)
13. A complete small example
14. Appendix: bytecode

---

## 1. File structure

A `.mus` file is plain text (Shift-JIS or ASCII; only ASCII matters).  The compiler is a line scanner that looks
for exactly four kinds of things and **silently ignores everything else**:

| Starts with | Meaning |
|---|---|
| `#` | a directive (`#title`, `#octave`, `#machine`, `#total_offset`) |
| `@` followed by a digit or `e` | a tone definition `@n={...}` or an envelope definition `@en={...}` |
| a **capital letter A-K in the first column** | a channel line: music for one or more channels |
| `//` or `/*` | a comment |

Anything else - prose, a lowercase letter at the start of a line, an indented channel line - is ignored **without any
message**.  A channel line that starts in column 2 or with a lowercase letter is therefore simply lost. **[tested]**

### Comments

* `// ...` runs to the end of the line.
* `/* ... */` can span lines and can be used inside a channel line. **[tested]**
* **Trap:** a `//` comment at the *end of a channel line* does not end that line.  The line continues onto the next
  line, which is read as more music for the same channel.  If the next line starts with another channel letter you get
  "Unknown command".  Put comments on their own line, or use `/* ... */` at the end of channel lines. **[tested]**

### Channel lines

    D	t76 q8 o4 l8 cdefg

* One or more channel letters, then a **space or tab**, then the music.  `ABC<tab>...` plays the same text on A, B and
  C.  Each of those channels is compiled separately from the start of the text, so each begins in a fresh state. **[tested]**
* A channel line is complete at the end of the physical line.  **The last line of the file must end with a newline** or
  it is dropped without a message. **[tested]**
* Several lines for the same channel are appended in order.  Octave, default length, quantize, key signature, transpose and
  loop nesting carry over from one line to the next for that channel. **[tested]**

### Definitions and directives

    #title    "Gate of steiner OPM+PSG"
    #octave   n                       (or x)
    #machine  x1                      (pc88, x1 or mz)
    #total_offset  -2
    @1={ ... }                        FM tone 1       (section 9)
    @e3={ ... }                       volume envelope 3 (section 9)

* `#title "text"` - the song title (up to the closing quote, same line).  Shown by the player.
* `#octave n|x` - `n` normal; `x` reverses the direction of `<` `>` **and of `(` `)`**.  Applies to every channel from that
  point on.  Default `n`. **[tested]**
* `#machine pc88|x1|mz` - selects the **dialect** (section 2).  Directives are case-insensitive.
* `#total_offset N` - an integer from -8 to 100: how much louder (negative) or quieter (positive) all FM carrier
  operators are made.  Default 0.  Section 9 explains the arithmetic.  Takes effect for tones defined **after** it. **[code]**

Directive names and the tone `@e` prefix are case-insensitive; **commands inside channel lines are case-sensitive**.

---

## 2. Dialects and machines

There are two separate things:

* the **dialect** of the text, set by `#machine` (default `pc88`): which channel letters exist and how many numbers a
  tone operator has;
* the **machine** whose sound chip plays it (the game picks it; in this project the `-dev` option of the player's debug mode, or else the `#machine` line).

| `#machine` | Channel letters | Values per tone operator | Meant for |
|---|---|---|---|
| `pc88` (default) | A-F (6) | 10 | PC-8801: YM2203 - 3 FM + 3 PSG |
| `x1` | A-K (11) | 11 | Sharp X1turbo: YM2151 - 8 FM + 3 PSG |
| `mz` | A-F (6) | 10 | MZ-2000: PWM sound.  The compiler treats `mz` exactly like `pc88` (identical bytes, **tested against the original**); the line only tells a player which chip to choose |

Using a letter beyond the dialect is an error: "Bad channel header ... A-F (this dialect has 6 channels)". **[tested]**

The dialect does **not** choose the chip.  In the original plugin the chip is chosen by the game script, and a file played on
the wrong chip is not corrected: the sequencer decides FM or PSG from the chip and the channel number alone (next section).

---

## 3. Channels - what the choice of letter changes

### At compile time: almost nothing

The compiler never looks at which channel it is compiling, except to check that the letter exists in the dialect.
The same text on channel A, D or F compiles to **byte-identical** code. **[tested]**  What a channel letter does at compile
time is only to pick *which channel's* state and code the text goes to: each channel has its own octave, default length,
quantize, key signature, transpose, loop stack and tick counter.

Commands that act on the whole song rather than on one channel are `t` (tempo) and `F` (fade-out).  They are stored in the
channel that contains them, but when they run they change the tempo or fade of the **whole slot**.  If two channels
contain different `t` commands at different times, the last one executed wins; the usual practice is to put `t` at the
start of every channel (`DEF t76`) so they agree.

### At play time: which voice, set by chip and channel number

| Chip | A | B | C | D | E | F | G | H | I | J | K |
|---|---|---|---|---|---|---|---|---|---|---|---|
| PC-88 (OPN) | FM 1 | FM 2 | FM 3 | PSG 1 | PSG 2 | PSG 3 | - | - | - | - | - |
| X1 (OPM) | FM 1 | FM 2 | FM 3 | FM 4 | FM 5 | FM 6 | FM 7 | FM 8 | PSG 1 | PSG 2 | PSG 3 |

**[code]**  That is why PC-88 files use **ABC for FM and DEF for PSG**, and X1 files use **A-H for FM and IJK for PSG**.  Using
`D` for FM on the PC-88 is simply not possible: the chip has no fourth FM voice.

What the voice type changes:

| | FM voice | PSG voice |
|---|---|---|
| `@n` | picks FM tone n from the tone table | selects tone/noise mode (section 9.2) |
| `v` | attenuates the carrier operators (3 dB per step; `v0` is quiet but **not** silent) | the chip's 4-bit amplitude (`v0` = silent) |
| `p` pan | stereo position - **X1 only** (the OPN has no pan) | none |
| `MP`, `MA`, `D`, `P`, `R`, `EA`, bend | work | work |
| lowest/highest note | o0 c+ ... o7 b | below o1 c all notes sound as o1 c |
| key-off | the tone's own release (RR) is heard | amplitude drops to 0 at once |

On the **MZ** chip (PWM) the FM voices make **no sound at all** (the original behaves the same); only D-F (PSG, with its output inverted so that it sounds different from the PC-88) are heard.  This is why the `_pwm` files only use channel F. **[tested]**

A channel that stops (end of its code without a loop) is silenced.

### Several slots playing at once

The game plays background music in slot 3 and sound effects in other slots.  Slots are processed in order 0, 1, 2, 3 and a
channel that is playing in a lower slot **keeps** that chip voice: the same channel of a higher slot keeps counting time
but writes nothing to the chip until the lower one has finished. **[code]**

### Muting

A mute mask silences channels: bit 0 = A, bit 1 = B, ... bit 10 = K.  The game uses mask `7` (A, B, C = all FM) on
machines that only have the PSG, so a file shared with the FM machines plays only its PSG parts there.

---

## 4. Notes, rests and lengths

| Command | Meaning |
|---|---|
| `c d e f g a b` | the note, in the current octave, for the default length |
| `r` | rest |
| `+` `-` after a note | sharp / flat; may be repeated (`c++`).  There is no `#`. |
| number after a note | length: `c4` quarter note, `c8` eighth, ... |
| `l`*length* | set the default length (`l8`) |

### Lengths

The unit is the **tick**: a whole note is **192 ticks**, a quarter note 48.

| Written | Ticks | Note |
|---|---|---|
| `4` | 48 | `192 / n`, whole-number division |
| `5` | 38 | 192/5 = 38 (rounded down): not every number divides evenly |
| `4.` | 72 | each dot adds half of the previous value: `4..` = 84 |
| `4^8` | 72 | `^` adds another length (tie) |
| `4~8` | 24 | `~` subtracts |
| `%12` | 12 | `%` means "this many ticks", e.g. `c%5`, `l%12` |

Maximum length 16000 ticks, minimum 1.  `c0` is an error "Note length out of range". **[tested]**

### Gate time: `q` and `@q`

A note does not sound for its whole length: the end is cut so that repeated notes are separate.

* `q`*n* (1-8): the note sounds `length * n / 8` ticks, then a rest fills the rest of the time.  `q8` = full length
  (default), `q4` = half. **[tested]**
* `@q`*n* (1-127): the note sounds `length - n` ticks (at least 1). **[tested]**

### Ties and slurs

* `c4^8` adds lengths inside one note.
* `c4&d4`: `&` after a note means "no gap and do not retrigger": the first note plays its full length and the next note
  continues without a new attack (envelope and LFO are not restarted; only the pitch changes). **[tested]**

---

## 5. Pitch

### Octave and note numbers

`o`*n* sets the octave 0-7 (default 4); `>` raises and `<` lowers it by one (swapped by `#octave x`).  The octave is checked
after **every** command, so a stray `>` that goes to octave 8 is an error even if `<` follows. **[tested]**

A note's number is `12 * octave + (c=0 d=2 e=4 f=5 g=7 a=9 b=11) + accidentals + transpose` and must come out from 1 to
95.  So the lowest note is `o0 c+` (`o0 c` is an error) and the highest is `o7 b`. **[tested]**

### Key signature

    $SHARP{cf}     every c and f is raised a semitone from here on
    $FLAT{be}      every b and e is lowered

Each `$` command first resets to C major and then applies its list (letters `c d e f g a b` only).  An accidental written on
a note is added to the signature.

### Transpose and detune

* `T`*n* (-64 to 64): transposes every following note by n semitones **at compile time**.
* `D`*n* (-8192 to 8192): fine detune in 1/16 semitone steps (so `D16` = one semitone, `D1` = about 6 cents). **[code]**

### Pitch bend: `note length _ target`

    c4_e4

Slides from the first note's pitch to the target's.  Detail: the **target note is the note that sounds**; the length written
after the *first* note is how many ticks the slide takes; the first note itself takes no time of its own.  So `c4_e4` plays
an e for a quarter note whose pitch starts at c and reaches e after 48 ticks. **[tested]**  The target may contain `<` `>`
and accidentals before the letter.  A bend cannot start from a rest, and the slide must fit its length (error "too wide").

### Portamento: `P`*n* (0-7)

`P0` is off.  For 1-7 each tick moves the pitch a fraction of the way to the target: `new = (n*old + (8-n)*target) / 8`,
so a larger number is slower and smoother.  The glide starts from the previous note's pitch. **[code]**

---

## 6. Volume and expression

### Volume

* `v`*n* (0-15): channel volume, 15 loudest.  For FM, each step below 15 adds 4 to the total level of every carrier
  operator (= 3 dB); the result is limited to 127.  For PSG it is the chip's amplitude directly.
* `(`*n* and `)`*n* (1-10, default 1): relative volume.  With `#octave n` (default) **`(` lowers and `)` raises** the volume;
  with `#octave x` it is the other way round. **[tested]**  The volume stays within 0-15.

The final level of every tick is: `v` + envelope + amplitude LFO - release decay - fade-out, limited to 0-15.

### Volume envelope: `EA`*n*`,`*speed* and `EAOF`

Selects software envelope n (defined with `@en={...}`, section 9.3) for the following notes.  `speed` (1-100): each value of
the envelope lasts `speed + 1` ticks.  `EAOF` switches it off.  The envelope restarts at every new note (not on a tied
note).  `n` is 1-63; `0` means off.  **Define every envelope you use**: an undefined one is read from neighbouring memory
in the original (see section 12).

### Release decay: `R`*step*`,`*volume* and `ROF`

Without `R` a note is cut off (key-off) at the end of its gate time.  With `R`, the gap (the rest part after the gate, and
real rests) is not silent: the volume falls by `volume` (1-15) now and again every `step + 1` ticks (step 0-127), and the note is
keyed off only when it reaches zero. **[code]**  `ROF` (= step 0) restores the instant cut.

### LFOs: `MP` (pitch) and `MA` (volume)

    MP type,period,depth[,delay]      MA type,period,depth[,delay]      MPOF      MAOF

| Field | Range | Meaning |
|---|---|---|
| type | 0-4 | 0 off; 1 square (alternates between 0 and depth); 2 triangle (ramps 0 to depth and back); 4 alternating (0, +depth, 0, -depth); **3 does nothing** |
| period | 1-100 | ticks per step |
| depth | -512 to 511 | `MP`: 16 = one semitone.  `MA`: volume steps.  Values outside -128..127 are stored in quarter resolution |
| delay | 0-100 | ticks before it starts (default 0) |

The LFO restarts with each new note.  **[code]**

### Pan: `p1`, `p2`, `p3`

1 = left, 2 = right, 3 = both.  Only the X1 chip (FM channels A-H) has stereo; on the PC-88 chip it is accepted and ignored.

### Fade-out: `F`*n* (1-255)

Starts a fade-out of the **whole slot**: the volume of every channel is reduced by one step every `256 / (256 - n)` updates of
16 ms, and when it is finished every channel stops.  The total time is about `3825 / (256 - n) * 16` ms: `F16` takes
0.26 s (this is what the stop command of the game uses), `F128` 0.48 s, `F200` 1.1 s, `F255` about 61 s. **[code]**
A fade also ends a looping song: use it at the end of a non-looping piece.

---

## 7. Tempo

`t`*n* (1-255) = quarter notes per minute.  The default when a song starts is **120**.  One tick lasts
`60000 / (48 * n)` ms: at `t120` a tick is 10.4 ms, a quarter note 0.5 s and a whole note 2 s.  The tempo belongs to the slot, not the
channel (section 3).

The game can also change the tempo from outside (it does so for the "unstable tempo" effect on the X1 machine).

---

## 8. Loops and the song loop point

    [ ... ]n          repeat n times (n 1-127, default 2)
    [ ... / ... ]n    on the last pass, skip the part after the '/'
    L                 the song loop point

* Loops can be nested **5 deep**.  A loop body must contain at least one note or rest ("zero length" error). **[tested]**
* `L` marks where the channel jumps back to when its code ends.  Without an `L` a channel simply stops at its end.
* `L` cannot be inside a `[ ]` loop.
* A channel with `L` as its very last command (nothing after it) is an error.
* **Channels loop independently.**  Nothing requires all channels to have the same length: if they differ, the parts drift
  against each other.  Good files make every channel the same length (the example files do: `mmldump` prints the tick
  count of every channel and the loop point).

---

## 9. Instruments

### 9.1 FM tones: `@n={ ... }`

`n` is 0-127 (but only 1-127 can be *selected* with `@n`; a channel starts on tone 1).  The braces may span lines.  A tone is a
list of numbers: **4 operators** with N values each, then 2 to 4 channel values.  Operator order is the standard FM order
(operator 1, 2, 3, 4).

Values of **each operator** (in this order):

| # | Name | Range | Meaning |
|---|---|---|---|
| 1 | AR | 0-31 | attack rate (31 = instant) |
| 2 | DR | 0-31 | first decay rate (D1R) |
| 3 | SR | 0-31 | second decay / sustain rate (D2R) |
| 4 | RR | 0-15 | release rate |
| 5 | SL | 0-15 | sustain level (0 = loudest) |
| 6 | TL | 0-127 | total level: **attenuation**, 0 loudest |
| 7 | KS | 0-3 | key scaling |
| 8 | MUL | 0-15 | frequency multiple |
| 9 | DT | -3 to 7 | detune: 0-3 up, -1 to -3 down; 4-7 are the raw chip values (5 = -1, 6 = -2, 7 = -3) |
| 10 | AMS | 0-1 | amplitude-LFO sensitivity of the operator (**PC-88 dialect**) |

In the **X1 dialect** there are 11 values: the 10th is **DT2** (0-3, coarse detune) and the 11th is **AMS** (0-1).
**Warning, tested against the original:** the original stores the DT2 value in the byte that holds AMS (bits 7-6), so
DT2 = 2 or 3 turns amplitude sensitivity on, and the 11th value is read and then never stored.  Write `0,0` there, as every
shipped file does.

Then the **channel values**:

| # | Name | Range | Meaning |
|---|---|---|---|
| 1 | AL | 0-7 | algorithm (how the operators are connected) |
| 2 | FB | 0-7 | feedback of operator 1 |
| 3 | PMD | 0-7 | pitch sensitivity (optional; 0 when omitted) |
| 4 | AMD | 0-3 | amplitude sensitivity (optional; 0 when omitted) |

(Values 3 and 4 only matter when the chip's own LFO is switched on with `y`; they are stored as one byte `AMD*16 + PMD`, which
the chip registers of the PC-88 and the X1 read with different layouts.  The software LFOs `MP`/`MA` do not use them.)

Total number of values: PC-88 42-44, X1 46-48.  Wrong counts and out-of-range values are errors that name the parameter and
operator.

Example (the Piano of the shipped files, PC-88):

    //	AR DR SR RR SL OL KS ML DT AMS
    @001={	25, 2, 2, 0, 0,35, 2, 1, 3,0,
    	24, 0, 1, 5, 0,38, 3, 1, 0,0,
    	31, 6, 5, 5, 3,42, 2, 5, 5,0,
    	25, 6, 4, 6, 1, 2, 2, 1, 0,0,	 2,7}

**Which operators are carriers** (the ones that make sound and that volume changes): algorithm 0-3: operator 4;
4: operators 2 and 4; 5-6: operators 2, 3 and 4; 7: all four.

**Volume arithmetic.**  When the tone is compiled, `TL + (#total_offset + 8)` is stored for every carrier (so the default
makes the FM about 6 dB quieter, to balance with the PSG).  If that sum would exceed 127 the carrier is stored as silent.
When a note starts, the carriers get `+ 4 * (15 - v)` and the total is limited to 127.  Modulators (non-carriers) are not touched
by `v` or `#total_offset`.

### 9.2 PSG "tones": `@n` on a PSG channel

On a PSG voice `@n` does **not** pick a tone table entry; the number is read as bits:

| `@n` | Result |
|---|---|
| 1-31 | tone (square wave) only |
| 32-63 | **noise** only; bits 0-4 = noise period |
| 64-95 | tone only (bits 0-4 set the noise period but noise is off) |
| 96-127 | tone **and** noise; bits 0-4 = noise period |

i.e. bit 6 (64) = tone on, bit 5 (32) = noise on, bits 0-4 = noise period (0-31).  The noise period is shared by all three PSG
voices. **[code]**  For example `@64` is a plain square wave, `@96` square plus noise, `@32` pure noise (hi-hat/percussion).
Volume shape comes from `v`, `EA` and `R`; there is no tone table for the PSG.

### 9.3 Volume envelopes: `@en={ ... }` and `@en,loop={ ... }`

    @e1={-1,0,0,0,0,-1,-1,-2,-2,-3}
    @e2,1={0,-1,-2,-3}

* `n` is 1-63; the list has 1-63 integers.
* The numbers are **added to the channel volume** (0-15 scale) on every tick, one value for each `speed + 1` ticks
  (`EA n,speed`).  Negative = quieter.
* When the end is reached the envelope jumps to the **loop index** (counting from 0): the number after the comma in the definition
  (`@e2,1` loops from the second value); the default is the last value, so it just holds its final value.
* It restarts at each new note.

Typical shapes: a decay `{0,-1,-2,-3,-4}`; a fade-in `{-15,-14,...,0}`; a tremolo `{0,-2,0,-2}` with a loop index of 0.

---

## 10. Raw chip registers: `y`

`y`*reg*`,`*value* (both 0-255) writes a value straight into a register of the sound chip, using the register map of the
current chip (OPN or OPM).  It is meant for features the language has no command for (chip LFO, noise on the X1's OPM,
...).  Wrong values can make a channel silent or loud; on the OPN register `27` is ignored by this port as by the original.

---

## 11. Limits and error messages

All numbers are whole numbers; a leading `+` or `-` is accepted where a negative value is allowed.  Errors report
`file:line:column`.

| Command | Valid | Message when wrong |
|---|---|---|
| `[` nesting | 5 | Loops are nested too deeply |
| `]`*n* | 1-127 | Repeat count out of range |
| `@`*n* | 1-127 | Tone number out of range |
| `@q`*n* | 1-127 | Quantize @q out of range |
| `q`*n* | 1-8 | Quantize out of range |
| `D`*n* | -8192..8192 | Detune out of range |
| `v`*n* | 0-15 | Volume out of range |
| `(` `)` | 0-10 | Relative volume out of range |
| `R` step,vol | 0-127, 1-15 | Bad R command |
| `EA` n,speed | 0-64, 1-100 | Bad volume envelope command |
| `MP`/`MA` | type 0-4, period 1-100, depth -512..511, delay 0-100 | Bad LFO command |
| `F`*n* | 1-255 | Fade-out out of range |
| `p`*n* | 1-3 | Pan out of range |
| `t`*n* | 1-255 | Tempo out of range |
| `P`*n* | 0-7 | Portamento out of range |
| `T`*n* | -64..64 | Transpose out of range |
| `y` a,b | 0-255 each | Bad y command |
| `o`*n* | 0-7 | Octave out of range |
| length | 1-16000 ticks | Note length out of range |
| note | 1-95 | Note pitch out of range |
| `#total_offset` | -8..100 | #total_offset expects a whole number |
| `@n` / `@en` | 0-127 / 1-63 | Tone / Envelope number out of range |

At the end of the file: an unclosed `[`, and an `L` with nothing after it, are reported per channel.

---

## 12. Known quirks of the original (kept on purpose)

These behaviours are in the original; real `.mus` files were written against them, so the port keeps them.

1. A trailing `//` comment on a channel line swallows the next line (section 1). **[tested]**
2. The octave range is checked after every command. **[tested]**
3. The delay of an amplitude LFO (`MA ...,delay`) is used up by the first note: later notes start the LFO at once. **[code]**
4. Writing OPN register `27` with `y` has no effect. **[code]**
5. A file played on the wrong chip plays whatever the chip and channel number imply (the port warns).
6. In the X1 dialect the tone's DT2 value is stored in the AMS byte and the 11th value is dropped. **[tested]**
7. Lowercase or indented channel lines, and a final line without a newline, are silently ignored. **[tested]**
8. Starting a song number that was never loaded plays the first song of the slot.
9. On the X1 chip, FM channels D-H (not A-C) go to volume 0 at key-off like PSG voices, which shortens their release.  **[code]**
10. An envelope that is selected with `EA` but was never defined reads whatever memory follows the envelope table: the effect
    is unpredictable.  Always define the envelopes you use.

---

## 13. A complete small example

This compiles, and plays on the PC-88 chip: FM bass and chords on A-C, a square-wave lead and a noise hi-hat on D-F.

    #title "Example"
    #octave n

    @1={ 31, 5, 2, 7, 2,20, 1, 1, 0,0,
         31, 5, 2, 7, 2,10, 1, 1, 0,0,
         31, 5, 2, 7, 2,10, 1, 1, 0,0,
         31, 5, 2, 7, 2,10, 1, 1, 0,0,   4,5 }
    @e1={0,-1,-2,-3}
    @e2={-4,-2,0}

    ABCDEF	t120 q8
    A	@1 v13 o3 l8 L [c c c c]2
    B	@1 v9  o4 l2 L e g
    C	@1 v9  o4 l2 L g >c<
    D	@64 v12 EA1,2 o5 l4 L c e g e
    E	@64 v8  o4 l4 L r g r g
    F	@32 v10 EA2,1 o4 l8 L [c r]4

Notes on it: the first line `ABCDEF t120 q8` is shared by all six channels (each gets its own copy of `q8`; every channel sets the
same tempo).  Channel A repeats a pattern, B and C hold two half notes, D plays a lead with a decaying envelope (`EA1,2`), E is a
second square voice, and F uses `@32` (pure noise) with a quick envelope as a hi-hat.  Every channel is exactly 192 ticks long
(one 4/4 bar), so they loop together at `L`.  `mmldump` shows the tick count of each channel; if they differ, the parts drift.

---

## 14. Appendix: bytecode

Each channel compiles to a byte string ending in `00`.  Notes are `pitch length` (pitch 1-95, `7F` = rest); a length of 127 or
less is one byte, longer is two bytes `((n >> 7) | 80) (n & 7F)`.  Commands are `opcode argument [more]`:

| Opcode | Command | Arguments |
|---|---|---|
| `80` | `v` | volume |
| `81` | `D` | detune, 16 bit big-endian |
| `82` | `@` | tone |
| `83` | `&` | (none) |
| `87` | `t` | tempo |
| `88` | `P` | portamento |
| `8F` | `y` | register, value |
| `90` | pitch bend | amount (16 bit), rate (16 bit), length (16 bit) |
| `94` | `R` | step, volume |
| `95` | `F` | fade |
| `96` / `97` | `)` / `(` (normal octave direction) | amount (volume up / down) |
| `98` `99` `9A` | `p1` `p2` `p3` | (none) |
| `A0` / `A2` | `MP` | type, period, depth, delay (`A2`: depth stored /4) |
| `A1` / `A3` | `MA` | type, period, depth, delay (`A3`: depth stored /4) |
| `A5` | `EA` | envelope, speed |
| `C0` | `[` | repeat count |
| `C1` | `/` | bytes to skip (16 bit) |
| `C2` | `]` | bytes to go back (16 bit) |
| `CF` | `L` | (none) |
| `00` | end of channel | |

`mmldump` prints the compiled channels and `mmltrace` the chip register writes the sequencer makes from them.
