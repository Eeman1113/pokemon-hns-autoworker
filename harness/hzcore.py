"""hzcore — headless GBA emulation for the Auto Worker harness.

Drives libmgba directly through a small compiled shim (shim/hzcore.c). No
window, no audio, no frame limiter: ~68x realtime on an M-series Mac, which is
what makes it realistic to verify an agent that is supposed to play a 30-hour
game.

Deliberately NOT mGBA's Lua scripting: the Homebrew build ships only the Qt
app, whose `--help` exposes no --script flag, so Lua there is GUI-only and
cannot be driven from CI. mgba-py was the other candidate and publishes no
macOS arm64 wheel.
"""
import ctypes, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
DYLIB = os.path.join(HERE, "shim", "libhzcore.dylib")
MGBA_PREFIX_CANDIDATES = [
    "/opt/homebrew/Cellar/mgba/0.10.5_2",
    "/opt/homebrew/opt/mgba",
    "/usr/local/opt/mgba",
]


class EmulatorUnavailable(RuntimeError):
    """The harness could not start an emulator. Never confuse with a test failure."""


# GBA key bit positions, from mgba/internal/gba/input.h
KEY = {"A": 1 << 0, "B": 1 << 1, "SELECT": 1 << 2, "START": 1 << 3,
       "RIGHT": 1 << 4, "LEFT": 1 << 5, "UP": 1 << 6, "DOWN": 1 << 7,
       "R": 1 << 8, "L": 1 << 9}


def _find_mgba_prefix():
    for p in MGBA_PREFIX_CANDIDATES:
        if os.path.isdir(os.path.join(p, "include", "mgba")):
            return p
    try:
        out = subprocess.run(["brew", "--prefix", "mgba"], capture_output=True,
                             text=True, timeout=20)
        p = out.stdout.strip()
        if p and os.path.isdir(os.path.join(p, "include", "mgba")):
            return p
    except Exception:
        pass
    return None


def ensure_built():
    """Compile the shim if missing. Raises EmulatorUnavailable with a real reason."""
    if os.path.exists(DYLIB):
        return DYLIB
    prefix = _find_mgba_prefix()
    if not prefix:
        raise EmulatorUnavailable(
            "mGBA headers not found. Install with: brew install mgba")
    src = os.path.join(HERE, "shim", "hzcore.c")
    cmd = ["clang", "-O2", "-fPIC", "-shared", "-o", DYLIB, src,
           f"-I{prefix}/include", f"-L{prefix}/lib", "-lmgba",
           f"-Wl,-rpath,{prefix}/lib"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise EmulatorUnavailable(f"shim build failed:\n{r.stderr[:2000]}")
    return DYLIB


class Core:
    def __init__(self, rom_path, silence=True):
        rom_path = os.path.expanduser(rom_path)
        if not os.path.exists(rom_path):
            raise EmulatorUnavailable(f"ROM not found: {rom_path}")
        ensure_built()
        self._lib = ctypes.CDLL(DYLIB)
        self._bind()
        if silence:
            self._lib.hz_silence_logs()
        self._h = self._lib.hz_open(rom_path.encode())
        if not self._h:
            raise EmulatorUnavailable(f"libmgba refused the ROM: {rom_path}")
        self.rom_path = rom_path
        self._keys = 0

    def _bind(self):
        L = self._lib
        c_vp, c_u32, c_i = ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int
        L.hz_open.restype, L.hz_open.argtypes = c_vp, [ctypes.c_char_p]
        L.hz_close.argtypes = [c_vp]
        L.hz_reset.argtypes = [c_vp]
        L.hz_set_keys.argtypes = [c_vp, c_u32]
        L.hz_get_keys.restype, L.hz_get_keys.argtypes = c_u32, [c_vp]
        L.hz_run_frames.argtypes = [c_vp, c_i]
        L.hz_frame_counter.restype, L.hz_frame_counter.argtypes = c_u32, [c_vp]
        for n in ("hz_read8", "hz_read16", "hz_read32"):
            getattr(L, n).restype = c_u32
            getattr(L, n).argtypes = [c_vp, c_u32]
        L.hz_write8.argtypes = [c_vp, c_u32, ctypes.c_uint8]
        L.hz_write16.argtypes = [c_vp, c_u32, ctypes.c_uint16]
        L.hz_write32.argtypes = [c_vp, c_u32, c_u32]
        L.hz_read_block.argtypes = [c_vp, c_u32, ctypes.POINTER(ctypes.c_uint8), c_u32]
        L.hz_save_state.restype, L.hz_save_state.argtypes = c_i, [c_vp, ctypes.c_char_p]
        L.hz_load_state.restype, L.hz_load_state.argtypes = c_i, [c_vp, ctypes.c_char_p]
        L.hz_video_info.argtypes = [c_vp] + [ctypes.POINTER(ctypes.c_uint)] * 3
        L.hz_video_copy.argtypes = [c_vp, ctypes.POINTER(ctypes.c_uint8)]
        L.hz_game_code.argtypes = [c_vp, ctypes.c_char_p]
        L.hz_game_title.argtypes = [c_vp, ctypes.c_char_p]

    # -- lifecycle ---------------------------------------------------------
    def close(self):
        if getattr(self, "_h", None):
            self._lib.hz_close(self._h)
            self._h = None

    def __enter__(self): return self
    def __exit__(self, *a): self.close()

    def reset(self): self._lib.hz_reset(self._h)

    @property
    def frame(self): return self._lib.hz_frame_counter(self._h)

    def game_title(self):
        b = ctypes.create_string_buffer(32)
        self._lib.hz_game_title(self._h, b)
        return b.value.decode(errors="replace")

    def game_code(self):
        b = ctypes.create_string_buffer(16)
        self._lib.hz_game_code(self._h, b)
        return b.value.decode(errors="replace")

    # -- input -------------------------------------------------------------
    def set_keys(self, mask):
        self._keys = mask
        self._lib.hz_set_keys(self._h, mask)

    def keys_from(self, names):
        m = 0
        for n in names:
            n = n.strip().upper()
            if n and n not in KEY:
                raise ValueError(f"unknown button {n!r}; known: {sorted(KEY)}")
            if n:
                m |= KEY[n]
        return m

    def run(self, frames, keys=0):
        """Hold `keys` for `frames` frames. Caller owns press/release timing."""
        self.set_keys(keys)
        self._lib.hz_run_frames(self._h, int(frames))

    def tap(self, button, hold=4, gap=4):
        """A press the game will actually register as an edge."""
        self.run(hold, self.keys_from([button]))
        self.run(gap, 0)

    # -- memory ------------------------------------------------------------
    def r8(self, a):  return self._lib.hz_read8(self._h, a)
    def r16(self, a): return self._lib.hz_read16(self._h, a)
    def r32(self, a): return self._lib.hz_read32(self._h, a)
    def w8(self, a, v):  self._lib.hz_write8(self._h, a, v)
    def w16(self, a, v): self._lib.hz_write16(self._h, a, v)
    def w32(self, a, v): self._lib.hz_write32(self._h, a, v)

    def block(self, addr, length):
        buf = (ctypes.c_uint8 * length)()
        self._lib.hz_read_block(self._h, addr, buf, length)
        return bytes(buf)

    # -- video -------------------------------------------------------------
    def video_info(self):
        w = ctypes.c_uint(); h = ctypes.c_uint(); bpp = ctypes.c_uint()
        self._lib.hz_video_info(self._h, ctypes.byref(w), ctypes.byref(h),
                                ctypes.byref(bpp))
        return w.value, h.value, bpp.value

    def framebuffer(self):
        """Raw pixels. Lets a scenario assert on what is actually on screen, and
        lets a human see which state a generated save state actually reached."""
        w, h, bpp = self.video_info()
        buf = (ctypes.c_uint8 * (w * h * bpp))()
        self._lib.hz_video_copy(self._h, buf)
        return bytes(buf), w, h, bpp

    def screenshot(self, path):
        from PIL import Image
        raw, w, h, bpp = self.framebuffer()
        if bpp == 4:
            img = Image.frombytes("RGBA", (w, h), raw).convert("RGB")
            b, g, r = img.split()[0], img.split()[1], img.split()[2]
            img = Image.merge("RGB", (r, g, b))   # mGBA buffers BGR on LE
        else:
            img = Image.frombytes("RGB;16", (w, h), raw)
        img.save(path)
        return path

    def screen_hash(self):
        import hashlib
        return hashlib.sha256(self.framebuffer()[0]).hexdigest()[:16]

    # -- states ------------------------------------------------------------
    def save_state(self, path):
        return bool(self._lib.hz_save_state(self._h, os.fspath(path).encode()))

    def load_state(self, path):
        path = os.path.expanduser(path)
        if not os.path.exists(path):
            raise EmulatorUnavailable(f"save state missing: {path}")
        return bool(self._lib.hz_load_state(self._h, path.encode()))
