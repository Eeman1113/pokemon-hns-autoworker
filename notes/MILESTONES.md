# Milestone testing — any objective, without playing to it

Objectives could previously only be tested by reaching them, so objectives 7–44
had never once been executed. This makes each one directly testable.

**ROM under test: `a35d1a2c`** (pinned snapshot, `harness/milestones/rom/`).
The live ROM was rebuilt three times mid-session (`af522551` → `51086e1c` →
`a35d1a2c`); save states and symbol addresses are both build-specific, so a
moving target cannot be measured. Results below describe that build.

## How a milestone state is made

The GAME's state is set, not the agent's. The agent decides where it is by
running `AW_DetectStage()` over the save file's own flags and vars, so setting
those correctly means the agent arrives at objective N by its own logic.

1. Satisfy every earlier objective's completion condition; ensure N's is not
   already true.
2. Warp via the game's own loader and place the player on the target tile.
3. Clone the starter into a 6-mon over-levelled party.
4. Verify.

## The matrix

| Outcome | Count |
|---|---|
| **COMPLETED** | **6** |
| STALLED — battle hang | 22 |
| STALLED — navigation | 8 |
| COULD_NOT_RUN | 9 |

**The important reading: `AW_STOP_BATTLE_HANG` only fires while `gMain.inBattle`
is true.** The agent therefore *reached and entered* those battles — it
navigated to the trainer successfully and then the battle never terminated.
Of 20 gym/Elite-Four/Champion objectives, **18 were navigated to successfully**
and failed only at the fight itself.

So navigation and the objective table are in far better shape than a raw
"6 of 45 completed" suggests: **28 of 45 objectives reached their target**
(6 completed outright + 22 that got into their battle). Only **8** are genuine
navigation failures, and 3 of those are HM gating (`SURF`, `UNREACH`) rather
than router defects.

Completed outright: 0, 1, 3, 7 (talk/walk), 16 (Ecruteak HM Surf), 18 (Olivine
HM Strength).

Genuine navigation stalls: 2, 10, 13 (no block reason), 19, 22, 23 (`UNREACH`),
21, 26 (`SURF` — HM gating).

The battle hang was subsequently fixed on a later build by driving battles with
synthesised A presses instead of the engine AI. **That fix is not in this
snapshot**, so those 22 should be re-measured; the question they answer here is
"did the agent get there", and the answer is yes.

## Honest limitations

* **9 states could not be switched on.** The start menu is a task drawn over
  the field, so `callback2` never changes and the only way to know it opened is
  `sAW.enabled`. On nine states no menu row enabled the agent. Those objectives
  are untested, not passed.
* **4 of 45 states do not verify.** Objectives 0–1 sit before the base state's
  own progress (it is built by playing to the starter, which already completes
  0–2). Objectives 22 and 26 depend on `HAS_ITEM` conditions, and bag writes
  are not modelled, so their predecessors cannot be satisfied.
* **Battle competence is deliberately NOT tested.** Encounters are suppressed
  and the party is over-levelled, because battles are driven by A-mashing. A
  gym failing on an under-levelled Pokémon would say nothing about navigation.
  Whether mashing A can beat a gym leader is a separate, unanswered question.

## Six wrong-offset bugs, all silent

Each returned a confident wrong number rather than an error:

1. `struct AwObjective` stride is **28, not 24** — `-mabi=apcs-gnu` word-aligns
   structs, so `struct AwCond` is 8 bytes on GBA and 6 on a host.
2. `SaveBlock1.flags` is **0x198C**, not the documented 0x1270.
3. Callback pointers need **bit 0 set** (Thumb). Writing the even symbol
   address stopped the main loop with no error at all.
4. `SetMainCallback2` also **zeroes `gMain.state`**; writing only the pointer
   hangs the map load.
5. `SaveBlock1.pos` takes **map-local** coordinates — the loader adds
   `MAP_OFFSET` itself. Pre-adding it placed the player 7 tiles off on both axes.
6. The stop-reason decode had `NO_ROUTE`/`NAV_BLOCKED` transposed and was
   missing `BATTLE_HANG`, so the single most common failure printed as `10`.

Everything is now compiler-derived (`gbaoffsets.py`, `awlayout.py`) or parsed
from the snapshotted source.

## Two bugs this work found in the harness itself

* **The map check was circular.** `on_target_map` compared against
  `SaveBlock1.location` — the field the generator had just written. It now
  checks `gObjectEvents[0]`, the player sprite's real map, plus an exact
  placement assertion (delta must be `(7,7)`).
* **The enable routine was faking completions.** It tapped A a dozen times to
  clear arrival dialogue, which advanced scripts by itself — objective 6 was
  marked COMPLETED when the A-mash, not the agent, had finished it. Nothing is
  now pressed except the menu sequence.

## Files

```
harness/milestones/
  awtable.py     read the 45-objective table from ROM (ARM-computed layout)
  gbaoffsets.py  SaveBlock1/Pokemon/WarpData offsets via the ARM compiler
  awlayout.py    AwState offsets from the snapshotted source
  snapboot.py    fresh boot -> overworld, pinned to the snapshot
  mkbase.py      base state with a party, agent off
  mkstate.py     build + verify a milestone state for any objective
  runobj.py      run one objective; COMPLETED / STALLED / COULD_NOT_RUN
  locate.py      empirical flags/vars search (superseded, kept as evidence)
  rom/           pinned snapshot.gba + snapshot.map + auto_worker.snapshot.c
  states/        45 milestone states, build-stamped
  matrix.json    the matrix
```

Run: `python3 mkstate.py --from 0 --to 44 && python3 runobj.py --from 0 --to 44`


---

# Addendum — why the gym measurement still does not exist

## The "frozen gym states" were two builds, not a defect

States are built against the PINNED `rom/snapshot.gba` and stamped with its
CRC. Ad-hoc probes loaded them against the LIVE `pokehns-expansion/pokehns.gba`
by calling `core.load_state()` directly, bypassing `buildid.check()`. The
snapshot was `a35d1a2c`; the live build had moved to `d52948ac`. A mismatched
state does not fail cleanly -- it loads, and every address then means something
else. That produced a game frozen on the first frame, reported twice as a
product defect in gym battles. It was neither a gym nor a battle.

`buildid.load_or_die()` now refuses the unsafe path, the snapshot is re-pinned
to `d52948ac`, and `mkstate.py` reloads each state in a FRESH core and asserts
`vblankCounter1` advances before declaring it verified. Objectives 12-18 now
report `agreed / on_target_map / placed_exactly / alive` all true.

## With sound states, navigation is confirmed and the battle is real

From `obj12.state` the agent walks to grid `(16,12)` -- orthogonally adjacent to
Falkner's tile -- and triggers a genuine trainer battle: `gBattleTypeFlags`
`0xc` = `BATTLE_TYPE_TRAINER | bit2`, opponent trainer id **402**. The battle
then PROGRESSES: an opponent faints and the screen reads
"CYNDAQUIL gained 85 Exp. Points!".

(Two of my own misreads, recorded so they are not repeated: `0xc` does **not**
contain `BATTLE_TYPE_FIRST_BATTLE`, which is `1 << 4` = `0x10`; and
`gBattlerControllerFuncs[1]` resolving to `PrintOakText_HowDisappointing+0x19`
is a nearest-symbol artefact of an unnamed static, not evidence of a tutorial
controller.)

## The battle then never ends — and the agent is not the cause

It sticks on that EXP message for **119,600 frames**, identical screen
throughout. Ruled out by measurement, so nobody re-tests them:

| hypothesis | test | result |
|---|---|---|
| the agent's battle driver | human mashes A instead | **sticks identically** |
| A not reaching the game | read `gMain.newKeys` | A delivered ~5x per 60 frames |
| synthetic over-levelled party | clone-only, original level 5 | **sticks identically** |
| `BATTLE_TYPE_FIRST_BATTLE` | decode the real bit values | flag is not set |
| the agent's own watchdog | zero `battleFrames` each tick | still stuck at 119,600 |

Meanwhile `verify.py` is **11/11** on the same build, including
`battles-do-not-freeze-and-ai-is-on` over 18 battles with vblank advancing.

**So battles work in normally-reached states and do not complete in
warp-constructed ones.** The warp drives `CB2_LoadMap` directly rather than
entering the map the way play does, and the most likely consequence is that
some battle-exit path depends on field/script state that this shortcut never
establishes. That is a hypothesis, NOT a measurement -- I did not isolate it.

## What this means

**The gym win/loss table still does not exist, and milestone states as built
cannot produce it.** They remain valid for navigation, which is what the
original matrix measured and why "18 of 20 gym objectives navigated to" stands.
Anything about battle *outcome* from a warped state is untrustworthy.

Two ways forward, neither attempted here:

1. Make the warp use the game's full map-entry path instead of poking
   `CB2_LoadMap`, then re-verify that a battle completes from a warped state
   before trusting any battle result.
2. Measure gyms only from states reached by actual play. Correct by
   construction, but gated on the live horizon, which is objective 11.

**Add a battle-completion assertion to state verification.** A state that
cannot finish a battle should be marked unusable for battle tests, exactly as
the liveness check now marks a dead state -- otherwise the next person measures
battle competence through a broken state and believes the number.
