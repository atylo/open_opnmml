// ================================================================================================================
// terminal_win.hpp - the Windows console as a Terminal for ScreenBlock (screen.hpp).  Plain Win32 console calls: no escape sequences, so it
// also works in a console that has not got "virtual terminal processing" switched on, and no extra libraries.
//
// Text goes out through stdout; before the cursor is moved the buffered text is flushed.  The cursor is moved RELATIVE to where it is now
// (GetConsoleScreenBufferInfo says where), so it does not matter that the window has scrolled since the block was drawn.  When the output is
// not a console (redirected to a file or a pipe) the calls fail and "up" is just a carriage return.
// ================================================================================================================
#pragma once
#ifdef _WIN32
#include "screen.hpp"
#include <cstdio>
#include <windows.h>

namespace opnmml {

class ConsoleTerminal : public Terminal {
public:
    ConsoleTerminal() : out(GetStdHandle(STD_OUTPUT_HANDLE)) {}

    int width() override {
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (!GetConsoleScreenBufferInfo(out, &info)) return 80;
        int w = info.srWindow.Right - info.srWindow.Left + 1;
        return w < 2 ? 2 : w;
    }
    void write(const std::string& text) override { fputs(text.c_str(), stdout); }
    void newline() override { fputc('\n', stdout); }
    void up(int rows) override {
        fflush(stdout);
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (!GetConsoleScreenBufferInfo(out, &info)) { fputc('\r', stdout); return; }
        COORD pos;
        pos.X = 0;
        pos.Y = (SHORT)(info.dwCursorPosition.Y - rows);
        if (pos.Y < 0) pos.Y = 0;
        SetConsoleCursorPosition(out, pos);
    }
    void flush() override { fflush(stdout); }

private:
    HANDLE out;
};

}  // namespace opnmml
#endif
