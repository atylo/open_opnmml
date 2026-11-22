// mmlplayer - interactive player for .mus music files (open-source port of a game music plugin).
// The playlist, the keys and the engine are in player.cpp; this file is the command line, the keyboard and the screen.
#include "player.hpp"
#include "screen.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#ifdef _WIN32
#include "audio_winmm.hpp"
#include "terminal_win.hpp"
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
"  PATH is a .mus file, or a folder: every .mus file in it and in all the folders below it is played (the files of a folder\n"
"  first, then its sub-folders, everything in the order of the names).  The files are played in the order given.\n"
"  The machine (PC-88, X1, MZ) is taken from the file's '#machine' line.\n"
"\n"
"OPTIONS\n"
"  -1, --once   start in 'once' mode: every song plays once and then the next file follows.  Default: 'loop', songs repeat\n"
"               for ever, like in the game (a song without a loop point still ends by itself and the next file follows).\n"
"               The l key switches between the two while playing.\n"
"  -h, --help   show this text\n"
"\n"
"THE SCREEN\n"
"  Three lines that are updated in place: the title, the file name with the machine and the place in the list, and\n"
"      playing  0:08 / 1:17  loop  vol 32  tempo 76  ---DEF  h = controls\n"
"  = state, time / length of one pass, loop or once, volume, tempo, and the channels: the letter of a channel that plays,\n"
"  '-' for one without music (or finished), '.' for one that you switched off.  The h key shows / hides the list of the keys.\n"
"\n"
"KEYS WHILE PLAYING\n"
"  space          pause / continue (after a stop: play again)     s   stop, with the game's quick fade (about 0.26 s)\n"
"  Left / Right   previous / next file (wraps round)              l   switch between loop and once\n"
"  Up / Down      louder / quieter (about 0.9 dB per press)       [ / ]   slower / faster by 5 beats per minute\n"
"  1 2 ... 9 0 K  switch channel A ... K off / on (1 = A, 2 = B ... 9 = I, 0 = J, K = K); all are on again with the next file\n"
"  h              show / hide the keys                            q   quit (Esc also works)\n"
"\n"
"EXAMPLES\n"
"  mmlplayer bgm01_opm.mus\n"
"  mmlplayer bgm                  (every .mus file in the folder bgm and below)\n"
"  mmlplayer --once bgm/chama\n";

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

    opnmml::ConsoleTerminal term;
    opnmml::ScreenBlock screen(term);
    std::vector<std::string> shown;                                  // the lines that are on the screen now
    DWORD lastDraw = 0;
    // messages (an error, a warning, the end of the playlist) are printed as ordinary lines; the block is drawn again below them
    auto notices = [&]() {
        std::vector<std::string> m = pl.takeMessages();
        if (m.empty()) return;
        screen.printAbove(m);
        shown.clear();
    };

    bool started = pl.begin();
    notices();
    if (!started) { out.close(); return 1; }

    for (;;) {
        while (_kbhit()) {
            int c = _getch();
            if (c == 0 || c == 0xE0) {                              // arrow and function keys come as two codes
                int c2 = _getch();
                if (c2 == 75) pl.key(opnmml::Player::kKeyLeft);
                else if (c2 == 77) pl.key(opnmml::Player::kKeyRight);
                else if (c2 == 72) pl.key(opnmml::Player::kKeyUp);
                else if (c2 == 80) pl.key(opnmml::Player::kKeyDown);
                continue;                                           // the other special keys do nothing
            }
            pl.key(c);
        }
        pl.pump(out, out.chunkFrames());
        notices();
        bool done = pl.wantsQuit() || pl.finishedPlaylist();
        std::vector<std::string> lines = pl.screenLines(term.width());
        DWORD now = GetTickCount();
        if (lines != shown && (done || shown.empty() || now - lastDraw >= 40)) {
            screen.draw(lines);
            shown = lines;
            lastDraw = now;
        }
        if (done) break;
        Sleep(5);
    }
    out.close();
    screen.finish();
    return 0;
#endif
}
