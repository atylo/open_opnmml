// ================================================================================================================
// screen.hpp - a block of text lines that is drawn once and then redrawn IN PLACE (not part of the original plugin).
//
// The player's screen is a few lines (title, file, status line, and the list of the keys while it is switched on).  Instead of printing a new
// set of lines every time something changes, the block moves the cursor back to its first line and writes the new text over the old.  Only
// real messages (an error, a warning) are printed as ordinary lines: printAbove() erases the block, prints them, and the next draw() puts
// the block below them.
//
// The block only needs four things from the terminal (see Terminal), and it only moves the cursor RELATIVE to where it is, so it keeps working
// when the window scrolls.  The Windows version of Terminal is in terminal_win.hpp; the tests use a simulated console with scrolling.
// ================================================================================================================
#pragma once
#include <string>
#include <vector>

namespace opnmml {

struct Terminal {
    virtual ~Terminal() {}
    virtual int  width() = 0;                          // columns of the window
    virtual void write(const std::string& text) = 0;   // text without line breaks, written at the cursor (the cursor moves right)
    virtual void newline() = 0;                        // column 0 of the next row (the window scrolls at the bottom)
    virtual void up(int rows) = 0;                     // `rows` rows up, in column 0  (up(0) = column 0 of this row)
    virtual void flush() = 0;
};

class ScreenBlock {
public:
    explicit ScreenBlock(Terminal& t) : term(t) {}

    // Show the lines, or change what is shown: the old lines are written over (a shorter list clears the lines that are no longer used).
    // A line is cut to fit the window (width - 1 columns: a line that fills the last column could wrap).  The cursor stays in the block.
    void draw(const std::vector<std::string>& lines) {
        std::vector<std::string> l = lines;
        if (l.empty()) l.push_back("");
        int w = textWidth();
        int n = (int)l.size();
        if (drawn > 0) term.up(drawn - 1);
        for (int i = 0; i < n; i++) {
            term.write(fit(l[i], w));
            if (i + 1 < n) term.newline();
        }
        if (drawn > n) {                                // the block got shorter: wipe what is left and come back
            for (int i = n; i < drawn; i++) { term.newline(); term.write(blank(w)); }
            term.up(drawn - n);
        }
        drawn = n;
        term.flush();
    }

    // Erase the block, print `text` as ordinary lines (they stay on the screen) where the block was.  The next draw() starts below them.
    void printAbove(const std::vector<std::string>& text) {
        if (drawn > 0) {
            int w = textWidth();
            term.up(drawn - 1);
            for (int i = 0; i < drawn; i++) { term.write(blank(w)); if (i + 1 < drawn) term.newline(); }
            term.up(drawn - 1);
            drawn = 0;
        }
        for (size_t i = 0; i < text.size(); i++) { term.write(text[i]); term.newline(); }
        term.flush();
    }

    // Leave the block on the screen and go to a fresh line below it (the shell's prompt comes there).
    void finish() {
        if (drawn > 0) { term.newline(); drawn = 0; }
        term.flush();
    }

    int height() const { return drawn; }

private:
    int textWidth() { int w = term.width() - 1; return w < 1 ? 1 : w; }
    static std::string fit(const std::string& s, int w) {
        std::string r = s.substr(0, (size_t)w);
        r.resize((size_t)w, ' ');                       // the rest of the old text is written over with spaces
        return r;
    }
    static std::string blank(int w) { return std::string((size_t)w, ' '); }

    Terminal& term;
    int drawn = 0;                                      // lines on the screen now; the cursor is in the last of them
};

}  // namespace opnmml
