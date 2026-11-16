"""Drive a fresh boot all the way to the overworld and save a state.

A flat A-mash cannot do this: HnS opens with a settings screen that needs a
deliberate A, and the new-game intro contains a name-entry box where A only
appends a letter and START is what confirms. So this escalates -- when the
screen stops changing, it tries progressively different inputs rather than
pressing harder.
"""
import sys, hzcore

ROM = sys.argv[1]
OUT = sys.argv[2] if len(sys.argv) > 2 else "states/overworld.state"

core = hzcore.Core(ROM)
core.reset()
core.run(300)

# B and SELECT are deliberately absent: B backs OUT of the intro, so including
# it turns the bootstrap into a loop that undoes its own progress -- which is
# exactly what the first attempt did, cycling three screens forever.
ESCALATION = ["A", "A", "START", "A", "DOWN", "A", "START", "A", "RIGHT", "A"]
last, stall, esc = None, 0, 0
history = []

for step in range(900):
    btn = "A" if stall == 0 else ESCALATION[esc % len(ESCALATION)]
    core.tap(btn, hold=6, gap=12)
    core.run(16)
    h = core.screen_hash()
    if h == last:
        stall += 1
        if stall % 3 == 0:
            esc += 1
    else:
        stall, esc = 0, 0
    last = h
    if step % 60 == 0:
        core.screenshot(f"shots/bs_{step:04d}.png")
        history.append((step, core.frame, h[:10]))
        print(f"  step {step:4d} frame {core.frame:6d} screen {h[:10]}")

core.screenshot("shots/bs_final.png")
core.save_state(OUT)
print(f"saved {OUT} at frame {core.frame}")
