#!/bin/sh
# Builds and runs verify/player_test.cpp (the interactive player's self-test), then the mutation tests.
#
#   sh verify/player_test.sh                 build, run the test, then break the player 14 ways: every break must be caught
#   sh verify/player_test.sh --no-mutants    only the test
#   SANITIZE=1 sh verify/player_test.sh      also build with -fsanitize=address,undefined (Linux; not available in MSYS2)
#   CXX=x86_64-w64-mingw32-g++ ...           another compiler
#
# No sound card and no original DLL needed.  A mutant is "caught" when the test fails or crashes on it.
cd "$(dirname "$0")" || exit 1
CXX=${CXX:-g++}
FLAGS="-std=c++11 -O1 -g -Wall -Wextra -I.."
[ -n "$SANITIZE" ] && FLAGS="$FLAGS -fsanitize=address,undefined -fno-sanitize-recover=undefined"
mkdir -p obj || exit 1

for f in mixer opx4 seq mmlcomp songlen; do
    $CXX $FLAGS -c ../$f.cpp -o obj/$f.o || exit 1
done
$CXX $FLAGS -c player_test.cpp -o obj/test.o || exit 1
ENGINE="obj/mixer.o obj/opx4.o obj/seq.o obj/mmlcomp.o obj/songlen.o obj/test.o"

echo "== the player as it is"
$CXX $FLAGS -c ../player.cpp -o obj/player.o && $CXX $FLAGS -o obj/player_test obj/player.o $ENGINE || exit 1
timeout 600 ./obj/player_test .. >obj/real.log 2>&1; rc=$?
tail -3 obj/real.log
[ "$rc" = 0 ] || { echo "THE TEST FAILS ON THE REAL PLAYER:"; grep FAIL obj/real.log; exit 1; }
[ "$1" = "--no-mutants" ] && exit 0

echo
echo "== mutation tests (each line breaks player.cpp in one way; the test must notice)"
missed=0; total=0
mutant() {   # name, sed expression
    total=$((total + 1))
    sed "$2" ../player.cpp > obj/player_mut.cpp
    if cmp -s ../player.cpp obj/player_mut.cpp; then echo "  ?? $1: the edit did not apply (the player source changed?)"; missed=$((missed + 1)); return; fi
    if ! $CXX $FLAGS -c obj/player_mut.cpp -o obj/player_mut.o 2>obj/mut_build.log; then echo "  ?? $1: does not compile"; missed=$((missed + 1)); return; fi
    $CXX $FLAGS -o obj/player_mut obj/player_mut.o $ENGINE || { missed=$((missed + 1)); return; }
    if timeout 300 ./obj/player_mut .. >obj/mut.log 2>&1; then
        echo "  NOT CAUGHT  $1"; missed=$((missed + 1))
    else
        n=$(grep -c FAIL obj/mut.log); echo "  caught      $1   ($n failed checks$( [ "$n" = 0 ] && echo ', crashed or hung'))"
    fi
}
mutant "pause does nothing"                         's/if (isPaused || st == IDLE || !mx)/if (st == IDLE || !mx)/'
mutant "unused part of a block is thrown away"      's/if (carryPos >= carryLen) { renderBlock(); continue; }/if (carryPos >= carryLen || carryPos > 0) { renderBlock(); continue; }/'
mutant "volume step 3 instead of 4"                 's/const int kVolumeStep = 4;/const int kVolumeStep = 3;/'
mutant "stop cuts instead of the quick fade"        's/mx->stopSlot(kSlot)/mx->cutSlot(kSlot)/g'
mutant "a channel cannot be switched on again"      's/mute ^= 1u << ch;/mute |= 1u << ch;/'
mutant "no automatic advance at the end of a song"  's/autoAdvance = (st == PLAYING);/autoAdvance = false;/'
mutant "previous does not wrap round"               's/goTo(((base + dir) % n + n) % n);/goTo((base + dir) % n < 0 ? 0 : (base + dir) % n);/'
mutant "--once is ignored"                          's/if (opt.once) m->setLooping(kSlot, false);//'
mutant "folder files sorted case-sensitively"       's/return la != lb ? la < lb : a < b;/return a < b;/'
mutant "volume has no lower limit"                  's/vol = std::max(-200, vol - kVolumeStep);/vol = vol - kVolumeStep;/'
mutant "n twice does not skip a file"               's/int base = pendingIdx >= 0 ? pendingIdx : cur;/int base = cur;/'
mutant "a long name is not shortened"               's/if (budget < 4) budget = 4;/budget = 100;/'
mutant "a bad file stops the search"                's/if (loadTrack(i)) return;/if (loadTrack(i)) return; else break;/'
mutant "s during the tail does not cancel advance"  's/else if (st == STOPPING || st == TAIL) { pendingIdx = -1; autoAdvance = false; isPaused = false; }/else if (st == STOPPING || st == TAIL) { pendingIdx = -1; isPaused = false; }/'
echo
if [ "$missed" = 0 ]; then echo "all $total mutants caught"; else echo "$missed of $total mutants NOT caught"; exit 1; fi
