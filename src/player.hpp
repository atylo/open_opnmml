// ================================================================================================================
// player - the interactive player's core (NOT in the original plugin).
//
// Everything that is not specific to one operating system is here, so that it can be tested without a sound card:
//   * the playlist (files and folders), the keys, pause / stop with the game's quick fade / next / previous,
//   * the audio: pull() hands out exactly the number of frames that are asked for.  The engine is run in blocks of 128 frames (what the
//     game's own audio path does) and the part of a block that has not been handed out yet is kept for the next call, so the sound does not
//     depend on how many frames the caller asks for at a time,
//   * the status line and the messages for the screen.
// The operating-system parts (sound output, keyboard) are small and live outside: see AudioOut below, audio_winmm.hpp and mmlplayer.cpp.
//
// The music plays in slot 3 of the engine, song 0, like the game's background music.  Each file is played from a freshly created engine, so
// nothing of the previous file (tones, envelopes, ringing notes) is left over.
// ================================================================================================================
#pragma once
#include "mixer.hpp"
#include "songlen.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace opnmml {

// Where the sound goes.  The Windows version is in audio_winmm.hpp; the tests use a fake one that counts time by itself.
struct AudioOut {
    virtual ~AudioOut() {}
    virtual bool open(int rate, std::string& error) = 0;            // 16-bit stereo at `rate` Hz
    virtual int  freeFrames() = 0;                                  // how many frames can be queued right now without waiting
    virtual void write(const int16_t* stereo, int frames) = 0;      // queue `frames` frames (never more than freeFrames())
    virtual void close() = 0;                                       // stop at once and give the device back
};

class Player {
public:
    struct Options {
        bool once = false;      // the mode the player starts in.  false: songs loop for ever, like the game.  true: every channel stops at the
                                // end of its own text.  The `l` key switches it while playing; the choice stays for the next files.
        int  rate = 44100;      // sample rate of the sound (the game uses 44100)
    };
    explicit Player(const Options& o);

    // ---- the playlist ---------------------------------------------------------------------------------------------------------------------
    // Add a file, or every .mus file of a folder and of all the folders below it.  In a folder the files come first (sorted by name), then the
    // sub-folders (sorted by name), each handled in the same way.  Sub-folders that are links are not followed, and folders nested more than
    // 12 levels deep are not searched.  Returns false when nothing was added; the reason is queued as a message.  A file is only read when its
    // turn comes, so a bad file shows up then.
    bool addPath(const std::string& path);
    int  trackCount() const { return (int)files.size(); }
    const std::string& trackPath(int i) const { return files[i]; }
    // Load and start the first track that can be played (a file that cannot be read or has an error is reported and skipped).
    // Returns false when no track can be played.
    bool begin();

    // ---- controls -------------------------------------------------------------------------------------------------------------------------
    //   space    pause / continue (and play again after a stop)      s     stop with the game's quick fade (about 0.26 s)
    //   Left     previous file      Right  next file                 l     switch between "loop" and "once" while playing
    //   Up       louder             Down   quieter                   [  ]  slower / faster (5 beats per minute)
    //   1..9 0 K switch channel A..I, J and K off / on (1 = A, 2 = B ... 9 = I, 0 = J)
    //   h        show / hide the list of the keys                    q  Esc  Ctrl-C  quit
    // Ordinary keys are given as their character code.  The arrow keys have no character: the front end gives the kKey... codes.
    static const int kKeyLeft = 1000, kKeyRight = 1001, kKeyUp = 1002, kKeyDown = 1003;
    void key(int ch);
    bool wantsQuit() const { return quitting; }
    // True when the player has nothing more to do: the last track ended by itself.  (Stopping with `s` does not count: space plays again.)
    bool finishedPlaylist() const { return allDone; }

    // ---- audio ----------------------------------------------------------------------------------------------------------------------------
    // Write exactly `frames` stereo frames (interleaved L,R).  Silence while paused or stopped.
    void pull(int16_t* out, int frames);
    // Hand the sound device whole chunks of `chunk` frames for as long as it has room for them.
    void pump(AudioOut& out, int chunk = 512);

    // ---- what to show ---------------------------------------------------------------------------------------------------------------------
    // The screen is a block of lines that the front end draws in place and redraws when it changes (see screen.hpp):
    //   1. the title of the music (an empty line when the file has none)
    //   2. the file name, the machine and the place in the list ("bgm01_opm.mus   X1   track 1 of 55")
    //   3. the status line (below)
    //   and, while the help is switched on with the h key, the list of the keys.
    std::vector<std::string> screenLines(int width = 79) const;
    // One line of at most width-1 characters: state, time / length, loop or once, volume, tempo, channels (A..K), and a hint for the help key.
    std::string statusLine(int width = 79) const;
    // Lines that have to stay on the screen as they are: errors, warnings, the end of the playlist.  (Everything else is in screenLines.)
    std::vector<std::string> takeMessages();
    bool helpShown() const { return showHelp; }

    // ---- read-only state (the tests use these) --------------------------------------------------------------------------------------------
    enum State { IDLE, PLAYING, STOPPING, TAIL };   // IDLE: nothing sounds.  STOPPING: the quick fade runs.  TAIL: the music is over, the last notes die away
    State state() const { return st; }
    bool  paused() const { return isPaused; }
    bool  playOnce() const { return once; }          // true: "once", false: "loop"
    int   index() const { return cur; }              // the current track (0-based), -1 before begin()
    int   volume() const { return vol; }             // master volume as the engine takes it: every +27 halves the level; 32 is the game's value
    unsigned muteMask() const { return mute; }       // bit n set = channel n (A = 0) is switched off
    int64_t framesDelivered() const { return delivered; }   // frames of the current track handed out so far (64 bits: a long is only 32 bits on Windows)
    const Mixer* mixer() const { return mx.get(); }
    static const int kBlock = 128;                   // engine block, in frames

private:
    bool readTrack(int idx, std::string& text);      // text of a file, or false (message queued)
    bool loadTrack(int idx);                         // build a fresh engine for track idx and start it
    void startFrom(int idx, int dir, bool automatic);// load idx; when it cannot be played try the next ones in direction dir
    void goTo(int idx);                              // fade out (or switch at once when nothing sounds), then play track idx
    void renderBlock();
    void afterTail();
    bool anyChannelActive() const;
    void say(const std::string& s) { msgs.push_back(s); }
    static std::vector<std::string> helpLines();
    const char* shownState() const;
    int  nChannels() const { return x1Dialect ? 11 : 6; }

    Options opt;
    bool once;                           // the current mode (starts as opt.once)
    std::vector<std::string> files;
    std::vector<std::string> msgs;

    std::unique_ptr<Mixer> mx;
    State st = IDLE;
    bool isPaused = false, quitting = false, allDone = false;
    bool autoAdvance = false;            // the music ended by itself (not by a key): go on to the next file after the tail
    int  cur = -1, pendingIdx = -1;      // pendingIdx: a track to start when the fade has finished
    int  pendingDir = 1;
    int  vol = 32;
    unsigned mute = 0;
    bool x1Dialect = false;
    std::string deviceName, titleText, infoText;
    bool showHelp = false;
    long onceMs = -1;                    // length of one pass in ms, -1 = unknown
    int64_t delivered = 0;

    // the block that is being handed out, and what is needed to see when the sound has died away
    int16_t carry[kBlock * 2];
    int carryPos = 0, carryLen = 0;
    int peak = 0;                        // loudest sample of this track so far
    static const int kWin = 14;          // 14 blocks = 1792 frames = about 40 ms
    int lo[kWin], hi[kWin], winPos = 0;
    long tailBlocks = 0;
};

}  // namespace opnmml
