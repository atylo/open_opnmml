// ================================================================================================================
// mmlplay - render a .mus MML file to a 16-bit stereo WAV file with the ported engine.
//
// Build (MSYS2 MinGW64):   g++ -O2 -s -o mmlplay.exe mmlplay.cpp mixer.cpp opx4.cpp seq.cpp mmlcomp.cpp
// Run "mmlplay --help" for the options.
// ================================================================================================================
#include "cli.hpp"
#include "mixer.hpp"
#include "songlen.hpp"
#include "post.hpp"
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace opnmml;

static const char* kHelpHead =
"mmlplay - render an OPN/OPM/PSG .mus music file to a WAV file\n"
"          (open-source port of a game music plugin)\n"
"\n"
"USAGE\n"
"  mmlplay INPUT.mus OUTPUT.wav [options]\n"
"\n"
"REQUIRED (exactly two file names, in this order)\n"
"  INPUT.mus      the MML music file to play\n"
"  OUTPUT.wav     the WAV file to create (an existing file is overwritten)\n"
"\n"
"OPTIONS - every option is optional; the default is used for any you leave out.\n"
"  Give the option letter, a space, then ONLY the value:   -v 40   (not  -v volume=40)\n"
"  The long form works too:  --volume 40  or  --volume=40\n"
"\n"
"  -t, --time SECONDS     how many seconds of audio to render, 0.1 to 3600.\n"
"                         Default: exactly ONE PASS of the song: its length is measured (see the 'song :' line), the file\n"
"                         ends where the longest channel ends, followed by the natural release of the last notes (until\n"
"                         the sound is 60 dB below the loudest point of the song) and no fade-out.  Every channel stops\n"
"                         at the end of its own text; nothing of the next round is played.\n"
"                         With -t the song keeps looping until the time is up.\n"

"  -v, --volume N         master volume.  Every +27 halves the level, every -27 doubles it: 32 is what the game itself uses,\n"
"                         0 is 7 dB louder, -20 is 12 dB louder.  Range -200 to 512.  Negative values are as loud as the original\n"
"                         allows; whatever gets above full scale is clipped (a warning says how much).  Default: 32\n"
"  -d, -debug, --debug    debug mode: unlocks the technical options (machine, mute, sample rate, gains, timer phase, noise\n"
"                         seed, block size) and prints the settings that were used.  They are only accepted, and only listed\n"
"                         here, when this option is given:   mmlplay -d --help\n"
"  -h, --help             show this text\n";

static const char* kHelpDebug =
"\n"
"DEBUG OPTIONS (debug mode is on)\n"
"  -dev, --device NAME    machine whose sound chip is emulated: PC88, X1 or MZ (not case sensitive).\n"
"                         Default: taken from the file's '#machine' line (PC88 if there is none).\n"
"                         Note: in the original plugin the chip is chosen by the game script (OPNSetDevice) and '#machine'\n"
"                         only selects the MML dialect (channels A-K and 11 values per tone operator for x1, A-F for the\n"
"                         others).  A warning is printed when the device and the file's dialect disagree, and when the\n"
"                         device is MZ and the file has music on channels that are silent there (the MZ chip has no FM).\n"
"  -m, --mute MASK        silence channels. MASK is a number where bit 0 = channel A, bit 1 = B ... bit 10 = K;\n"
"                         decimal or hexadecimal (0x...).  Example: -m 0x7 mutes A, B and C.   Default: 0 (none)\n"
"  -r, --rate HZ          sample rate of the WAV file, 8000 to 192000.             Default: 44100\n"
"  -g, --fm-gain N        chip output gain, 1024 = x1.  Default 2048, which is what the original mixer sets after\n"
"                         creating the chip (the chip's own default of 1024 is overridden).\n"
"  -o, --total-offset N   value of the compiler's carrier total-level offset when the file has no '#total_offset'\n"
"                         (internal value; the original uses 8).  Default: 8\n"
"  -p, --phase N          starting phase of the sequencer's millisecond timer, 0 to 11288 at 44.1 kHz.  Default: 0.\n"
"                         (The original's audio thread is already running when a song starts, so its phase is arbitrary.)\n"
"  -s, --noise-seed N     seed of the random table used by the X1 chip's noise generator and the random LFO waveform.\n"
"                         Default: 1, which is the original's table (Visual C++ rand() that was never seeded).  The PSG noise\n"
"                         does not use this table (it is a fixed shift register), so this seed does not change it.\n"
"  -G, --gain DB|normalize  plain gain in dB (-60 to 60) applied at the very end, or the word normalize: the gain is chosen so that the\n"
"                         loudest sample of the file reaches full scale (the song is rendered twice).  A fixed gain is the same\n"
"                         for every song; normalize adapts to each song.  Default: none\n"
"  -R, --mimic-release    make the file sound like the public release of the soundtrack, in one switch: DC filter, equaliser\n"
"                         'release', gain +12 dB, 0.45 s of silence at the start, then one pass of the song, the start of the\n"
"                         next round, and an 8 s fade-out (starting 0.9 s into the next round) followed by 2 s of silence.  The\n"
"                         numbers were measured on the release of bgm01_x1 (see calib/).  -G, -e (also -e none) and -c still\n"
"                         change their part; with -t the length is that time and the fade-out ends 2 s before it.\n"
"  -c, --dc-block         remove the DC offset of the PSG output with a 15 Hz high-pass (inaudible; centres the waveform)\n"
"  -e, --eq release       apply the gentle equaliser of the public release of the soundtrack (+1.4 dB below 220 Hz, -0.6 dB around\n"
"                         5.5 kHz, +1.7 dB above 11 kHz).  -e none = off (default).  Order of the steps: DC filter, equaliser, gain\n"
"  -b, --block N          frames rendered per engine call, 64 to 65536.  Default: 128, the size the original's DirectSound\n"
"                         path uses.  It does not change how the music sounds, but it moves note onsets by a few samples,\n"
"                         which changes the waveform at the sample level (as in the original).\n";

static const char* kExamplesBasic =
"\n"
"EXAMPLES\n"
"  mmlplay bgm01_opm.mus bgm01.wav\n"
"  mmlplay bgm03_fm3.mus bgm03.wav -t 120 -v 24\n";

static const char* kExamplesDebug =
"  mmlplay bgm03_fm3.mus psg_only.wav -t 30 -d -m 0x7     (mute the FM channels A-C, hear only the PSG)\n"
"  mmlplay bgm01_opm.mus out.wav -d -dev PC88             (force a different machine)\n";

static const char* kHelpTail =
"\n"
"EXIT STATUS   0 = success, 1 = failed (unreadable file, MML error, cannot write output), 2 = bad command line\n";

// The options that exist only in debug mode: short name, long name.
struct DebugOption { const char* shortName; const char* longName; };
static const DebugOption kDebugOptions[] = {
    {"-dev", "--device"}, {"-m", "--mute"}, {"-r", "--rate"}, {"-g", "--fm-gain"},
    {"-o", "--total-offset"}, {"-p", "--phase"}, {"-s", "--noise-seed"}, {"-b", "--block"}, {"-G", "--gain"}, {"-e", "--eq"}};

static bool isDebugFlag(const char* a) { return !strcmp(a, "-d") || !strcmp(a, "-debug") || !strcmp(a, "--debug"); }
static bool isHelpFlag(const char* a)  { return !strcmp(a, "-h") || !strcmp(a, "--help") || !strcmp(a, "-?"); }

static void printHelp(bool debug) {
    fputs(kHelpHead, stdout);
    if (debug) fputs(kHelpDebug, stdout);
    fputs(kExamplesBasic, stdout);
    if (debug) fputs(kExamplesDebug, stdout);
    fputs(kHelpTail, stdout);
}

static int usageError(const char* what) {
    fprintf(stderr, "error: %s\nRun 'mmlplay --help' for the list of options.\n", what);
    return 2;
}

static void put32(FILE* f, uint32_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); fputc((v >> 16) & 255, f); fputc((v >> 24) & 255, f); }
static void put16(FILE* f, uint16_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }

int main(int argc, char** argv) {
    // ---- defaults ----
    double seconds = 60.0;
    bool timeGiven = false;
    long rate = 44100, volume = 32, mute = 0, fmGain = 2048, totalOffset = 8, noiseSeed = 1, blockFrames = 128, timerPhase = 0;
    int device = -1;                      // -1 = from the file
    bool deviceGiven = false;
    bool seedGiven = false;
    bool dcBlock = false, eqRelease = false, normalize = false, gainGiven = false, eqGiven = false, mimic = false;
    double gainDb = 0;
    std::vector<const char*> files;

    // ---- debug mode and help are looked for first, so the order of the options does not matter ----
    bool debug = false, wantHelp = false;
    for (int i = 1; i < argc; i++) { if (isDebugFlag(argv[i])) debug = true; if (isHelpFlag(argv[i])) wantHelp = true; }
    if (wantHelp) { printHelp(debug); return 0; }

    // ---- parse the command line ----
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char* val = nullptr;
        if (isDebugFlag(a)) continue;
        if (a[0] != '-' || a[1] == '\0') { files.push_back(a); continue; }
        if (!debug) {                      // the technical options are not available without debug mode
            if (!strcmp(a, "-R") || !strcmp(a, "--mimic-release"))
                return usageError("option -R (--mimic-release) is a debug option.\n       Add -d (or --debug) to the command line to use it; 'mmlplay -d --help' lists them.");
            if (!strcmp(a, "-c") || !strcmp(a, "--dc-block"))
                return usageError("option -c (--dc-block) is a debug option.\n       Add -d (or --debug) to the command line to use it; 'mmlplay -d --help' lists them.");
            for (const DebugOption& o : kDebugOptions) {
                int j = i; const char* v = nullptr;
                if (cli::matchOption(argc, argv, j, o.shortName, o.longName, v)) {
                    char msg[300];
                    snprintf(msg, sizeof msg, "option %s (%s) is a debug option.\n       Add -d (or --debug) to the command line to use it; 'mmlplay -d --help' lists them.", o.shortName, o.longName);
                    return usageError(msg);
                }
            }
        }
        #define NEED_VALUE(name) if (!val) return usageError("option " name " needs a value, e.g. " name " 32")
        if (cli::matchOption(argc, argv, i, "-t", "--time", val)) {
            NEED_VALUE("-t");  if (!cli::parseDouble("-t", val, 0.1, 3600, seconds, "seconds of audio", "90")) return 2;
            timeGiven = true;
        } else if (cli::matchOption(argc, argv, i, "-dev", "--device", val)) {
            NEED_VALUE("-dev");
            std::string s = val; for (char& c : s) c = (char)toupper((unsigned char)c);
            if (s == "PC88" || s == "PC-88" || s == "PC8801") device = Mixer::DEVICE_PC88;
            else if (s == "X1") device = Mixer::DEVICE_X1;
            else if (s == "MZ") device = Mixer::DEVICE_MZ;
            else { fprintf(stderr, "error: option -dev: unknown device '%s'.\n       Valid devices are: PC88, X1, MZ.\n", val); return 2; }
        } else if (cli::matchOption(argc, argv, i, "-v", "--volume", val)) {
            NEED_VALUE("-v");  if (!cli::parseInt("-v", val, -200, 512, volume, "master volume, 0 = loud, negative = louder", "32")) return 2;
        } else if (cli::matchOption(argc, argv, i, "-m", "--mute", val)) {
            NEED_VALUE("-m");  if (!cli::parseInt("-m", val, 0, 0x7FF, mute, "bit mask of channels A-K", "0x7")) return 2;
        } else if (cli::matchOption(argc, argv, i, "-r", "--rate", val)) {
            NEED_VALUE("-r");  if (!cli::parseInt("-r", val, 8000, 192000, rate, "sample rate in Hz", "44100")) return 2;
        } else if (cli::matchOption(argc, argv, i, "-g", "--fm-gain", val)) {
            NEED_VALUE("-g");  if (!cli::parseInt("-g", val, 1, 65536, fmGain, "1024 = x1", "2048")) return 2;
        } else if (cli::matchOption(argc, argv, i, "-o", "--total-offset", val)) {
            NEED_VALUE("-o");  if (!cli::parseInt("-o", val, 0, 255, totalOffset, "internal carrier offset", "8")) return 2;
        } else if (cli::matchOption(argc, argv, i, "-s", "--noise-seed", val)) {
            NEED_VALUE("-s");  if (!cli::parseInt("-s", val, 0, 0x7FFFFFFFL, noiseSeed, "random table seed", "1")) return 2;
            seedGiven = true;
        } else if (cli::matchOption(argc, argv, i, "-p", "--phase", val)) {
            NEED_VALUE("-p");  if (!cli::parseInt("-p", val, 0, 1000000, timerPhase, "timer phase", "0")) return 2;
        } else if (cli::matchOption(argc, argv, i, "-b", "--block", val)) {
            NEED_VALUE("-b");  if (!cli::parseInt("-b", val, 64, 65536, blockFrames, "frames per engine call", "1024")) return 2;
        } else if (!strcmp(a, "-R") || !strcmp(a, "--mimic-release")) {
            mimic = true;
        } else if (!strcmp(a, "-c") || !strcmp(a, "--dc-block")) {
            dcBlock = true;
        } else if (cli::matchOption(argc, argv, i, "-G", "--gain", val)) {
            NEED_VALUE("-G");
            std::string w = val; for (char& c : w) c = (char)tolower((unsigned char)c);
            if (w == "normalize" || w == "normalise" || w == "peak") { normalize = true; gainGiven = true; }
            else { if (!cli::parseDouble("-G", val, -60, 60, gainDb, "gain in dB, or the word normalize", "11.7")) return 2; gainGiven = true; }
        } else if (cli::matchOption(argc, argv, i, "-e", "--eq", val)) {
            NEED_VALUE("-e");
            std::string w = val; for (char& c : w) c = (char)tolower((unsigned char)c);
            eqGiven = true;
            if (w == "release") eqRelease = true; else if (w == "none" || w == "off") eqRelease = false;
            else return usageError("option -e: the only equaliser is 'release' (or 'none').");
        } else {
            char msg[300];
            snprintf(msg, sizeof msg, "unknown option '%s'.", a);
            return usageError(msg);
        }
        #undef NEED_VALUE
    }
    if (files.size() < 2) return usageError(files.empty() ? "no input and output file given.\n       Usage: mmlplay INPUT.mus OUTPUT.wav [options]"
                                                          : "no output file given.\n       Usage: mmlplay INPUT.mus OUTPUT.wav [options]");
    if (files.size() > 2) {
        char msg[500];
        std::string extra = files[2]; for (char& c : extra) c = (char)toupper((unsigned char)c);
        const char* hint = (extra == "PC88" || extra == "X1" || extra == "MZ")
            ? "\n       (The device option is now  -dev NAME  and needs debug mode:  -d -dev PC88;  plain -d turns debug mode on.)" : "";
        snprintf(msg, sizeof msg, "unexpected extra argument '%s'.\n       Only INPUT.mus and OUTPUT.wav are positional; "
                                  "options start with '-' (for the length use  -t SECONDS).%s", files[2], hint);
        return usageError(msg);
    }
    const char* inPath = files[0];
    const char* outPath = files[1];

    // ---- load and compile the MML ----
    std::string text;
    if (!cli::readFile(inPath, text)) return 1;
    static Song song;
    Song::defaultTotalOffset = (int)totalOffset;
    if (!compile(text.c_str(), song)) {
        fprintf(stderr, "%s\n", song.describeError(inPath).c_str());
        return 1;
    }
    if (seedGiven) Opx4::setNoiseSeed((uint32_t)noiseSeed);

    // ---- --mimic-release: the settings measured on the public release (explicit -G / -e / -c still win) ----
    // Measured on the release of bgm01_x1: the engine's time zero sits 0.452 s into the file, the fade starts 0.90 s after the end of the first
    // pass and lasts 8.0 s, and 1.95 s of digital silence follow it.  The gain is +12.0 dB with our three-section equaliser (the same level as
    // the release to within 0.1 dB rms; the +11.7 dB measured on the 1/3-octave bands is the gain of the equaliser's pass band).
    const double LEAD_S = 0.452, FADE_DELAY_S = 0.90, FADE_LEN_S = 8.0, END_SILENCE_S = 1.95;
    if (mimic) {
        dcBlock = true;
        if (!eqGiven) eqRelease = true;
        if (!gainGiven) { gainGiven = true; gainDb = 12.0; }
    }

    // ---- the device: from -dev, else from the file ----
    deviceGiven = (device >= 0);
    if (device < 0) device = (song.machine == M_X1) ? Mixer::DEVICE_X1 : (song.machine == M_MZ) ? Mixer::DEVICE_MZ : Mixer::DEVICE_PC88;
    // The sequencer picks FM or PSG voices from the CHIP, not from the file, so a file whose dialect does not match the
    // device plays on the wrong voices (the original plugin does the same).  Tell the user.
    if (song.x1Mode && device != Mixer::DEVICE_X1)
        fprintf(stderr, "warning: %s is written in the X1 dialect (#machine x1, channels A-K) but the device is %s, which has only 3 FM and 3 PSG\n"
                        "         voices: channels D-K will not sound as the file intends.  Use  -dev X1  (or leave -dev out).\n", inPath, device == Mixer::DEVICE_MZ ? "MZ" : "PC88");
    else if (!song.x1Mode && device == Mixer::DEVICE_X1)
        fprintf(stderr, "warning: %s is written in the PC-88 dialect (channels A-F) but the device is X1: channels D-F will play as FM voices\n"
                        "         instead of PSG.  Use  -dev PC88  (or leave -dev out) unless that is what you want.\n", inPath);
    // The MZ chip has no FM sound: in the original, channels A-C (and G-K of the X1 dialect) are silent there; only D-F are heard.
    if (device == Mixer::DEVICE_MZ) {
        std::string silent;
        for (int c = 0; c < 11; c++)
            if ((c < 3 || c > 5) && song.ch[c].totalTicks > 0) { if (!silent.empty()) silent += ", "; silent += (char)('A' + c); }
        if (!silent.empty())
            fprintf(stderr, "warning: %s has music on channel(s) %s, but the MZ device has no FM sound: %s will be silent (the original plugin\n"
                            "         does the same).  Only channels D, E and F (PSG) are heard on this device.\n", inPath, silent.c_str(), silent.find(',') == std::string::npos ? "that channel" : "those channels");
    }

    // ---- how much to render ----
    // Without -t the file holds exactly one pass of the song: the original never stops a looping song by itself, so its length is
    // measured (songlen.cpp) and the render ends where the longest channel reaches its end.  The notes that are still ringing then are
    // allowed to die away through their own release (no fade-out); a looping song would otherwise just start its next round.
    SongTiming timing;
    bool haveTiming = measureSong(song, 0, timing);
    bool autoLength = !timeGiven;
    if (autoLength && (!haveTiming || timing.truncated)) {
        fprintf(stderr, "warning: the length of the song could not be determined; rendering 60 s instead (use -t to choose a time).\n");
        autoLength = false; seconds = 60.0;
    }
    long total = autoLength ? (long)((double)timing.onceMs * (double)rate / 1000.0 + 0.999999) : (long)(seconds * (double)rate + 0.5);
    // release style: the file is  [silence][the song, one pass and on into the next round][fade-out][silence]; the engine runs for all but the lead-in
    long leadFrames = 0, fadeStart = 0, fadeLen = 0;
    if (mimic) {
        autoLength = false;
        leadFrames = (long)(LEAD_S * rate + 0.5); fadeLen = (long)(FADE_LEN_S * rate + 0.5);
        if (!timeGiven) {
            if (haveTiming && !timing.truncated) {
                long passEnd = leadFrames + (long)((double)timing.onceMs * (double)rate / 1000.0 + 0.5);
                fadeStart = passEnd + (long)(FADE_DELAY_S * rate + 0.5);
                total = fadeStart + fadeLen + (long)(END_SILENCE_S * rate + 0.5);
            } else {
                fprintf(stderr, "warning: the length of the song could not be determined; using 60 s (use -t to choose a time).\n");
                total = (long)(60.0 * rate);
                fadeStart = total - fadeLen - (long)(END_SILENCE_S * rate + 0.5);
            }
        } else {
            fadeStart = total - fadeLen - (long)(END_SILENCE_S * rate + 0.5);
            if (fadeStart < leadFrames) fadeStart = leadFrames;
        }
    }
    long engineFrames = total - leadFrames;

    // ---- one fully configured engine with the song playing (made again for the first pass of -G normalize) ----
    int buildExit = 0;
    auto build = [&]() -> Mixer* {
        Mixer* m = nullptr;
        try {
            m = new Mixer((int)rate);
            m->setDevice(device);
            m->chip().setOutputGain((int)fmGain);
            m->setVolume((int)volume);
        } catch (const std::exception& e) { fprintf(stderr, "error: %s\n", e.what()); delete m; buildExit = 2; return nullptr; }
        m->setMute((unsigned)mute);
        if (timerPhase >= m->timerPhaseRange()) { fprintf(stderr, "error: option -p: %ld is too large for this sample rate (allowed 0..%d).\n", timerPhase, m->timerPhaseRange() - 1); delete m; buildExit = 2; return nullptr; }
        m->setTimerPhase((int)timerPhase);
        // The player starts from a clean slot: setDevice wiped every slot, so this file's tones and envelopes are the only ones.
        if (!timeGiven && !mimic) m->setLooping(3, false);   // one pass: every channel stops at the end of its own text (see below)
        if (!m->loadSong(3, 0, text.c_str(), true)) { fprintf(stderr, "error: %s\n", m->lastError().c_str()); delete m; buildExit = 1; return nullptr; }
        return m;
    };

    // ---- render the music (and, for one pass, the release tail); every block goes through `sink` ----
    long tailFrames = 0; bool tailOpen = false; int rawPeak = 0;
    typedef std::function<bool(int16_t*, int)> Sink;
    std::vector<int16_t> block((size_t)2 * (size_t)blockFrames);
    auto renderAll = [&](Mixer* m, const Sink& sink) -> bool {
        rawPeak = 0; tailFrames = 0; tailOpen = false;
        bool ok = true;
        for (long done = 0; done < leadFrames && ok; ) {                       // release style: silence before the song (the engine waits)
            int n = (int)((leadFrames - done) < blockFrames ? (leadFrames - done) : blockFrames);
            std::fill(block.begin(), block.begin() + 2 * n, (int16_t)0);
            ok = sink(block.data(), n);
            done += n;
        }
        for (long done = 0; done < engineFrames && ok; ) {
            int n = (int)((engineFrames - done) < blockFrames ? (engineFrames - done) : blockFrames);
            m->render(block.data(), n);
            for (int i = 0; i < 2 * n; i++) { int a = block[i] < 0 ? -block[i] : block[i]; if (a > rawPeak) rawPeak = a; }
            ok = sink(block.data(), n);
            done += n;
        }
        if (autoLength && ok) {
            // (no cut is needed: with the loop switched off every channel has already stopped by itself and been keyed off at its own end,
            // so the notes that are still ringing just die away through their release; nothing of the next round is ever played)
            std::vector<int16_t> tail;
            // The tail ends where the sound has fallen 60 dB below the loudest point of the song (0.1 % of the peak, but never less than the
            // chip's own constant floor of about 2 steps): from there on nothing can be heard.  Give up after 5 s.  (Judged on the raw
            // samples, before any of the optional processing.)
            const long cap = rate * 5, window = rate / 25;
            const int limit = (int)(rawPeak * 0.002) > 2 ? (int)(rawPeak * 0.002) : 2;       // allowed swing (peak to peak) in the last 40 ms
            for (;;) {
                size_t old = tail.size();
                tail.resize(old + (size_t)2 * (size_t)blockFrames);
                m->render(tail.data() + old, blockFrames);
                long n = (long)(tail.size() / 2);
                if (n >= window) {
                    int lo = 32767, hi = -32768;
                    for (size_t i = tail.size() - (size_t)2 * (size_t)window; i < tail.size(); i++) { if (tail[i] < lo) lo = tail[i]; if (tail[i] > hi) hi = tail[i]; }
                    if (hi - lo <= limit) { tailFrames = n - window; break; }
                }
                if (n >= cap) { tailFrames = n; tailOpen = true; break; }
            }
            for (long i = 0; i < 2 * tailFrames; i++) { int a = tail[i] < 0 ? -tail[i] : tail[i]; if (a > rawPeak) rawPeak = a; }
            for (long done = 0; done < tailFrames && ok; ) {
                int n = (int)((tailFrames - done) < blockFrames ? (tailFrames - done) : blockFrames);
                ok = sink(tail.data() + 2 * done, n);
                done += n;
            }
        }
        return ok;
    };

    // ---- optional processing after the engine (not in the original): DC filter, equaliser, gain ----
    PostChain chain(0 + (int)rate, dcBlock, eqRelease);
    double normFactor = 1.0;
    if (normalize) {                                      // first pass: only measure the loudest sample after the DC filter and equaliser
        Mixer* probeMixer = build();
        if (!probeMixer) return buildExit;
        PostChain probe((int)rate, dcBlock, eqRelease);
        if (mimic) probe.setFade(fadeStart, fadeLen);
        renderAll(probeMixer, [&](int16_t* b, int n) { probe.process(b, n); return true; });
        delete probeMixer;
        double pk = probe.peakBeforeGain();
        normFactor = pk > 0 ? std::floor(32767.0 / pk * 1e6) / 1e6 : 1.0;
        chain.setGain(normFactor);
    } else if (gainGiven) chain.setGain(std::pow(10.0, gainDb / 20.0));
    if (mimic) chain.setFade(fadeStart, fadeLen);

    // ---- render, streaming straight to the file ----
    Mixer* mx = build();
    if (!mx) return buildExit;
    FILE* f = fopen(outPath, "wb");
    if (!f) { fprintf(stderr, "error: cannot create '%s': %s\n", outPath, strerror(errno)); delete mx; return 1; }
    auto writeHeader = [&](long frames) {            // rewritten at the end, when the length of the release tail is known
        uint32_t bytes = (uint32_t)frames * 4u;
        fseek(f, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, f); put32(f, 36 + bytes); fwrite("WAVEfmt ", 1, 8, f); put32(f, 16); put16(f, 1); put16(f, 2);
        put32(f, (uint32_t)rate); put32(f, (uint32_t)rate * 4); put16(f, 4); put16(f, 16); fwrite("data", 1, 4, f); put32(f, bytes);
    };
    writeHeader(total);
    bool writeOk = renderAll(mx, [&](int16_t* b, int n) {
        if (chain.active()) chain.process(b, n);
        return fwrite(b, 4, (size_t)n, f) == (size_t)n;
    });
    if (writeOk && autoLength) writeHeader(total + tailFrames);
    int peak = chain.active() ? chain.peakOut() : rawPeak;
    if (fclose(f) != 0) writeOk = false;
    if (!writeOk) {
        fprintf(stderr, "error: writing '%s' failed (disk full or no permission?).\n", outPath);
        delete mx;
        return 1;
    }

    // ---- summary ----
    static const char* devName[] = {"PC88", "X1", "MZ"};
    printf("%s -> %s\n", inPath, outPath);
    if (!song.titleOf(0).empty()) printf("  title   : %s\n", song.titleOf(0).c_str());
    printf("  device  : %s  (%s)\n", devName[device - 4], deviceGiven ? "chosen with -dev" : song.machine != M_PC88 ? "from #machine" : "default");
    printf("  dialect : %s\n", song.x1Mode ? "X1 (channels A-K)" : "PC-88 (channels A-F)");
    if (haveTiming) {
        const SongTiming& tm = timing;
        printf("  song    : %.1f s played once", tm.onceMs / 1000.0);
        if (tm.loops && tm.uniform) printf("; loops from %.1f s, loop length %.1f s", tm.introMs / 1000.0, tm.loopMs / 1000.0);
        else if (tm.loops)          printf("; loops (channels differ, see mmldump)");
        else                        printf("; does not loop");
        if (tm.fadeEnd) printf("; ends with a fade-out");
        printf("\n");
    }
    if (autoLength) printf("  length  : %.1f s of music + %.2f s release tail = %.1f s at %ld Hz (%ld frames)   (one pass, no fade-out)\n",
                           (double)total / rate, (double)tailFrames / rate, (double)(total + tailFrames) / rate, rate, total + tailFrames);
    else            printf("  length  : %.1f s at %ld Hz (%ld frames)\n", (double)total / rate, rate, total);
    if (tailOpen) printf("  warning : the sound had not settled 5 s after the end of the music; the file ends there.\n");
    printf("  volume  : %ld   peak level: %.0f%% of full scale\n", volume, 100.0 * peak / 32767.0);
    if (chain.active()) {
        printf("  post    :");
        if (dcBlock) printf(" DC filter;");
        if (eqRelease) printf(" equaliser 'release';");
        if (mimic) printf(" release style (%.2f s silence first, fade-out %.1f s from %.1f s, file ends at %.1f s);", (double)leadFrames / rate, (double)fadeLen / rate, (double)fadeStart / rate, (double)total / rate);
        if (normalize) printf(" gain %+.2f dB (normalized to full scale);", 20 * std::log10(normFactor));
        else if (gainGiven) printf(" gain %+.2f dB;", gainDb);
        printf("   (after it: peak level %.0f%% of full scale)\n", 100.0 * peak / 32767.0);
    }
    if (debug) {
        static const int chipMode[] = {0, 4, 5};
        printf("  debug   : chip mode %d, mute 0x%lX, fm-gain %ld, total-offset %ld%s, timer phase %ld, noise seed %ld%s, block %ld frames\n",
               chipMode[device - 4], mute, fmGain, totalOffset, (int)totalOffset == 8 ? " (default)" : "", timerPhase, noiseSeed, seedGiven ? "" : " (default)", blockFrames);
        size_t codeBytes = 0;
        for (int c = 0; c < 11; c++) codeBytes += song.ch[c].code.size();
        printf("  debug   : %lu bytes of compiled code; channels with music:", (unsigned long)codeBytes);
        for (int c = 0; c < 11; c++) if (song.ch[c].totalTicks > 0) printf("  %c=%ld ticks", 'A' + c, (long)song.ch[c].totalTicks);
        printf("\n");
    }
    if (peak == 0) printf("  warning : the output is silent (all channels muted, or the song produced no sound in this time).\n");
    if (mx->clippedSamples() > 0) printf("  warning : %ld samples were clipped - lower the volume with a larger -v value.\n", mx->clippedSamples());
    if (chain.clippedSamples() > 0) printf("  warning : %ld samples were clipped by the gain - use a smaller -G value or -G normalize.\n", chain.clippedSamples());
    delete mx;
    return 0;
}
