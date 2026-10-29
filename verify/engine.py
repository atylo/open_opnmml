"""
engine.py - the original DLL's mixer/compiler/sequencer as a Python object (runs in the CPU emulator, see emu.py).

    m = DllMixer(); m.set_device(4); err = m.compile(3, 0, mml_bytes); m.play(3, 0); pcm = m.render(1024)
    song = m.dump_song(3)   # what the DLL's compiler stored in sequencer slot 3: channel bytecode, tones, envelopes
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu import DllEmu, c_sprintf, c_printf

DLL = os.environ.get('ORIGINAL_DLL', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'original.dll'))
OP_NEW, OP_DELETE, OP_DELETE_ARR, CRT_GETPTD, CRT_SPRINTF, CRT_PRINTF = 0x10006AB5, 0x100066CA, 0x10006BF4, 0x100093D2, 0x10006B77, 0x100069CA
MIXER_CTOR, MIXER_VOLUME, MIXER_DEVICE, MIXER_COMPILE, MIXER_PLAY, MIXER_RENDER, MIXER_MUTE, MIXER_RESET = (
    0x10023150, 0x10022B70, 0x10022D80, 0x10023060, 0x10022D30, 0x100233F0, 0x10022BC0, 0x10022DF0)
MIXER_STOP, MIXER_STOP_RESET, MIXER_TITLE, MIXER_TEMPO_GET, MIXER_TEMPO_SET = 0x10022CE0, 0x10022C80, 0x10022AF0, 0x10022A50, 0x10022A90
VEC_SIZE, VEC_AT = 0x10001780, 0x100017A0            # std::vector<unsigned char>: size() and operator[] (returns a pointer)


class DllMixer:
    def __init__(self, rate=44100, volume=32, device=4, heap_fill=0x00):
        e = self.emu = DllEmu(DLL, heap_fill=heap_fill)
        ptd = e.alloc(0x100, 0); e.write_u32(ptd + 0x14, 1)                  # per-thread data, rand() seed 1
        e.add_hook(CRT_GETPTD, lambda x: x.cdecl_return(ptd))
        e.add_hook(OP_NEW, lambda x: x.cdecl_return(x.alloc(x.arg(0))))      # operator new
        e.add_hook(OP_DELETE, lambda x: x.cdecl_return(0))                   # operator delete (memory is never reused)
        e.add_hook(OP_DELETE_ARR, lambda x: x.cdecl_return(0))
        e.add_hook(CRT_SPRINTF, c_sprintf)                                  # error messages are built with sprintf ...
        e.add_hook(CRT_PRINTF, c_printf)                                    # ... and printed with printf (collected in emu.stdout)
        self.rate = rate
        self.mixer = e.alloc(0x5C)
        e.call(MIXER_CTOR, rate, this=self.mixer)
        e.call(MIXER_VOLUME, volume, this=self.mixer)
        e.call(MIXER_DEVICE, device, this=self.mixer)

    def set_device(self, dev):
        """What System.OPNSetDevice does once the mixer exists: stop and CLEAR all four slots (sub_10022DF0), then select the chip."""
        self.emu.call(MIXER_RESET, this=self.mixer)
        self.emu.call(MIXER_DEVICE, dev, this=self.mixer)
    def set_volume(self, v):     self.emu.call(MIXER_VOLUME, v, this=self.mixer)
    def set_mute(self, m):       self.emu.call(MIXER_MUTE, m, this=self.mixer)

    def compile(self, slot, num, mml):
        """Compile MML text into slot/num exactly like OPNPlaySE does.  Returns '' on success or the error text."""
        text = self.emu.cstr(mml)
        err = self.emu.call(MIXER_COMPILE, slot, text, 0, num, this=self.mixer, max_instr=20_000_000)
        return self.emu.read(err, 200).split(b'\0')[0] if err else b''

    def play(self, slot, num): self.emu.call(MIXER_PLAY, slot, num, this=self.mixer)

    def render(self, frames):
        buf = self.emu.alloc(4 * frames, 0)
        self.emu.call(MIXER_RENDER, buf, frames, this=self.mixer)
        return self.emu.read(buf, 4 * frames)

    # ---- what the compiler stored (layout of the original's objects: tones +13324, envelopes +4876, code vectors +4) ----
    def seq_addr(self, slot): return self.emu.read_u32(self.mixer + 20 + 4 * slot)

    def dump_song(self, slot):
        e, s = self.emu, self.seq_addr(slot)
        code = []
        for ch in range(11):
            vec = s + 4 + 24 * ch
            n = e.call(VEC_SIZE, this=vec)
            if n == 0: code.append(b''); continue
            p = e.call(VEC_AT, 0, this=vec)
            code.append(e.read(p, n))
        tones = e.read(s + 13324, 26 * 128)
        envs = e.read(s + 4876, 132 * 64)
        x1_mode = e.read_u32(s + 18468) & 0xFF
        total_offset = e.read_u32(s + 16736)
        return {'code': code, 'tones': tones, 'envs': envs, 'x1_mode': x1_mode, 'total_offset': total_offset}

    # ---- slot-level operations (sub_10022CE0 / 10022C80 / 10022AF0 / 10022A50 / 10022A90) ----
    def stop(self, slot):          self.emu.call(MIXER_STOP, slot, this=self.mixer)          # OPNStopSE: fade-out
    def stop_reset(self, slot):    self.emu.call(MIXER_STOP_RESET, slot, this=self.mixer)    # cut at once
    def tempo(self, slot):
        v = self.emu.call(MIXER_TEMPO_GET, slot, this=self.mixer)
        return v - (1 << 32) if v & 0x80000000 else v
    def set_tempo(self, slot, bpm): self.emu.call(MIXER_TEMPO_SET, slot, bpm, this=self.mixer)
    def title(self, slot, num):
        p = self.emu.call(MIXER_TITLE, slot, num, this=self.mixer)
        return self.emu.read_cstr(p) if p else b''

    def state(self):
        """Everything the port's slotscript 'state' command prints, read straight out of the DLL's objects (same keys)."""
        import struct
        e, out = self.emu, {}
        sd = lambda v: v - (1 << 32) if v & 0x80000000 else v
        for s in range(4):
            sa = self.seq_addr(s)
            for c in range(11):
                vec = sa + 4 + 24 * c
                n = e.call(VEC_SIZE, this=vec)
                out[('code', s, c)] = e.read(e.call(VEC_AT, 0, this=vec), n) if n else b''
            out[('tones', s)] = e.read(sa + 13324, 26 * 128)
            out[('envs', s)] = e.read(sa + 4876, 132 * 64)
            out[('starts', s)] = e.read(sa + 4 * 67, 4 * 704)
            out[('rt', s)] = tuple(sd(e.read_u32(sa + 4 * i)) for i in (4179, 4176, 4177, 4178, 4180, 4181, 4182, 4183))
            for c in range(11):
                h = e.read_u32(sa + 4 * (4163 + c))
                raw = e.read(h, 0x68)
                owns = raw[8]; state = struct.unpack_from('<h', raw, 10)[0]; ln = struct.unpack_from('<H', raw, 12)[0]
                off, loop = struct.unpack_from('<ii', raw, 16); vol = struct.unpack_from('<i', raw, 32)[0]
                out[('rtch', s, c)] = (1 if struct.unpack_from('<I', raw, 4)[0] else 0, state, ln, off, loop, owns, raw[46], vol)
        return out
