# Build the three command-line tools.  In an MSYS2 MinGW64 shell:   make     (or mingw32-make)
# -s strips symbols for a small binary; remove it if you want to debug.
CXX      ?= g++
CXXFLAGS ?= -std=c++11 -O2 -Wall -Wextra -s

all: mmlplay mmltrace mmldump

mmlplay: mmlplay.cpp mixer.cpp opx4.cpp seq.cpp mmlcomp.cpp cli.hpp mixer.hpp opx4.hpp seq.hpp mmlcomp.hpp chiptables.inc tables.inc
	$(CXX) $(CXXFLAGS) -o $@ mmlplay.cpp mixer.cpp opx4.cpp seq.cpp mmlcomp.cpp

mmltrace: mmltrace.cpp seq.cpp mmlcomp.cpp cli.hpp seq.hpp mmlcomp.hpp tables.inc
	$(CXX) $(CXXFLAGS) -o $@ mmltrace.cpp seq.cpp mmlcomp.cpp

mmldump: mmldump.cpp mmlcomp.cpp cli.hpp mmlcomp.hpp
	$(CXX) $(CXXFLAGS) -o $@ mmldump.cpp mmlcomp.cpp

clean:
	rm -f mmlplay mmltrace mmldump mmlplay.exe mmltrace.exe mmldump.exe
.PHONY: all clean
