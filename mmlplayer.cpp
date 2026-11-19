// mmlplayer - interactive player for .mus music files (open-source port of a game music plugin).
// The playlist, the keys and the engine are in player.cpp; this file is the command line, the keyboard and the screen.
#include "player.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#ifdef _WIN32
#include "audio_winmm.hpp"
#include <conio.h>
#include <windows.h>
#endif

static const char* kHelp =
"mmlplayer - play .mus music files, with the keyboard to control them\n"
"             (open-source port of a game music plugin)\n"
"\n"
"USAGE\n"
"  mmlplayer [options] PATH [PATH ...]\n"
"\n"
"  PATH is a .mus file, or a folder: every .mus file in it (not in its sub-folders) is played, in the order of their names.\n"
"  The files are played in the order given.  The machine (PC-88, X1, MZ) is taken from the file's '#machine' line.\n"
"\n"
"OPTIONS\n"
"  -1, --once   play every song once and go on to the next file.  Default: songs loop for ever, like in the game\n"
"               (a song without a loop point still ends by itself and the player goes on to the next file).\n"
"  -h, --help   show this text\n"
"\n"
"KEYS WHILE PLAYING\n"
"  space   pause / continue (after a stop: play again)     s   stop, with the game's quick fade (about 0.26 s)\n"
"  n / p   next / previous file                            q   quit (Esc also works)\n"
"  + / -   louder / quieter (about 0.9 dB per press)       [ / ]   slower / faster by 5 beats per minute\n"
"  A..K    switch channel A..K off / on (all channels are on again with the next file)\n"
"  ?       list the keys on the screen\n"
"\n"
"EXAMPLES\n"
"  mmlplayer bgm01_opm.mus\n"
"  mmlplayer bgm                  (every .mus file of the folder bgm)\n"
"  mmlplayer --once bgm/chama\n";

#ifdef _WIN32
static const int kWidth = 79;       // the status line is padded to this many characters (a default console is 80 wide)

static void clearLine() { printf("\r%*s\r", kWidth, ""); }

// print the player's messages above the status line
static bool showMessages(opnmml::Player& pl) {
    std::vector<std::string> m = pl.takeMessages();
    if (m.empty()) return false;
    clearLine();
    for (size_t i = 0; i < m.size(); i++) printf("%s\n", m[i].c_str());
    fflush(stdout);
    return true;
}
#endif

int main(int argc, char** argv) {
    bool once = false;
    std::vector<std::string> paths;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help") || !strcmp(a, "-?")) { fputs(kHelp, stdout); return 0; }
        else if (!strcmp(a, "-1") || !strcmp(a, "--once")) once = true;
        else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "error: unknown option '%s'.\n       Options: -1 / --once, -h / --help.  (A file name that starts with '-' needs a path in front, e.g. ./%s)\n", a, a);
            return 2;
        } else paths.push_back(a);
    }
    if (paths.empty()) {
        fprintf(stderr, "error: no file or folder given.\n       Usage: mmlplayer [options] PATH [PATH ...]    (mmlplayer --help for the keys)\n");
        return 2;
    }

#ifndef _WIN32
    (void)once;
    fprintf(stderr, "error: this program plays through the Windows sound system (WinMM) and is only built for Windows.\n"
                    "       To turn a file into a WAV on other systems use mmlplay.\n");
    return 1;
#else
    opnmml::Player::Options opt;
    opt.once = once;
    opnmml::Player pl(opt);
    for (size_t i = 0; i < paths.size(); i++) pl.addPath(paths[i]);
    {
        std::vector<std::string> m = pl.takeMessages();             // errors about paths that do not exist or have no .mus files
        for (size_t i = 0; i < m.size(); i++) fprintf(stderr, "%s\n", m[i].c_str());
    }
    if (pl.trackCount() == 0) { fprintf(stderr, "error: nothing to play.\n"); return 1; }

    opnmml::WinmmOut out;
    std::string err;
    if (!out.open(opt.rate, err)) { fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    bool started = pl.begin();
    showMessages(pl);
    if (!started) { out.close(); return 1; }
    printf("space = pause   s = stop   n / p = next / previous   + / - = volume   [ / ] = tempo   A..K = channels   ? = all keys   q = quit\n");

    std::string lastStatus;
    DWORD lastDraw = 0;
    for (;;) {
        while (_kbhit()) {
            int c = _getch();
            if (c == 0 || c == 0xE0) { _getch(); continue; }        // arrow and function keys come as two codes: ignore them
            pl.key(c);
        }
        pl.pump(out, out.chunkFrames());
        bool printed = showMessages(pl);
        if (printed) lastStatus.clear();
        if (pl.wantsQuit() || pl.finishedPlaylist()) break;
        DWORD now = GetTickCount();
        std::string s = pl.statusLine(kWidth);
        if (s != lastStatus && (printed || now - lastDraw >= 100 || lastStatus.empty())) {
            printf("\r%-*s", kWidth - 1, s.c_str());
            fflush(stdout);
            lastStatus = s;
            lastDraw = now;
        }
        Sleep(5);
    }
    out.close();
    printf("\n");
    return 0;
#endif
}
