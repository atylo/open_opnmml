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
        bool once = false;      // false: songs loop for ever, like the game.  true: every channel stops at the end of its own text.
        int  rate = 44100;      // sample rate of the sound (the game uses 44100)
    };
    explicit Player(const Options& o);

    // ---- the playlist ---------------------------------------------------------------------------------------------------------------------
    // Add a file, or every .mus file of a folder (not the sub-folders), sorted by name.  Returns false when nothing was added; the reason
    // is queued as a message.  A file is only read when its turn comes, so a bad file shows up then.
    bool addPath(const std::string& path);
    int  trackCount() const { return (int)files.size(); }
    const std::string& trackPath(int i) const { return files[i]; }
    // Load and start the first track that can be played (a file that cannot be read or has an error is reported and skipped).
    // Returns false when no track can be played.
    bool begin();

    // ---- controls -------------------------------------------------------------------------------------------------------------------------
    //   space  pause / continue (and play again after a stop)      s  stop with the game's quick fade (about 0.26 s)
    //   + =    louder      -  quieter                                [  ]  slower / faster (5 beats per minute)
    //   A..K   switch a channel off / on (letters in either case)   n  next file     p  previous file
    //   ?      list the keys                                          q  Esc  quit
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
    // Lines of text that appeared since the last call (a new file started, an error, the key list ...).
    std::vector<std::string> takeMessages();
    // One line of at most width-1 characters: track number, file name, machine, time, volume, tempo, channels, state.
    std::string statusLine(int width = 79) const;

    // ---- read-only state (the tests use these) --------------------------------------------------------------------------------------------
    enum State { IDLE, PLAYING, STOPPING, TAIL };   // IDLE: nothing sounds.  STOPPING: the quick fade runs.  TAIL: the music is over, the last notes die away
    State state() const { return st; }
    bool  paused() const { return isPaused; }
    int   index() const { return cur; }              // the current track (0-based), -1 before begin()
    int   volume() const { return vol; }             // master volume as the engine takes it: every +27 halves the level; 32 is the game's value
    unsigned muteMask() const { return mute; }       // bit n set = channel n (A = 0) is switched off
    long  framesDelivered() const { return delivered; }   // frames of the current track handed out so far
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
    void listKeys();
    int  nChannels() const { return x1Dialect ? 11 : 6; }

    Options opt;
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
    std::string deviceName, trackName;
    long onceMs = -1;                    // length of one pass in ms, -1 = unknown
    long delivered = 0;

    // the block that is being handed out, and what is needed to see when the sound has died away
    int16_t carry[kBlock * 2];
    int carryPos = 0, carryLen = 0;
    int peak = 0;                        // loudest sample of this track so far
    static const int kWin = 14;          // 14 blocks = 1792 frames = about 40 ms
    int lo[kWin], hi[kWin], winPos = 0;
    long tailBlocks = 0;
};

}  // namespace opnmml
