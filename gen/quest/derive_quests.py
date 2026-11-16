#!/usr/bin/env python3
"""
derive_quests.py - derive the Auto Worker objective chain from the game's own scripts.

The brief assumed quest progression is a flag DAG. It is not. This codebase drives
progression primarily through per-area STATE MACHINES held in VAR_*_STATE variables
(1,392 setvar / 1,262 goto_if_eq, against 901 setflag / 349 goto_if_set). The analyser
therefore models BOTH:

  * var-state edges : script S on map M, reachable when VAR == a, performs setvar VAR = b
                      -> a transition a -> b, located at M, triggered by object O at (x,y)
  * flag edges      : script S sets FLAG F; scripts gating on F are downstream

The main line is then the subgraph that moves badge / HM / region-unlock state forward.
"""
import json, os, re, sys, glob
from collections import defaultdict, OrderedDict

REPO = "/Users/eemanmajumder/hns/pokehns-expansion"
MAPS = os.path.join(REPO, "data", "maps")

# ---------------------------------------------------------------- script parsing
LABEL_RE   = re.compile(r'^([A-Za-z_][A-Za-z0-9_]*)::')
# "goto_if_eq VAR, value, Label"  /  map_script_2 VAR, value, Label
GOTO_EQ_RE = re.compile(r'^\s*(?:goto_if_eq|call_if_eq|map_script_2)\s+(VAR_[A-Z0-9_]+)\s*,\s*([0-9]+)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)')
SETVAR_RE  = re.compile(r'^\s*setvar\s+(VAR_[A-Z0-9_]+)\s*,\s*([0-9]+)')
ADDVAR_RE  = re.compile(r'^\s*addvar\s+(VAR_[A-Z0-9_]+)\s*,\s*([0-9]+)')
SETFLAG_RE = re.compile(r'^\s*setflag\s+(FLAG_[A-Z0-9_]+)')
CLRFLAG_RE = re.compile(r'^\s*clearflag\s+(FLAG_[A-Z0-9_]+)')
IFSET_RE   = re.compile(r'^\s*(?:goto_if_set|call_if_set)\s+(FLAG_[A-Z0-9_]+)')
IFUNSET_RE = re.compile(r'^\s*(?:goto_if_unset|call_if_unset)\s+(FLAG_[A-Z0-9_]+)')
GOTO_RE    = re.compile(r'^\s*(?:goto|call|goto_if_eq|call_if_eq|goto_if_set|goto_if_unset|call_if_set|call_if_unset)\s+(?:[^,]+,\s*)*([A-Za-z_][A-Za-z0-9_]*)\s*$')
GIVEITEM_RE= re.compile(r'^\s*giveitem\s+(ITEM_[A-Z0-9_]+)')
TRAINER_RE = re.compile(r'^\s*trainerbattle\w*\s+(?:[A-Z_]+,\s*)?(TRAINER_[A-Z0-9_]+)')
COMPARE_RE = re.compile(r'^\s*compare\s+(VAR_[A-Z0-9_]+)\s*,\s*([0-9]+)')

class Script:
    __slots__ = ("name","map","lineno","sets_var","reads_var","sets_flag","clears_flag",
                 "checks_flag","gives_item","trainers","calls","raw")
    def __init__(self, name, mapname, lineno):
        self.name, self.map, self.lineno = name, mapname, lineno
        self.sets_var, self.reads_var = [], []
        self.sets_flag, self.clears_flag, self.checks_flag = [], [], []
        self.gives_item, self.trainers, self.calls = [], [], []
        self.raw = []

def parse_map_scripts(path, mapname):
    scripts, cur = OrderedDict(), None
    with open(path, errors="replace") as fh:
        for n, line in enumerate(fh, 1):
            m = LABEL_RE.match(line)
            if m:
                cur = Script(m.group(1), mapname, n)
                scripts[cur.name] = cur
                continue
            if cur is None:
                continue
            cur.raw.append(line)
            for rx, bucket in ((SETFLAG_RE, cur.sets_flag), (CLRFLAG_RE, cur.clears_flag),
                               (IFSET_RE, cur.checks_flag), (IFUNSET_RE, cur.checks_flag),
                               (GIVEITEM_RE, cur.gives_item), (TRAINER_RE, cur.trainers)):
                mm = rx.match(line)
                if mm: bucket.append(mm.group(1))
            mm = SETVAR_RE.match(line) or ADDVAR_RE.match(line)
            if mm: cur.sets_var.append((mm.group(1), int(mm.group(2))))
            mm = GOTO_EQ_RE.match(line)
            if mm:
                cur.reads_var.append((mm.group(1), int(mm.group(2))))
                cur.calls.append(mm.group(3))
            else:
                mm = COMPARE_RE.match(line)
                if mm: cur.reads_var.append((mm.group(1), int(mm.group(2))))
                mm = GOTO_RE.match(line)
                if mm: cur.calls.append(mm.group(1))
    return scripts

# ---------------------------------------------------------------- map metadata
def load_maps():
    out = {}
    for d in sorted(glob.glob(os.path.join(MAPS, "*"))):
        mj, si = os.path.join(d, "map.json"), os.path.join(d, "scripts.inc")
        if not os.path.exists(mj): continue
        try: data = json.load(open(mj))
        except Exception: continue
        name = os.path.basename(d)
        objs = {}
        for o in data.get("object_events", []) or []:
            s = o.get("script") or ""
            if s and s != "0" and s != "NULL":
                objs.setdefault(s, []).append((o.get("x"), o.get("y"), o.get("graphics_id","")))
        for b in (data.get("bg_events") or []):
            s = b.get("script") or ""
            if s and s not in ("0","NULL"):
                objs.setdefault(s, []).append((b.get("x"), b.get("y"), "SIGN"))
        scripts = parse_map_scripts(si, name) if os.path.exists(si) else OrderedDict()
        out[name] = dict(id=data.get("id",""), name=name, objs=objs, scripts=scripts,
                         warps=len(data.get("warp_events") or []),
                         region=data.get("region_map_section",""),
                         map_type=data.get("map_type",""))
    return out

# ---------------------------------------------------------------- graph building
def build(mapdata):
    # resolve reachable sub-scripts so a setvar in a called label is attributed
    # to the entry script the player actually interacts with
    def closure(mp, root, depth=0, seen=None):
        seen = seen if seen is not None else set()
        if root in seen or depth > 8: return seen
        seen.add(root)
        s = mp["scripts"].get(root)
        if s:
            for c in s.calls: closure(mp, c, depth+1, seen)
        return seen

    var_edges   = []   # (var, from_state, to_state, map, entry_script, coords)
    flag_set    = defaultdict(list)
    flag_check  = defaultdict(list)
    item_gives  = []
    for name, mp in mapdata.items():
        for entry, coords in mp["objs"].items():
            reach = closure(mp, entry)
            gate = []
            root = mp["scripts"].get(entry)
            for lbl in reach:
                s = mp["scripts"].get(lbl)
                if not s: continue
                for (v, val) in s.reads_var: gate.append((v, val))
                for (v, val) in s.sets_var:
                    froms = [g[1] for g in gate if g[0] == v]
                    var_edges.append(dict(var=v, frm=(froms[0] if froms else None), to=val,
                                          map=name, script=entry, coords=coords[0]))
                for f in s.sets_flag: flag_set[f].append((name, entry, coords[0]))
                for f in s.checks_flag: flag_check[f].append((name, entry))
                for it in s.gives_item: item_gives.append((it, name, entry, coords[0]))
        # map-level scripts (OnTransition / OnFrame) have no coordinates
        for lbl, s in mp["scripts"].items():
            if lbl in mp["objs"]: continue
            for f in s.sets_flag: flag_set[f].append((name, lbl, None))
            for f in s.checks_flag: flag_check[f].append((name, lbl))
    return var_edges, flag_set, flag_check, item_gives

if __name__ == "__main__":
    md = load_maps()
    ve, fs, fc, ig = build(md)
    hns = {k: v for k, v in md.items() if k.endswith("_hns")}
    print(f"maps parsed            : {len(md)} ({len(hns)} HnS)")
    print(f"scripts parsed         : {sum(len(m['scripts']) for m in md.values())}")
    print(f"var state transitions  : {len(ve)}")
    print(f"distinct flags set     : {len(fs)}")
    print(f"distinct flags checked : {len(fc)}")
    print(f"giveitem sites         : {len(ig)}")
    json.dump(dict(var_edges=ve,
                   flag_set={k: v for k, v in fs.items()},
                   flag_check={k: v for k, v in fc.items()},
                   item_gives=ig),
              open("/Users/eemanmajumder/hns/gen/quest/graph.json","w"), indent=1)
    print("wrote gen/quest/graph.json")
