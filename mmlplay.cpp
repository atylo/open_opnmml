// ================================================================================================================
// mmlplay - render a .mus MML file to a 16-bit stereo WAV file with the ported engine.
//
// Build (MSYS2 MinGW64):   g++ -O2 -s -o mmlplay.exe mmlplay.cpp mixer.cpp opx4.cpp seq.cpp mmlcomp.cpp
// Run "mmlplay --help" for the options.
// ================================================================================================================
#include "cli.hpp"
#include "mixer.hpp"
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
"  -t, --time SECONDS     how many seconds of audio to render, 0.1 to 3600.        Default: 60\n"
"                         (songs loop forever, so this is how long the file will be)\n"
"  -v, --volume N         master volume, 0 (loudest) to 512; every +27 halves the level.\n"
"                         Default: 32 (what the game itself uses)\n"
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
"  -s, --noise-seed N     seed of the random table behind noise and the random LFO.  Default: 1 (MSVC rand())\n"
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
    {"-o", "--total-offset"}, {"-p", "--phase"}, {"-s", "--noise-seed"}, {"-b", "--block"}};

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
    long rate = 44100, volume = 32, mute = 0, fmGain = 2048, totalOffset = 8, noiseSeed = 1, blockFrames = 128, timerPhase = 0;
    int device = -1;                      // -1 = from the file
    bool deviceGiven = false;
    bool seedGiven = false;
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
        } else if (cli::matchOption(argc, argv, i, "-dev", "--device", val)) {
            NEED_VALUE("-dev");
            std::string s = val; for (char& c : s) c = (char)toupper((unsigned char)c);
            if (s == "PC88" || s == "PC-88" || s == "PC8801") device = Mixer::DEVICE_PC88;
            else if (s == "X1") device = Mixer::DEVICE_X1;
            else if (s == "MZ") device = Mixer::DEVICE_MZ;
            else { fprintf(stderr, "error: option -dev: unknown device '%s'.\n       Valid devices are: PC88, X1, MZ.\n", val); return 2; }
        } else if (cli::matchOption(argc, argv, i, "-v", "--volume", val)) {
            NEED_VALUE("-v");  if (!cli::parseInt("-v", val, 0, 512, volume, "master volume, 0 = loudest", "32")) return 2;
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

    // ---- set up the engine ----
    Mixer* mx = nullptr;
    try {
        mx = new Mixer((int)rate);
        deviceGiven = (device >= 0);
        if (device < 0) device = (song.machine == M_X1) ? Mixer::DEVICE_X1 : (song.machine == M_MZ) ? Mixer::DEVICE_MZ : Mixer::DEVICE_PC88;
        mx->setDevice(device);
        mx->chip().setOutputGain((int)fmGain);
        mx->setVolume((int)volume);
    } catch (const std::exception& e) {
        fprintf(stderr, "error: %s\n", e.what());
        delete mx;
        return 2;
    }
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
    mx->setMute((unsigned)mute);
    if (timerPhase >= mx->timerPhaseRange()) { fprintf(stderr, "error: option -p: %ld is too large for this sample rate (allowed 0..%d).\n", timerPhase, mx->timerPhaseRange() - 1); delete mx; return 2; }
    mx->setTimerPhase((int)timerPhase);
    // The player starts from a clean slot: setDevice wiped every slot, so this file's tones and envelopes are the only ones.
    if (!mx->loadSong(3, 0, text.c_str(), true)) { fprintf(stderr, "error: %s\n", mx->lastError().c_str()); delete mx; return 1; }

    // ---- render, streaming straight to the file ----
    long total = (long)(seconds * (double)rate + 0.5);
    FILE* f = fopen(outPath, "wb");
    if (!f) { fprintf(stderr, "error: cannot create '%s': %s\n", outPath, strerror(errno)); delete mx; return 1; }
    uint32_t bytes = (uint32_t)total * 4u;
    fwrite("RIFF", 1, 4, f); put32(f, 36 + bytes); fwrite("WAVEfmt ", 1, 8, f); put32(f, 16); put16(f, 1); put16(f, 2);
    put32(f, (uint32_t)rate); put32(f, (uint32_t)rate * 4); put16(f, 4); put16(f, 16); fwrite("data", 1, 4, f); put32(f, bytes);
    std::vector<int16_t> block((size_t)2 * (size_t)blockFrames);
    int peak = 0;
    bool writeOk = true;
    for (long done = 0; done < total && writeOk; ) {
        int n = (int)((total - done) < blockFrames ? (total - done) : blockFrames);
        mx->render(block.data(), n);
        for (int i = 0; i < 2 * n; i++) { int a = block[i] < 0 ? -block[i] : block[i]; if (a > peak) peak = a; }
        writeOk = fwrite(block.data(), 4, (size_t)n, f) == (size_t)n;
        done += n;
    }
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
    printf("  length  : %.1f s at %ld Hz (%ld frames)\n", seconds, rate, total);
    printf("  volume  : %ld   peak level: %.0f%% of full scale\n", volume, 100.0 * peak / 32767.0);
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
    delete mx;
    return 0;
}
