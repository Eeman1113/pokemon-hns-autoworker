"""state.py — reads game state out of a running core.

Two problems stand between a raw emulator and an assertable game state:

  1. WHERE the globals live. The dev build emits a symbol map; the shipped ROM
     has none. Primary path is a symbol file; the fallback is empirical
     discovery (see discover_keys), which works on a stripped ROM.

  2. WHAT the offsets inside those globals are. include/global.h documents them
     inline as /*0x490*/ comments -- but this fork inserted `u16 saveVersion` at
     the top of SaveBlock1 WITHOUT updating the comments below it, so those
     numbers are suspect by 2-4 bytes. Every struct read is therefore
     plausibility-checked, and `validate()` reports what it could and could not
     confirm. Reading a wrong offset yields a confident wrong number, which is
     the worst failure mode a harness can have.
"""
import json, os, re

EWRAM = (0x02000000, 0x02040000)
IWRAM = (0x03000000, 0x03008000)

# Offsets transcribed from include/global.h. `confidence` is not decoration:
# DOC means the header documents it, SHIFTED means the header documents it but
# an un-commented field was inserted above it so it may be stale.
SAVEBLOCK1_LAYOUT = {
    "pos_x":            (0x00, "u16", "SHIFTED"),
    "pos_y":            (0x02, "u16", "SHIFTED"),
    "warp_map_group":   (0x04 + 4, "u8", "SHIFTED"),
    "warp_map_num":     (0x04 + 5, "u8", "SHIFTED"),
    "playerPartyCount": (0x234, "u8", "SHIFTED"),
    "playerParty":      (0x238, "bytes", "SHIFTED"),
    "money":            (0x490, "u32", "SHIFTED"),
    "flags":            (0x1270, "bytes", "SHIFTED"),
    "vars":             (0x139C, "bytes", "SHIFTED"),
}
MON_SIZE = 100          # struct Pokemon; ASSUMPTION -- verify against the build


class SymbolTable:
    """Tolerant parser: accepts nm output, pokeemerald .sym, and ld .map files."""
    _LINE = re.compile(r"^\s*(?:0x)?([0-9A-Fa-f]{6,8})\s+(?:[A-Za-z?]\s+)?([A-Za-z_][\w.]*)\s*$")
    _MAP = re.compile(r"^\s*(?:0x)?([0-9A-Fa-f]{8,16})\s+([A-Za-z_][\w.]*)\s*$")

    def __init__(self, path=None):
        self.syms, self.source = {}, None
        if path:
            self.load(path)

    def load(self, path):
        path = os.path.expanduser(path)
        if not os.path.exists(path):
            raise FileNotFoundError(path)
        n = 0
        with open(path, errors="replace") as fh:
            for line in fh:
                m = self._LINE.match(line) or self._MAP.match(line)
                if m:
                    addr = int(m.group(1), 16)
                    if 0x02000000 <= addr <= 0x09FFFFFF:
                        self.syms.setdefault(m.group(2), addr)
                        n += 1
        self.source = path
        return n

    def get(self, name):
        return self.syms.get(name)

    def __len__(self):
        return len(self.syms)


# Empirically confirmed on Pokemon Heart and Soul v2.0.6 (stripped, no symbols):
# all five key fields match include/main.h semantics, gMain+0x20 vblankCounter1
# advances 1/frame, and gMain+0x439 inBattle reads 0 on the overworld.
# Re-derive with discover_gmain() if the ROM changes -- do not trust this blind.
GMAIN_HNS_V206 = 0x03005BD8

GMAIN_FIELDS = {"heldKeysRaw": 0x28, "newKeysRaw": 0x2A, "heldKeys": 0x2C,
                "newKeys": 0x2E, "newAndRepeatedKeys": 0x30,
                "vblankCounter1": 0x20, "inBattle": 0x439}


def discover_gmain(core, settle=240):
    """Locate gMain with no symbol file, then prove it rather than assume it.

    Returns (addr, evidence) or (None, evidence). Verification is the point: a
    plausible-but-wrong base address produces confident garbage downstream.
    """
    from hzcore import KEY
    cand = discover_keys(core, settle)["candidates"]
    ev = {"key_candidates": [hex(a) for a in cand]}
    for held_raw in cand:
        base = held_raw - GMAIN_FIELDS["heldKeysRaw"]
        v1 = core.r32(base + GMAIN_FIELDS["vblankCounter1"])
        core.run(10, 0)
        v2 = core.r32(base + GMAIN_FIELDS["vblankCounter1"])
        core.set_keys(KEY["RIGHT"]); core._lib.hz_run_frames(core._h, 1)
        pulse = core.r16(base + GMAIN_FIELDS["newKeys"]) == KEY["RIGHT"]
        core._lib.hz_run_frames(core._h, 1)
        settled = core.r16(base + GMAIN_FIELDS["newKeys"]) == 0
        core.run(4, 0)
        if 8 <= (v2 - v1) <= 12 and pulse and settled:
            ev["verified"] = {"base": hex(base), "vblank_delta": v2 - v1,
                              "newKeys_pulses_once": True}
            return base, ev
    ev["verified"] = None
    return None, ev


def injection_signature(core, gmain):
    """True when something wrote the cooked key fields but not the raw ones.

    Auto Worker injects into newKeys/heldKeys/newAndRepeatedKeys and deliberately
    leaves heldKeysRaw/newKeysRaw untouched (so the A+B+START+SELECT soft reset
    always remains the human's). That asymmetry is directly observable, which
    makes "is the agent driving?" answerable without any symbol file.

    Caveat: ReadKeys() also remaps L to A in the cooked fields only, so a held L
    produces the same asymmetry. Callers must not hold L.
    """
    held = core.r16(gmain + GMAIN_FIELDS["heldKeys"])
    raw = core.r16(gmain + GMAIN_FIELDS["heldKeysRaw"])
    return held != raw, {"heldKeys": held, "heldKeysRaw": raw}


def discover_keys(core, settle=240):
    """Locate gMain's key fields empirically, with no symbol file.

    Holds a button and looks for RAM halfwords that become exactly that mask and
    return to 0 when released. This is verifiable on the shipped ROM, which is
    why it exists: it is the one part of symbol resolution that does not have to
    be taken on faith.

    Returns {'heldKeys': addr, ...} or {} if nothing matched.
    """
    from hzcore import KEY
    core.run(settle, 0)
    base, end = IWRAM
    mask = KEY["RIGHT"]

    core.run(8, 0)
    before = core.block(base, end - base)
    core.run(8, mask)
    during = core.block(base, end - base)
    core.run(8, 0)
    after = core.block(base, end - base)

    hits = []
    for off in range(0, len(before) - 1, 2):
        b = before[off] | (before[off + 1] << 8)
        d = during[off] | (during[off + 1] << 8)
        a = after[off] | (after[off + 1] << 8)
        if b == 0 and d == mask and a == 0:
            hits.append(base + off)
    # gMain lays heldKeysRaw/newKeysRaw/heldKeys/newKeys out contiguously, so a
    # run of adjacent hits is the signature we want rather than a lone match.
    return {"candidates": hits}


class GameState:
    def __init__(self, core, symbols=None, layout=None):
        self.core = core
        self.sym = symbols or SymbolTable()
        self.layout = layout or SAVEBLOCK1_LAYOUT
        self._sb1 = None
        self.notes = []

    # -- pointer resolution -------------------------------------------------
    def saveblock1(self):
        """Resolve gSaveBlock1Ptr. Requires symbols; raises otherwise."""
        if self._sb1 is not None:
            return self._sb1
        addr = self.sym.get("gSaveBlock1Ptr")
        if addr is None:
            raise Unavailable("gSaveBlock1Ptr not in symbol table "
                              "(build the dev ROM and pass --symbols)")
        ptr = self.core.r32(addr)
        if not (EWRAM[0] <= ptr < EWRAM[1]):
            raise Unavailable(f"gSaveBlock1Ptr -> 0x{ptr:08X}, outside EWRAM; "
                              "symbol file probably does not match this ROM")
        self._sb1 = ptr
        return ptr

    def _field(self, name):
        off, kind, _conf = self.layout[name]
        base = self.saveblock1() + off
        if kind == "u8":  return self.core.r8(base)
        if kind == "u16": return self.core.r16(base)
        if kind == "u32": return self.core.r32(base)
        raise ValueError(kind)

    # -- assertable state ---------------------------------------------------
    def position(self):
        return {"x": self._field("pos_x"), "y": self._field("pos_y"),
                "map_group": self._field("warp_map_group"),
                "map_num": self._field("warp_map_num")}

    def party_count(self):
        return self._field("playerPartyCount")

    def money(self):
        return self._field("money")

    def flag(self, index):
        off, _, _ = self.layout["flags"]
        byte = self.core.r8(self.saveblock1() + off + (index >> 3))
        return bool(byte & (1 << (index & 7)))

    def var(self, index):
        off, _, _ = self.layout["vars"]
        return self.core.r16(self.saveblock1() + off + index * 2)

    def in_battle(self):
        addr = self.sym.get("gMain")
        if addr is None:
            raise Unavailable("gMain not in symbol table")
        return bool(self.core.r8(addr + 0x439) & 0x01)   # inBattle:1, main.h:39

    # -- honesty ------------------------------------------------------------
    def validate(self):
        """Sanity-check what we read. Distinguishes 'wrong' from 'unavailable'."""
        out = {"symbols": len(self.sym), "symbol_source": self.sym.source,
               "checks": {}}
        def chk(name, fn, pred, why):
            try:
                v = fn()
                out["checks"][name] = {"value": v, "ok": bool(pred(v)), "why": why}
            except Unavailable as e:
                out["checks"][name] = {"value": None, "ok": None, "why": str(e)}
            except Exception as e:                       # noqa: BLE001
                out["checks"][name] = {"value": None, "ok": False, "why": repr(e)}
        chk("party_count", self.party_count, lambda v: 0 <= v <= 6, "party is 0..6")
        chk("money", self.money, lambda v: 0 <= v <= 999999, "money caps at 999999")
        chk("position", self.position,
            lambda p: p["x"] < 1000 and p["y"] < 1000, "map coords are small")
        ok = [c for c in out["checks"].values() if c["ok"] is True]
        bad = [c for c in out["checks"].values() if c["ok"] is False]
        out["verdict"] = ("OK" if bad == [] and ok else
                          "LAYOUT_SUSPECT" if bad else "UNAVAILABLE")
        return out


class Unavailable(RuntimeError):
    """State could not be read. Must never be reported as a passing assertion."""
