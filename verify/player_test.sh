#!/bin/sh
# Builds and runs verify/player_test.cpp (the interactive player's self-test), then the mutation tests.
#
#   sh verify/player_test.sh                 build, run the test (also as a 32-bit program), then break the player and the screen code in many
#                                            ways: every break must be caught
#   sh verify/player_test.sh --no-mutants    only the tests
#   SANITIZE=1 sh verify/player_test.sh      also build the 64-bit test with -fsanitize=address,undefined (Linux; not available in MSYS2)
#   M32=0 sh verify/player_test.sh           skip the 32-bit run (it needs a compiler that has -m32, e.g. g++-multilib)
#   CXX=x86_64-w64-mingw32-g++ ...           another compiler
#
# The 32-bit run matters: on Windows a C `long` has 32 bits (on Linux 64), and a calculation that only fits in 64 bits (frames * 1000 for the
# clock) goes wrong after 48 seconds there.  A 32-bit build has the same 32-bit long, so it finds such mistakes on Linux too.
# No sound card and no original DLL needed.  A mutant is "caught" when the test fails or crashes on it.
cd "$(dirname "$0")" || exit 1
CXX=${CXX:-g++}
FLAGS="-std=c++11 -O1 -g -Wall -Wextra -I.."
[ -n "$SANITIZE" ] && FLAGS="$FLAGS -fsanitize=address,undefined -fno-sanitize-recover=undefined"
mkdir -p obj obj/ms || exit 1

SRCS="mixer opx4 seq mmlcomp songlen"
for f in $SRCS; do $CXX $FLAGS -c ../$f.cpp -o obj/$f.o || exit 1; done
$CXX $FLAGS -c player_test.cpp -o obj/test.o || exit 1
ENGINE="obj/mixer.o obj/opx4.o obj/seq.o obj/mmlcomp.o obj/songlen.o"

echo "== the player as it is"
$CXX $FLAGS -c ../player.cpp -o obj/player.o && $CXX $FLAGS -o obj/player_test obj/player.o $ENGINE obj/test.o || exit 1
timeout 600 ./obj/player_test .. >obj/real.log 2>&1; rc=$?
tail -3 obj/real.log
[ "$rc" = 0 ] || { echo "THE TEST FAILS ON THE REAL PLAYER:"; grep FAIL obj/real.log; exit 1; }

# ---- the same test as a 32-bit program (long = 32 bits, like Windows) --------------------------------------------------------------------------
HAVE32=0
if [ "${M32:-1}" = 1 ] && echo 'int main(){return 0;}' | $CXX -m32 -x c++ - -o obj/probe32 2>/dev/null; then HAVE32=1; fi
F32="-std=c++11 -O1 -g -Wall -Wextra -I.. -m32"
if [ "$HAVE32" = 1 ]; then
    echo
    echo "== the player as a 32-bit program (long = 32 bits, like Windows)"
    mkdir -p obj32
    for f in $SRCS player; do $CXX $F32 -c ../$f.cpp -o obj32/$f.o || exit 1; done
    $CXX $F32 -c player_test.cpp -o obj32/test.o && $CXX $F32 -o obj32/player_test obj32/*.o || exit 1
    timeout 600 ./obj32/player_test .. >obj32/real.log 2>&1; rc=$?
    tail -3 obj32/real.log
    [ "$rc" = 0 ] || { echo "THE 32-BIT TEST FAILS:"; grep FAIL obj32/real.log; exit 1; }
else
    echo; echo "(no 32-bit compiler here: the 32-bit run is skipped)"
fi
[ "$1" = "--no-mutants" ] && exit 0

missed=0; total=0
report() {   # name, exit status of the mutant's test run, log
    if [ "$2" = 0 ]; then echo "  NOT CAUGHT  $1"; missed=$((missed + 1))
    else n=$(grep -c FAIL "$3"); echo "  caught      $1   ($n failed checks$( [ "$n" = 0 ] && echo ', crashed or hung'))"; fi
}
mutant() {   # name, sed expression for player.cpp
    total=$((total + 1))
    sed "$2" ../player.cpp > obj/player_mut.cpp
    if cmp -s ../player.cpp obj/player_mut.cpp; then echo "  ?? $1: the edit did not apply (the player source changed?)"; missed=$((missed + 1)); return; fi
    if ! $CXX $FLAGS -c obj/player_mut.cpp -o obj/player_mut.o 2>obj/mut_build.log; then echo "  ?? $1: does not compile"; missed=$((missed + 1)); return; fi
    $CXX $FLAGS -o obj/player_mut obj/player_mut.o $ENGINE obj/test.o || { missed=$((missed + 1)); return; }
    timeout 300 ./obj/player_mut .. >obj/mut.log 2>&1; report "$1" $? obj/mut.log
}
mutant32() { # name, sed expression for player.cpp: only a 32-bit program can see this one
    [ "$HAVE32" = 1 ] || return
    total=$((total + 1))
    sed "$2" ../player.cpp > obj/player_mut.cpp
    if cmp -s ../player.cpp obj/player_mut.cpp; then echo "  ?? $1: the edit did not apply"; missed=$((missed + 1)); return; fi
    $CXX $F32 -c obj/player_mut.cpp -o obj32/player_mut.o 2>obj/mut_build.log || { echo "  ?? $1: does not compile"; missed=$((missed + 1)); return; }
    $CXX $F32 -o obj32/player_mut obj32/player_mut.o obj32/mixer.o obj32/opx4.o obj32/seq.o obj32/mmlcomp.o obj32/songlen.o obj32/test.o || { missed=$((missed + 1)); return; }
    timeout 300 ./obj32/player_mut .. >obj/mut.log 2>&1; report "$1 (32-bit run)" $? obj/mut.log
}
smutant() {  # name, sed expression for screen.hpp
    total=$((total + 1))
    sed "$2" ../screen.hpp > obj/ms/screen.hpp
    if cmp -s ../screen.hpp obj/ms/screen.hpp; then echo "  ?? $1: the edit did not apply (screen.hpp changed?)"; missed=$((missed + 1)); return; fi
    if ! $CXX -Iobj/ms $FLAGS -c player_test.cpp -o obj/test_mut.o 2>obj/mut_build.log; then echo "  ?? $1: does not compile"; missed=$((missed + 1)); return; fi
    $CXX $FLAGS -o obj/player_mut obj/player.o $ENGINE obj/test_mut.o || { missed=$((missed + 1)); return; }
    timeout 300 ./obj/player_mut .. >obj/mut.log 2>&1; report "$1" $? obj/mut.log
}

echo
echo "== mutation tests (each line breaks player.cpp or screen.hpp in one way; the test must notice)"
mutant "pause does nothing"                         's/if (isPaused || st == IDLE || !mx)/if (st == IDLE || !mx)/'
mutant "unused part of a block is thrown away"      's/if (carryPos >= carryLen) { renderBlock(); continue; }/if (carryPos >= carryLen || carryPos > 0) { renderBlock(); continue; }/'
mutant "volume step 3 instead of 4"                 's/const int kVolumeStep = 4;/const int kVolumeStep = 3;/'
mutant "volume has no lower limit"                  's/vol = std::max(-200, vol - kVolumeStep);/vol = vol - kVolumeStep;/'
mutant "Up and Down swapped"                        's/} else if (c == kKeyUp) {/} else if (c == kKeyDown) {/'
mutant "+ still changes the volume"                 's/} else if (c == kKeyUp) {/} else if (c == kKeyUp || c == 43) {/'
mutant "the clock is calculated in 32 bits"         's/mmss(delivered \* 1000 \/ opt.rate)/mmss((int)delivered * 1000 \/ opt.rate)/'
mutant32 "the clock is calculated in a long"        's/mmss(delivered \* 1000 \/ opt.rate)/mmss((long)delivered * 1000L \/ opt.rate)/'
mutant "stop cuts instead of the quick fade"        's/mx->stopSlot(kSlot)/mx->cutSlot(kSlot)/g'
mutant "a channel cannot be switched on again"      's/mute ^= 1u << ch;/mute |= 1u << ch;/'
mutant "key 0 is channel 1 instead of 10"           "s/if (c == '0') return 9;/if (c == '0') return 0;/"
mutant "lower-case k is not channel 11"             "s/if (c == 'k' || c == 'K') return 10;/if (c == 'K') return 10;/"
mutant "a key for a missing channel mutes anyway"   's/if (ch < nChannels()) {/if (true) {/'
mutant "the status line shows the keys, not letters" 's/kChannelLetters\[i\] : .-.;/"1234567890K"[i] : 45;/'
mutant "help is on ? instead of h"                  "s/c == 'h' || c == 'H'/c == '?'/"
mutant "the help never hides"                       's/showHelp = !showHelp;/showHelp = true;/'
mutant "no automatic advance at the end of a song"  's/autoAdvance = (st == PLAYING);/autoAdvance = false;/'
mutant "Left does not wrap round"                   's/goTo(((base + dir) % n + n) % n);/goTo((base + dir) % n < 0 ? 0 : (base + dir) % n);/'
mutant "Right twice does not skip a file"           's/int base = pendingIdx >= 0 ? pendingIdx : cur;/int base = cur;/'
mutant "the start mode once is ignored"             's/if (once) m->setLooping(kSlot, false);//'
mutant "l does not change the running song"         's/mx->setLooping(kSlot, !once);//'
mutant "a bad file stops the search"                's/if (loadTrack(i)) return;/if (loadTrack(i)) return; else break;/'
mutant "s during the tail does not cancel advance"  's/else if (st == STOPPING || st == TAIL) { pendingIdx = -1; autoAdvance = false; isPaused = false; }/else if (st == STOPPING || st == TAIL) { pendingIdx = -1; isPaused = false; }/'
mutant "the status line shows a fading state"       's/if ((st == STOPPING || st == TAIL) \&\& pendingIdx < 0 \&\& !autoAdvance) return "stopped";/if (st == STOPPING) return "fading";/'
mutant "the help hint is never dropped"             's/if ((int)(line.size() + strlen(hint)) <= width - 1) line += hint;/line += hint;/'
mutant "the header shows the whole path"            's/infoText = fileName(path) + "   " + deviceName;/infoText = path + "   " + deviceName;/'
mutant "the header always has a track number"       's/if (files.size() > 1) {/if (true) {/'
mutant "the title is not shown"                     's/titleText = mx->songTitle(kSlot, 0);/titleText = "";/'
mutant "folder files sorted case-sensitively"       's/return la != lb ? la < lb : a < b;/return a < b;/'
mutant "sub-folders are not searched"               's/collectFolder(joinPath(dir, n), depth + 1, out, tooDeep);//'
mutant "no limit to the folder depth"               's/if (depth + 1 > kMaxDepth) { tooDeep = true; continue; }//'
mutant "links to folders are followed"              's/if (isFolder(full)) { if (!isLink(full)) folderNames.push_back(n); }/if (isFolder(full)) { folderNames.push_back(n); }/'
smutant "screen: redraw adds lines instead of overwriting" '0,/if (drawn > 0) term.up(drawn - 1);/s//if (false) term.up(drawn - 1);/'
smutant "screen: lines that are no longer used stay"       's/if (drawn > n) {/if (false) {/'
smutant "screen: the cursor is not brought back after wiping" 's/term.up(drawn - n);/term.up(0);/'
smutant "screen: a shorter text leaves the old one behind" "s/r.resize((size_t)w, ' ');//"
smutant "screen: a long line is not cut"                   's/std::string r = s.substr(0, (size_t)w);/std::string r = s;/;s/r.resize((size_t)w, . .);/if (r.size() < (size_t)w) r.resize((size_t)w, 32);/'
smutant "screen: lines may use the last column"            's/int w = term.width() - 1;/int w = term.width();/'
smutant "screen: printAbove does not erase the block"      's/for (int i = 0; i < drawn; i++) { term.write(blank(w));/for (int i = 0; i < 0; i++) { term.write(blank(w));/'
smutant "screen: finish does not start a new line"         's/if (drawn > 0) { term.newline(); drawn = 0; }/if (drawn > 0) { drawn = 0; }/'
echo
if [ "$missed" = 0 ]; then echo "all $total mutants caught"; else echo "$missed of $total mutants NOT caught"; exit 1; fi
