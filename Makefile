# Build the command-line tools and the interactive player.

CXX      ?= g++
CXXFLAGS ?= -std=c++11 -O2 -Wall -Wextra -s

# Search for source files and headers in src/
VPATH = src

all: mmlplay mmltrace mmldump mmlplayer

# The player uses the Windows multimedia API (winmm).
ifeq ($(OS),Windows_NT)
PLAYER_LIBS = -lwinmm
endif

mmlplay: mmlplay.cpp mixer.cpp opx4.cpp seq.cpp mmlcomp.cpp songlen.cpp post.cpp \
         post.hpp cli.hpp mixer.hpp opx4.hpp seq.hpp mmlcomp.hpp songlen.hpp chiptables.inc tables.inc
	$(CXX) $(CXXFLAGS) -o $@ $(filter %.cpp,$^)

mmltrace: mmltrace.cpp seq.cpp mmlcomp.cpp \
          cli.hpp seq.hpp mmlcomp.hpp tables.inc
	$(CXX) $(CXXFLAGS) -o $@ $(filter %.cpp,$^)

mmldump: mmldump.cpp mmlcomp.cpp seq.cpp songlen.cpp \
         cli.hpp mmlcomp.hpp seq.hpp songlen.hpp tables.inc
	$(CXX) $(CXXFLAGS) -o $@ $(filter %.cpp,$^)

mmlplayer: mmlplayer.cpp player.cpp mixer.cpp opx4.cpp seq.cpp mmlcomp.cpp songlen.cpp \
           player.hpp audio_winmm.hpp screen.hpp terminal_win.hpp \
           mixer.hpp opx4.hpp seq.hpp mmlcomp.hpp songlen.hpp chiptables.inc tables.inc
	$(CXX) $(CXXFLAGS) -o $@ $(filter %.cpp,$^) $(PLAYER_LIBS)

clean:
	rm -f mmlplay mmltrace mmldump mmlplayer mmlplay.exe mmltrace.exe mmldump.exe mmlplayer.exe

.PHONY: all clean
