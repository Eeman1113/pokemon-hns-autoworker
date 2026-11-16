#!/usr/bin/env python3
"""run.py — scenario runner for the Auto Worker agent.

Scenarios are DATA (scenarios.json), not code, so adding one costs a few lines
of JSON rather than a new script.

Three outcomes, never conflated:
  PASS          assertions ran and held
  FAIL          assertions ran and did not hold
  COULD_NOT_RUN a precondition was missing (emulator, ROM, save state, symbols)

The third exists because a harness that reports a missing save state as a pass
is worse than no harness. Exit code is 0 only if there are no FAILs and no
COULD_NOT_RUNs.
"""
import argparse, json, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hzcore
import state as st

PASS, FAIL, SKIP = "PASS", "FAIL", "COULD_NOT_RUN"
DEFAULT_ROM = "~/Downloads/Pokemon Heart and Soul v2.0.6.gba"


class Ctx:
    """Everything an assertion may look at."""
    def __init__(self, core, gmain, gs):
        self.core, self.gmain, self.gs = core, gmain, gs
        self.events = []

    def gm(self, field):
        off = st.GMAIN_FIELDS[field]
        if field == "inBattle":
            return self.core.r8(self.gmain + off) & 1
        if field == "vblankCounter1":
            return self.core.r32(self.gmain + off)
        return self.core.r16(self.gmain + off)


# ---- step verbs -----------------------------------------------------------
def step_run(ctx, frames=60, keys=""):
    mask = ctx.core.keys_from(keys.split("+")) if keys else 0
    ctx.core.run(int(frames), mask)


def step_tap(ctx, button="A", times=1, hold=4, gap=6):
    for _ in range(int(times)):
        ctx.core.tap(button, int(hold), int(gap))


def step_load_state(ctx, path=""):
    if not path or not os.path.exists(os.path.expanduser(path)):
        raise hzcore.EmulatorUnavailable(f"save state missing: {path}")
    ctx.core.load_state(path)


def step_record(ctx, name="sample"):
    ok, detail = st.injection_signature(ctx.core, ctx.gmain)
    ctx.events.append({"name": name, "injecting": ok, **detail,
                       "frame": ctx.core.frame})


STEPS = {"run": step_run, "tap": step_tap, "load_state": step_load_state,
         "record": step_record}


# ---- assertions -----------------------------------------------------------
def a_no_injection(ctx, **_):
    ok, d = st.injection_signature(ctx.core, ctx.gmain)
    return (not ok), f"heldKeys=0x{d['heldKeys']:X} heldKeysRaw=0x{d['heldKeysRaw']:X}"


def a_injection_present(ctx, **_):
    ok, d = st.injection_signature(ctx.core, ctx.gmain)
    return ok, f"heldKeys=0x{d['heldKeys']:X} heldKeysRaw=0x{d['heldKeysRaw']:X}"


def a_in_battle(ctx, expected=True, **_):
    v = bool(ctx.gm("inBattle"))
    return v == bool(expected), f"inBattle={v}"


def a_frames_advanced(ctx, at_least=1, **_):
    v1 = ctx.gm("vblankCounter1")
    ctx.core.run(int(at_least) + 2)
    v2 = ctx.gm("vblankCounter1")
    return (v2 - v1) >= int(at_least), f"vblank {v1}->{v2}"


def a_party_count(ctx, min=0, max=6, **_):
    v = ctx.gs.party_count()          # raises Unavailable without symbols
    return int(min) <= v <= int(max), f"party={v}"


def a_flag_set(ctx, index=0, expected=True, **_):
    v = ctx.gs.flag(int(index))
    return v == bool(expected), f"flag[{index}]={v}"


def a_position(ctx, map_group=None, map_num=None, **_):
    p = ctx.gs.position()
    ok = ((map_group is None or p["map_group"] == map_group) and
          (map_num is None or p["map_num"] == map_num))
    return ok, str(p)


ASSERTS = {"no_injection": a_no_injection, "injection_present": a_injection_present,
           "in_battle": a_in_battle, "frames_advanced": a_frames_advanced,
           "party_count": a_party_count, "flag_set": a_flag_set,
           "position": a_position}


# ---- driver ---------------------------------------------------------------
def run_scenario(sc, rom, symbols=None, wall_timeout=120.0):
    res = {"name": sc["name"], "why": sc.get("why", ""), "status": None,
           "detail": [], "frames": 0, "seconds": 0.0}
    t0 = time.time()
    core = None
    try:
        core = hzcore.Core(rom)
        core.run(int(sc.get("boot_frames", 300)))
        gmain, ev = st.discover_gmain(core)
        if gmain is None:
            raise hzcore.EmulatorUnavailable(f"could not locate gMain: {ev}")
        res["gmain"] = hex(gmain)

        symtab = st.SymbolTable(symbols) if symbols else st.SymbolTable()
        gs = st.GameState(core, symtab)
        ctx = Ctx(core, gmain, gs)

        budget = int(sc.get("frame_budget", 60 * 60 * 10))
        for stp in sc.get("steps", []):
            kind = stp.pop("step") if "step" in stp else stp.pop("do")
            if kind not in STEPS:
                raise hzcore.EmulatorUnavailable(f"unknown step {kind!r}")
            STEPS[kind](ctx, **stp)
            if core.frame > budget:
                res["status"] = FAIL
                res["detail"].append({"assert": "frame_budget",
                                      "ok": False,
                                      "info": f"exceeded {budget} frames"})
                return _finish(res, core, t0)
            if time.time() - t0 > wall_timeout:
                res["status"] = FAIL
                res["detail"].append({"assert": "wall_timeout", "ok": False,
                                      "info": f"exceeded {wall_timeout}s"})
                return _finish(res, core, t0)

        allok = True
        for asrt in sc.get("assert", []):
            a = dict(asrt)
            kind = a.pop("that")
            if kind not in ASSERTS:
                raise hzcore.EmulatorUnavailable(f"unknown assertion {kind!r}")
            try:
                ok, info = ASSERTS[kind](ctx, **a)
            except st.Unavailable as e:
                res["status"] = SKIP
                res["detail"].append({"assert": kind, "ok": None, "info": str(e)})
                return _finish(res, core, t0)
            allok &= ok
            res["detail"].append({"assert": kind, "ok": ok, "info": info})
        res["events"] = ctx.events
        res["status"] = PASS if allok else FAIL
    except (hzcore.EmulatorUnavailable, FileNotFoundError) as e:
        res["status"] = SKIP
        res["detail"].append({"assert": "precondition", "ok": None, "info": str(e)})
    except Exception as e:                                   # noqa: BLE001
        res["status"] = FAIL
        res["detail"].append({"assert": "exception", "ok": False, "info": repr(e)})
    return _finish(res, core, t0)


def _finish(res, core, t0):
    if core:
        res["frames"] = core.frame
        core.close()
    res["seconds"] = round(time.time() - t0, 2)
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", default=DEFAULT_ROM)
    ap.add_argument("--scenarios", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "scenarios.json"))
    ap.add_argument("--symbols", default=None,
                    help="pokehns.sym / .map from the dev build")
    ap.add_argument("--only", default=None, help="substring filter on name")
    ap.add_argument("--json", default=None, help="write machine-readable results")
    ap.add_argument("--timeout", type=float, default=120.0)
    args = ap.parse_args()

    with open(args.scenarios) as fh:
        scenarios = json.load(fh)["scenarios"]
    if args.only:
        scenarios = [s for s in scenarios if args.only.lower() in s["name"].lower()]

    results = [run_scenario(dict(s), args.rom, args.symbols, args.timeout)
               for s in scenarios]

    width = max((len(r["name"]) for r in results), default=10)
    print(f"\n{'SCENARIO'.ljust(width)}  STATUS          FRAMES   TIME")
    print("-" * (width + 34))
    for r in results:
        print(f"{r['name'].ljust(width)}  {r['status']:<14}  {r['frames']:>6}  "
              f"{r['seconds']:>5}s")
        for d in r["detail"]:
            if d["ok"] is not True:
                mark = "!" if d["ok"] is False else "-"
                print(f"{' ' * width}   {mark} {d['assert']}: {d['info']}")

    n = {k: sum(1 for r in results if r["status"] == k) for k in (PASS, FAIL, SKIP)}
    print(f"\n{n[PASS]} passed, {n[FAIL]} failed, {n[SKIP]} could not run")
    if args.json:
        with open(args.json, "w") as fh:
            json.dump({"results": results, "summary": n}, fh, indent=1)
        print(f"wrote {args.json}")
    return 0 if (n[FAIL] == 0 and n[SKIP] == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
