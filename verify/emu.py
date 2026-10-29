"""
emu.py - run code from the ORIGINAL DLL inside a CPU emulator (Unicorn), with no host application and no Windows.

The DLL is a 32-bit x86 Windows plugin, but the sound engine inside it (compiler, sequencer, chip, mixer) is plain
computation.  We map the DLL image at its preferred base address (0x10000000), never run its entry point or the
host-facing functions (V2Link, the TJS glue), and instead call the engine's internal functions directly by their
addresses - the sub_XXXXXXXX numbers are those addresses.

What has to be faked:
  * the few operating-system calls the engine makes (critical sections, heap) - see IMPORT_HANDLERS;
  * a handful of C-runtime functions that live inside the DLL (rand, operator new/delete, ...) - see HOOKS.
    rand() is NOT faked: it runs as the DLL's own code except for its thread-data lookup, so the real generator is used.

Requirements:  pip install unicorn pefile
"""
import struct
import pefile
from unicorn import (Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED, UcError)
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_ESP,
                               UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EIP)

IMAGE_BASE = 0x10000000
STACK_TOP = 0x001F0000
STUB_BASE = 0x70000000       # one 16-byte stub per imported function
HEAP_BASE = 0x20000000
RETURN_SENTINEL = 0x7FFF0000  # calls "return" here; emulation stops when EIP reaches it


class EmuError(Exception):
    pass


class DllEmu:
    def __init__(self, dll_path, heap_fill=0x00, verbose=False):
        self.pe = pefile.PE(dll_path)
        self.verbose = verbose
        mu = self.mu = Uc(UC_ARCH_X86, UC_MODE_32)
        size = (self.pe.OPTIONAL_HEADER.SizeOfImage + 0xFFF) & ~0xFFF
        mu.mem_map(IMAGE_BASE, size)
        mu.mem_write(IMAGE_BASE, bytes(self.pe.get_memory_mapped_image()))
        mu.mem_map(STACK_TOP - 0x100000, 0x100000)
        mu.mem_map(HEAP_BASE, 0x04000000)
        mu.mem_map(STUB_BASE, 0x10000)
        mu.mem_map(RETURN_SENTINEL & ~0xFFF, 0x1000)
        self.heap_ptr = HEAP_BASE
        self._setup_teb()
        self.heap_fill = heap_fill
        self.hooks = {}            # absolute address -> python callable(self)
        self.stub_names = {}       # stub address -> (dll, name)
        self.call_log = []
        self._patch_imports()
        # Only the stub region is hooked as a range; individual DLL functions are hooked one address at a time
        # (a Python callback per instruction would be far too slow).
        mu.hook_add(UC_HOOK_CODE, self._stub_hook, begin=STUB_BASE, end=STUB_BASE + 0x10000)
        mu.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped)

    # ------------------------------------------------------------------ thread block
    def _setup_teb(self):
        """Compiler-generated exception frames read fs:[0] (head of the SEH chain); give the code a minimal thread block.
        Layout: +0 SEH chain head (-1 = end), +4 stack base, +8 stack limit, +0x18 self pointer, +0x2C TLS array."""
        TEB = 0x7FFD0000
        self.mu.mem_map(TEB, 0x2000)
        blob = bytearray(0x2000)
        struct.pack_into('<I', blob, 0x00, 0xFFFFFFFF)
        struct.pack_into('<I', blob, 0x04, STACK_TOP)
        struct.pack_into('<I', blob, 0x08, STACK_TOP - 0x100000)
        struct.pack_into('<I', blob, 0x18, TEB)
        struct.pack_into('<I', blob, 0x2C, TEB + 0x1000)
        self.mu.mem_write(TEB, bytes(blob))
        self.mu.msr_write(0xC0000100, TEB)          # IA32_FS_BASE
        self.teb = TEB

    # ------------------------------------------------------------------ imports
    def _patch_imports(self):
        mu = self.mu
        n = 0
        for entry in self.pe.DIRECTORY_ENTRY_IMPORT:
            dll = entry.dll.decode()
            for imp in entry.imports:
                name = imp.name.decode() if imp.name else 'ord%d' % imp.ordinal
                stub = STUB_BASE + 16 * n
                n += 1
                mu.mem_write(stub, b'\xC3')                       # ret (never executed: the hook intercepts first)
                mu.mem_write(imp.address, struct.pack('<I', stub))   # IAT slot -> stub
                self.stub_names[stub] = (dll, name)

    def add_hook(self, address, fn):
        """Run fn(emu) whenever execution reaches `address` (instead of the instruction there).
        fn must finish the call itself, e.g. with cdecl_return() / stdcall_return()."""
        self.hooks[address] = fn
        self.mu.hook_add(UC_HOOK_CODE, lambda mu, a, s, u: fn(self), begin=address, end=address)

    def _stub_hook(self, mu, address, size, user):
        if True:
            dll, name = self.stub_names.get(address, ('?', '?'))
            h = IMPORT_HANDLERS.get(name)
            self.call_log.append(name)
            if h is None:
                raise EmuError('unimplemented import %s!%s called from 0x%08x' % (dll, name, self.read_u32(self.reg(UC_X86_REG_ESP))))
            nargs = h.__code__.co_argcount - 1
            args = [self.arg(i) for i in range(nargs)]
            ret = h(self, *args) or 0
            self.set_reg(UC_X86_REG_EAX, ret & 0xFFFFFFFF)
            self.stdcall_return(nargs)

    def _unmapped(self, mu, access, address, size, value, user):
        raise EmuError('access to unmapped memory 0x%08x (eip=0x%08x)' % (address, self.reg(UC_X86_REG_EIP)))

    # ------------------------------------------------------------------ small helpers
    def reg(self, r): return self.mu.reg_read(r)
    def set_reg(self, r, v): self.mu.reg_write(r, v & 0xFFFFFFFF)
    def read(self, addr, n): return bytes(self.mu.mem_read(addr, n))
    def write(self, addr, data): self.mu.mem_write(addr, bytes(data))
    def read_u32(self, a): return struct.unpack('<I', self.read(a, 4))[0]
    def read_i32(self, a): return struct.unpack('<i', self.read(a, 4))[0]
    def write_u32(self, a, v): self.write(a, struct.pack('<I', v & 0xFFFFFFFF))
    def arg(self, i):                      # i-th stack argument of the function being entered (stack[0] = return address)
        return self.read_u32(self.reg(UC_X86_REG_ESP) + 4 + 4 * i)

    def cdecl_return(self, value=0):
        """Return from a hooked function the caller cleans up (cdecl, or thiscall with no stack args)."""
        self.set_reg(UC_X86_REG_EAX, value & 0xFFFFFFFF)
        esp = self.reg(UC_X86_REG_ESP)
        self.set_reg(UC_X86_REG_EIP, self.read_u32(esp))
        self.set_reg(UC_X86_REG_ESP, esp + 4)

    def stdcall_return(self, nargs):
        esp = self.reg(UC_X86_REG_ESP)
        ret = self.read_u32(esp)
        self.set_reg(UC_X86_REG_ESP, esp + 4 + 4 * nargs)
        self.set_reg(UC_X86_REG_EIP, ret)

    def read_cstr(self, addr, limit=4096):
        out = bytearray()
        while len(out) < limit:
            b = self.read(addr + len(out), 1)[0]
            if b == 0: break
            out.append(b)
        return bytes(out)

    def alloc(self, size, fill=None):
        """Simple bump allocator in the emulated heap (16-byte aligned)."""
        size = (size + 15) & ~15
        addr = self.heap_ptr
        self.heap_ptr += size
        if self.heap_ptr > HEAP_BASE + 0x04000000:
            raise EmuError('emulated heap exhausted')
        f = self.heap_fill if fill is None else fill
        self.write(addr, bytes([f]) * size)
        return addr

    def cstr(self, text):
        data = text if isinstance(text, bytes) else text.encode('latin-1')
        addr = self.alloc(len(data) + 1, 0)
        self.write(addr, data + b'\0')
        return addr

    # ------------------------------------------------------------------ calling emulated functions
    def call(self, addr, *args, this=None, max_instr=300_000_000):
        """Call a function in the DLL.  `this` selects __thiscall (ECX); arguments are pushed right to left.
        All the engine functions are __thiscall/__cdecl/__stdcall callee-clean or caller-clean; we reset ESP afterwards,
        so the difference does not matter.  Returns EAX."""
        mu = self.mu
        esp = STACK_TOP - 0x1000
        for a in reversed(args):
            esp -= 4
            self.write_u32(esp, a)
        esp -= 4
        self.write_u32(esp, RETURN_SENTINEL)
        self.set_reg(UC_X86_REG_ESP, esp)
        self.set_reg(UC_X86_REG_EBP, 0)
        if this is not None:
            self.set_reg(UC_X86_REG_ECX, this)
        try:
            mu.emu_start(addr, RETURN_SENTINEL, count=max_instr)
        except UcError as e:
            raise EmuError('emulation error %s at eip=0x%08x' % (e, self.reg(UC_X86_REG_EIP)))
        if self.reg(UC_X86_REG_EIP) != RETURN_SENTINEL:
            raise EmuError('call to 0x%08x did not return within %d instructions (stopped at eip=0x%08x)' % (addr, max_instr, self.reg(UC_X86_REG_EIP)))
        return self.reg(UC_X86_REG_EAX)


# ====================================================================================================================
# Operating-system calls the engine makes (KERNEL32).  Each handler gets the stack arguments, returns EAX.
# ====================================================================================================================
def _noop(emu, *a): return 1

IMPORT_HANDLERS = {
    'InitializeCriticalSection': lambda emu, p: 0,
    'EnterCriticalSection': lambda emu, p: 0,
    'LeaveCriticalSection': lambda emu, p: 0,
    'DeleteCriticalSection': lambda emu, p: 0,
    'TryEnterCriticalSection': lambda emu, p: 1,
    'GetCurrentThreadId': lambda emu: 1,
    'GetLastError': lambda emu: 0,
    'SetLastError': lambda emu, e: 0,
    'InterlockedIncrement': lambda emu, p: _interlocked(emu, p, 1),
    'InterlockedDecrement': lambda emu, p: _interlocked(emu, p, -1),
    'Sleep': lambda emu, ms: 0,
    'HeapAlloc': lambda emu, h, flags, size: emu.alloc(size, 0 if flags & 8 else None),
    'HeapFree': lambda emu, h, flags, p: 1,
}


def _interlocked(emu, p, d):
    v = (emu.read_i32(p) + d)
    emu.write_u32(p, v)
    return v


def c_format(emu, fmt, argi):
    """C-style formatting of the DLL's variadic calls: %s %d %i %u %c %x %X %% with optional width / zero padding."""
    out, i = bytearray(), 0
    while i < len(fmt):
        c = fmt[i]
        if c != 0x25:
            out.append(c); i += 1; continue
        i += 1
        flags = bytearray()
        while i < len(fmt) and chr(fmt[i]) in '-+ #0123456789.':
            flags.append(fmt[i]); i += 1
        conv = chr(fmt[i]); i += 1
        width = int(bytes(flags).lstrip(b'-+ #0').split(b'.')[0] or 0) if flags else 0
        zero = bytes(flags).startswith(b'0')
        if conv == '%': out.append(0x25); continue
        v = emu.arg(argi); argi += 1
        if conv in 'di': s = str(v - (1 << 32) if v & 0x80000000 else v).encode()
        elif conv == 'u': s = str(v).encode()
        elif conv in 'xX': s = (('%x' if conv == 'x' else '%X') % v).encode()
        elif conv == 'c': s = bytes([v & 0xFF])
        elif conv == 's': s = emu.read_cstr(v)
        else: s = b'?'
        out += s.rjust(width, b'0' if zero else b' ')
    return bytes(out)


def c_sprintf(emu):
    """Replacement for the DLL's CRT sprintf (cdecl: buffer, format, ...)."""
    out = c_format(emu, emu.read_cstr(emu.arg(1)), 2)
    emu.write(emu.arg(0), out + b'\0')
    emu.cdecl_return(len(out))


def c_printf(emu):
    """Replacement for the DLL's CRT printf (cdecl: format, ...): the text is collected in emu.stdout instead of printed."""
    out = c_format(emu, emu.read_cstr(emu.arg(0)), 1)
    emu.stdout = getattr(emu, 'stdout', b'') + out
    emu.cdecl_return(len(out))
