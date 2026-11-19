// ================================================================================================================
// audio_winmm.hpp - sound output through the Windows multimedia API (waveOut).  No extra libraries: link with -lwinmm.
//
// A ring of eight buffers of 512 frames (11.6 ms each, 93 ms in all) is handed to the sound card.  The caller asks how much room there is
// (freeFrames) and writes whole buffers; the card marks a buffer as done when it has played it.  There is no callback and no thread: the
// player's main loop does everything, so nothing needs locking.
// ================================================================================================================
#pragma once
#ifdef _WIN32
#include "player.hpp"
#include <cstring>
#include <vector>
#include <windows.h>
#include <mmsystem.h>

namespace opnmml {

class WinmmOut : public AudioOut {
public:
    static const int kBufFrames = 512;
    static const int kBuffers = 8;

    ~WinmmOut() { close(); }
    int chunkFrames() const { return kBufFrames; }

    bool open(int rate, std::string& error) override {
        WAVEFORMATEX fmt;
        memset(&fmt, 0, sizeof fmt);
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 2;
        fmt.nSamplesPerSec = (DWORD)rate;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = 4;
        fmt.nAvgBytesPerSec = (DWORD)rate * 4;
        MMRESULT r = waveOutOpen(&dev, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
        if (r != MMSYSERR_NOERROR) {
            char text[MAXERRORLENGTH] = "";
            waveOutGetErrorTextA(r, text, sizeof text);
            error = std::string("cannot open the sound device: ") + text;
            dev = NULL;
            return false;
        }
        data.assign((size_t)kBuffers * kBufFrames * 2, 0);
        memset(hdr, 0, sizeof hdr);
        for (int i = 0; i < kBuffers; i++) {
            hdr[i].lpData = (LPSTR)&data[(size_t)i * kBufFrames * 2];
            hdr[i].dwBufferLength = kBufFrames * 4;
            waveOutPrepareHeader(dev, &hdr[i], sizeof(WAVEHDR));
            hdr[i].dwFlags |= WHDR_DONE;                    // "done" = free to fill
        }
        next = 0;
        return true;
    }

    int freeFrames() override {
        int n = 0;
        for (int i = 0; i < kBuffers; i++) if (isDone(i)) n++;
        return n * kBufFrames;
    }

    void write(const int16_t* stereo, int frames) override {
        if (!dev || frames <= 0 || frames > kBufFrames || !isDone(next)) return;
        memcpy(hdr[next].lpData, stereo, (size_t)frames * 4);
        hdr[next].dwBufferLength = (DWORD)frames * 4;
        waveOutWrite(dev, &hdr[next], sizeof(WAVEHDR));
        next = (next + 1) % kBuffers;
    }

    void close() override {
        if (!dev) return;
        waveOutReset(dev);                                  // gives every queued buffer back at once
        for (int i = 0; i < kBuffers; i++) waveOutUnprepareHeader(dev, &hdr[i], sizeof(WAVEHDR));
        waveOutClose(dev);
        dev = NULL;
    }

private:
    bool isDone(int i) const { return ((*(volatile const DWORD*)&hdr[i].dwFlags) & WHDR_DONE) != 0; }
    HWAVEOUT dev = NULL;
    WAVEHDR hdr[kBuffers];
    std::vector<int16_t> data;
    int next = 0;
};

}  // namespace opnmml
#endif
