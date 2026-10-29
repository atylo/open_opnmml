// lenscript FILE.mus  -  print the song timing the port measures (src: songlen.cpp), one line per channel, for verify/songlen_diff.py.
//   ch <c> <used> <loops> <end> <loopStart> <loopMs> <fade>      (times in ms, -1 = none)
//   song <onceMs> <introMs> <loopMs> <loops> <uniform> <fadeEnd> <truncated>
#include "songlen.hpp"
#include <cstdio>
#include <string>
using namespace opnmml;
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    FILE* f = fopen(argv[1], "rb"); if (!f) return 2;
    std::string text; char b[4096]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) text.append(b, n);
    fclose(f);
    static Song s;
    if (!compile(text.c_str(), s)) { printf("ERR\n"); return 0; }
    SongTiming tm;
    if (!measureSong(s, 0, tm)) { printf("EMPTY\n"); return 0; }
    for (int c = 0; c < 11; c++) { const ChannelTiming& t = tm.ch[c];
        printf("ch %d %d %d %ld %ld %ld %d\n", c, t.used, t.loops, t.endMs, t.loopStartMs, t.loopMs, t.fadeEnded); }
    printf("song %ld %ld %ld %d %d %d %d\n", tm.onceMs, tm.introMs, tm.loopMs, tm.loops, tm.uniform, tm.fadeEnd, tm.truncated);
    return 0;
}
