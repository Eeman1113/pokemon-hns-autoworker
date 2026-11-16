#!/usr/bin/env python3
"""bootstate.py — generate save states without a human.

Only the fresh-boot states can be made this way; anything past character
creation needs a real playthrough (see HARNESS.md). The script screenshots every
checkpoint so the state it produced can actually be verified rather than
assumed -- a save state that silently stopped on the wrong screen would make
every scenario built on it meaningless.
"""
import argparse, json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hzcore

HERE = os.path.dirname(os.path.abspath(__file__))
STATES = os.path.join(HERE, "states")
SHOTS = os.path.join(HERE, "shots")


def mash(core, button, frames, hold=6, gap=6):
    """Advance through unskippable intro text without holding a key down, which
    many menus read as a single press rather than repeated ones."""
    done = 0
    while done < frames:
        core.tap(button, hold, gap)
        done += hold + gap


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", default="~/Downloads/Pokemon Heart and Soul v2.0.6.gba")
    ap.add_argument("--checkpoints", type=int, default=6)
    ap.add_argument("--frames-per-checkpoint", type=int, default=900)
    args = ap.parse_args()

    os.makedirs(STATES, exist_ok=True)
    os.makedirs(SHOTS, exist_ok=True)

    manifest = []
    with hzcore.Core(args.rom) as core:
        core.run(300)
        core.screenshot(os.path.join(SHOTS, "cp00_boot.png"))
        core.save_state(os.path.join(STATES, "cp00_boot.state"))
        manifest.append({"checkpoint": 0, "label": "boot",
                         "frame": core.frame, "hash": core.screen_hash(),
                         "state": "states/cp00_boot.state",
                         "shot": "shots/cp00_boot.png"})

        for i in range(1, args.checkpoints + 1):
            mash(core, "A", args.frames_per_checkpoint)
            shot = os.path.join(SHOTS, f"cp{i:02d}.png")
            stt = os.path.join(STATES, f"cp{i:02d}.state")
            core.screenshot(shot)
            core.save_state(stt)
            manifest.append({"checkpoint": i, "label": f"after {i} A-mash blocks",
                             "frame": core.frame, "hash": core.screen_hash(),
                             "state": os.path.relpath(stt, HERE),
                             "shot": os.path.relpath(shot, HERE)})
            print(f"  checkpoint {i}: frame {core.frame}, screen {core.screen_hash()}")

    path = os.path.join(STATES, "manifest.json")
    with open(path, "w") as fh:
        json.dump({"rom": args.rom, "checkpoints": manifest}, fh, indent=1)
    print(f"\nwrote {len(manifest)} checkpoints -> {path}")
    print("Inspect the PNGs in shots/ to see which screen each state actually "
          "reached before relying on it.")


if __name__ == "__main__":
    main()
