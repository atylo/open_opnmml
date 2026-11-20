# Build the command-line tools and the interactive player.  In an MSYS2 MinGW64 shell:   make     (or mingw32-make)
# -s strips symbols for a small binary; remove it if you want to debug.
CXX      ?= g++
CXXFLAGS ?= -std=c++11 -O2 -Wall -Wextra -s

all: mmlplay mmltrace mmldump mmlplayer

# the player talks to the sound card through the Windows multimedia API (winmm); nothing is needed on other systems
ifeq ($(OS),Windows_NT)
PLAYER_LIBS = -lwinmm
endif

mmlplay: mmlplay.cpp mixer.cpp opx4.cpp seq.cpp mmlcomp.cpp songlen.cpp post.cpp post.hpp cli.hpp mixer.hpp opx4.hpp seq.hpp mmlcomp.hpp songlen.hpp chiptables.inc tables.inc
	$(CXX) $(CXXFLAGS) -o $@ mmlplay.cpp mixer.cpp opx4.cpp seq.cpp mmlcomp.cpp songlen.cpp post.cpp

mmltrace: mmltrace.cpp seq.cpp mmlcomp.cpp cli.hpp seq.hpp mmlcomp.hpp tables.inc
	$(CXX) $(CXXFLAGS) -o $@ mmltrace.cpp seq.cpp mmlcomp.cpp

mmldump: mmldump.cpp mmlcomp.cpp seq.cpp songlen.cpp cli.hpp mmlcomp.hpp seq.hpp songlen.hpp tables.inc
	$(CXX) $(CXXFLAGS) -o $@ mmldump.cpp mmlcomp.cpp seq.cpp songlen.cpp

mmlplayer: mmlplayer.cpp player.cpp player.hpp audio_winmm.hpp screen.hpp terminal_win.hpp mixer.cpp opx4.cpp seq.cpp mmlcomp.cpp songlen.cpp mixer.hpp opx4.hpp seq.hpp mmlcomp.hpp songlen.hpp chiptables.inc tables.inc
	$(CXX) $(CXXFLAGS) -o $@ mmlplayer.cpp player.cpp mixer.cpp opx4.cpp seq.cpp mmlcomp.cpp songlen.cpp $(PLAYER_LIBS)

clean:
	rm -f mmlplay mmltrace mmldump mmlplayer mmlplay.exe mmltrace.exe mmldump.exe mmlplayer.exe
.PHONY: all clean
