# Auto Worker — long-horizon progress log

Each entry is a stall observed in the emulator, its real cause, and what was
done. "Observed" means read out of live memory, not inferred.

Metric, from a fresh-boot state, 12,000 frames:

| Run | tiles | maps | objectives reached |
|-----|-------|------|--------------------|
| start of session | 10 | 2 | 0 |
| after stall 1 fix | 15 | 3 | 0 → 1 |
| after stall 2 fix | 34 | 4 | 1 (reaches the Lab) |
| after stall 3 fix | 38 | 4 | 3 of 37 (starter obtained) |
| after stall 4 + audit table | 63 | 5 | 5 of 39 (on Route 29) |
| after stall 6 root-cause fix | 110 | 5 | 5 of 39 (Route 29, battles survived) |
| after stall 7 fix + quest3 | 140 | 7 | 7 of 44 (Mr. Pokemon's house) |
| healing cycle | 142 | 5 | 7 of 44 (stall 8 unfixed) |
| determinism + stall 8 | **331** | **6** | **8 of 44** |

---

## Stall 1 — objective 0 never completed (FIXED)

**Observed:** agent reached objective 0's tile (Mom, `PLAYERS_HOUSE_1F`),
interacted, `sawScript=1`, and the stage never advanced. ~18,000 of 20,000
frames spent oscillating `INTERACT → ADVANCE → INTERACT` at (16,11).

**Cause:** objective 0's completion test was `VAR_NEWBARK_TOWN_STATE >= 5`.
Mom's script sets that variable to **2**
(`NewBarkTown_PlayersHouse_1F_hns/scripts.inc:228`). The value 5 is set much
later by `MomEventScript_GoodLuck` (`:45`), three objectives downstream. The
generator had inferred the threshold as the *maximum* value the variable ever
reaches rather than the value this objective's own script produces — the table
even carried the comment "threshold inferred".

The agent was waiting for a state it could only reach after completing the step
it was stuck on.

**Fixed:** threshold corrected to 2 in `gen/quest/gen_table.py` (regenerated,
confidence raised INFERRED → DERIVED with the source line cited).

**Also fixed, generally:** mis-inferred thresholds are the least trustworthy
part of the whole table, so they no longer have to be right. The agent now
snapshots the completion variable when an objective begins; if the variable
changes at all while a script of ours ran, the objective counts as done. A wrong
threshold now costs one redundant interaction instead of stalling forever.

---

## Stall 2 — agent dies the moment it leaves the house (FIXED)

**Observed:** objective 0 completes, the agent walks out of the front door into
New Bark Town (map 0,0), and ~80 frames later disables itself at (27,19).

`stopReason=NAV_BLOCKED`, `navRet=BLOCKED`, `blockReason=UNREACHABLE`,
`hereIdx=0` (correct — New Bark Town), `SetGoal(mapId=256, g=1, n=0)` (correct —
the Lab).

**Ruled out by evidence, so nobody needs to re-check these:**

- *The world graph.* A host build of the shipped `world_graph.c` gives
  `dist[NewBarkTown]=1` for a backward BFS from the Lab. The edge exists and the
  BFS is correct.
- *The objective table.* Read out of ROM at the real 28-byte stride:
  `obj[1].mapId = 256` = `MAP_NEW_BARK_TOWN_LAB_HNS`. Correct.
- *`SetGoal`'s arguments.* Recorded in `sAW` at call time: `g=1, n=0`. Correct.
- *A transient post-warp state.* The runner now retries a failing navigator 60
  times, re-planning each time, before giving up. It still fails all 60.
- *The two `UNREACHABLE` branches in `AW_Nav_Tick`.* Both instrumented; neither
  fires (`dbgWhich=0`).

**Therefore:** the failure is in the **intra-map search on New Bark Town**, which
reports a `sawReason`-derived `UNREACHABLE` (`auto_worker_nav.c:585,630`) rather
than one of the explicit branches. The agent is at grid (27,19); the Lab door is
warp 0 at local (10,9) = grid (17,16) — comfortably inside the 32×32 window.

**Next step for whoever picks this up:** instrument `ChooseIntraTarget()` and
`sawReason`. The two most likely candidates, in order:

1. `ChooseIntraTarget` selects the wrong warp. New Bark Town has **two** warps
   to the Lab (warp 0 at (10,9) and warp 6 at (12,7)) plus two developer warps
   to `TEST_MAP1_HNS` and one to `TIN_TOWER_ROOF_DAY_HNS`. If gradient descent
   picks a sink warp, or a warp tile that is itself walled off, the search has
   no reachable target.
2. The "every warp tile that is not the current target is treated as a wall"
   rule. In a town whose every building door is a warp, that rule removes a lot
   of tiles; if the *chosen* target warp is not correctly exempted, the search
   can never reach it.

---

## Stall 2 — resolution

**The inter-map layer was never at fault.** Once the ROM reported its own values
(`AW_Nav_DebugDistGoal()` etc.) instead of the harness guessing offsets, it read
`goalIdx=71, dist[goal]=0, dist[here]=1` — exactly right. Four separate
hand-derived `sNav` offsets had been wrong, each returning a confident number.
*Every* nav diagnostic is now reported by the ROM.

**The real cause: Gen 3 building doors are impassable tiles with a warp
attached.** The player never occupies one — they stand on the approach tile and
press into it. The navigator aimed the intra-map BFS *at* the door tile, so the
search seeded one cell and expanded nothing (`qHead == qTail == 1`).

Proven rather than argued: a probe of all four approaches to New Bark Town's Lab
door returned `0x2222` — `IMPASSABLE` from every side — while the same probe
aimed at the player's own neighbours returned `0x0020`, correctly reporting
three walkable sides and one wall. The probe was fine; the geometry really was
a wall.

**Fix:** `ChooseIntraTarget` now probes a warp tile's four approaches. If any is
enterable it aims at the warp directly (staircases, ladders, floor warps —
unchanged). If all are `IMPASSABLE` it aims at the tile below and records
`doorDir`, and the tick presses into the door on arrival.

**The first version of that fix regressed everything to 5 tiles / 1 map**, and
the regression taught the real rule: it tested `col == COLLISION_NONE`, so the
2F staircase *the player is standing on* reported `COLLISION_OBJECT_EVENT` —
the probe is a stranger to `DoesObjectCollideWithObjectAt` and sees the player
as an obstacle. The staircase was reclassified as a door and the agent walked
off it forever. Only `IMPASSABLE` means "wall"; `OBJECT_EVENT` means "something
movable is there". Also made `doorDir` clear on every hop, not only warp hops.

---

## Stall 3 — reaches Elm, never gets the starter (OPEN)

**Observed:** agent crosses New Bark Town, enters the Lab (map 1,0) at frame
~7,200, walks to (13,11) — one tile below Elm — and stays in `AW_ADVANCE` for
the remaining ~58,000 frames. `party=0` throughout. It does not disable: the
dialogue branch resets `noProgress`, so neither watchdog trips.

**Likely cause — a quest-table gap, not an engine bug.** Objective 1 is a single
`TALK_TO` Elm with completion `FLAG_SYS_POKEMON_GET`. But receiving the starter
needs *two* steps: talk to Elm, then walk to one of three Pokéballs and press A.
There is no objective for the second, so the agent talks to a finished Elm
forever.

**Next step:** split objective 1 into `TALK_TO Elm` then `TAKE_ITEM`/`TALK_TO`
at a starter ball, in `gen/quest/gen_table.py`. Also worth fixing generally:
the `AW_ADVANCE` branch resets `noProgress`, so a lock→unlock→lock dialogue
loop escapes both watchdogs. A loop that returns to the same tile and the same
objective for N thousand frames should trip something.

---

## Stall 3 — reaches Elm, never gets the starter (FIXED)

**Cause:** objective 1 was a single `TALK_TO Elm` whose completion was
`FLAG_SYS_POKEMON_GET` — but that flag is set by the *Pokéball* script, not by
Elm. Talking to Elm sets `VAR_NEWBARKTOWN_LABSTATE = 1`
(`NewBarkTown_Lab_hns/scripts.inc:142`), and only then do the three ball scripts
at (8,4) (9,4) (10,4) let you take one.

**Fix: split into two objectives, not a new verb.** Obtaining the starter is two
interactions separated by navigation — different targets, different completion
tests. That is two objectives, and the table already sequences objectives. A
`AW_TAKE_FROM_BALL` verb would encode a sequence inside one row, needing its own
second-target fields: a row that is secretly two rows.

**The generalisable rule this exposed, which matters for the other 34:** *a
completion test belongs to the objective whose own script sets it.* Attributing
a downstream flag to an upstream step makes that step permanently incomplete.
Worth checking every objective whose completion flag is set on a different map
or by a different NPC than its target.

**Result:** objectives 0, 1 and 2 now complete; the agent reaches stage 3.
Table regenerated: **37 objectives, 25 DERIVED / 8 INFERRED / 4 UNKNOWN.**

---

## Watchdog hole (FIXED, then fixed again)

The dialogue branch resets `noProgress` explicitly and `dialogueFrames` resets
whenever the field unlocks, so a lock → unlock → lock loop escaped both. Added
`AW_STOP_BACKSTOP`: a watchdog evaluated in `AutoWorker_Tick` before any
per-mode branch can touch it.

**The first version was also wrong, and stall 4 proved it.** It reset on a
changed tile — but an agent bouncing between two tiles changes tile every step,
so it never fired. Observed at the Lab exit: 145,000 frames oscillating
(13,14) ↔ (13,15). The backstop now resets **only** on a new objective or a new
map. Crossing the largest map in the game costs far less than the 18,000-frame
budget, so honest travel is never punished. Verified firing at `wd=18001`.

---

## Stall 4 — cannot leave Elm's Lab (OPEN)

**Observed:** at stage 3 (Cherrygrove, goal grid (64,15)) the agent oscillates
between (13,14) and (13,15) on map (1,0), pressing DOWN, alternating
TRAVEL/ADVANCE. It now hands back after 18,000 frames instead of looping
forever.

The alternation with `ADVANCE` says a script fires on one of those tiles — most
likely Elm's aide stopping the player on the way out (there is an
`ElmAideIsWaiting` script), or the exit door triggering dialogue that pushes the
player back. Next step: identify which object owns that script and whether the
exit needs an intervening objective, exactly as the starter did.

---

## Stall 4 — cannot leave Elm's Lab (FIXED, two objectives)

Confirmed from the coord-event index rather than the oscillation pattern.
`NewBarkTown_Lab_hns` has triggers at local (5,8)/(6,8)/(7,8) — grid (12-14,15),
exactly the observed bounce tiles — firing `TriggerReturnPlayer` while
`VAR_NEWBARKTOWN_LABSTATE == 1`. Taking the ball leaves LABSTATE at 1, so the
exit pushed the player back forever.

It is **two** missing steps, not one:
1. **Talk to Elm again** → `MrPokemon:295` sets LABSTATE 2 *and*
   `VAR_NEWBARK_TOWN_STATE` 3.
2. **Step on the exit tile** → coord event at (6,12), `TriggerAideGivePotion`,
   sets LABSTATE 3.

Checked for chained gates at the same time, as they are cheaper to find
together: New Bark Town's own leave-triggers at (0,12)-(0,14) fire only at
`VAR_NEWBARK_TOWN_STATE` 2 or 4. Elm sets 3, so **no further gate blocks the
road out** — one step unlocked both the Lab door and the town exit.

Rule from the audit, which pairs with the completion-test rule:
**a coord trigger on a warp tile is an objective, not scenery.**

---

## Stall 5 — map connections could never be crossed (FIXED)

With the Lab open, the agent reached New Bark Town and immediately returned
`NAV_BLOCKED/UNREACH`. Route 29 is reached by a *connection*, not a warp, and
`ChooseIntraTarget` deliberately placed the target OUTSIDE the map so the window
seeding would walk toward the border. But when that phantom tile happened to
fall inside the 32x32 window, `SeedGoal` treated it as a real cell and seeded
one out-of-map tile — the identical "one seed, no expansion" failure as aiming
at a door.

**Fix:** connections now aim at the last tile *inside* the map on that side and
record the direction to press on arrival, reusing the door machinery. `SeedGoal`
seeds the whole in-window boundary line rather than a single tile, so one
blocked cell on the player's own row cannot sink a crossing.

Verified: the agent now crosses into `map (0,11)` — Route 29.

---

## Stall 6 — a battle that never ends (BOUNDED, not solved)

A battle began at frame 11,931 and `inBattle` stayed true for the remaining
190,000 frames. The overworld backstop deliberately ignores battles, so this was
an unbounded hang.

Bounded with `AW_STOP_BATTLE_HANG` at 18,000 frames. On re-run the agent in fact
battles repeatedly and recovers each time — it is wild encounters on Route 29,
which is correct behaviour while travelling — so the single long battle may have
been a one-off. **The watchdog is a bound, not a diagnosis:** if a battle really
can hang, the cause is still unknown and worth a cycle once it recurs.

NPC-initiated battles are therefore *partly* answered: the agent survives
battles it did not choose to start, and the engine AI drives them. The rival
fight has not yet been reached.

---

## Stall 6 — ROOT CAUSE FOUND: auto-battling HANGS the game in wild battles

Last cycle this was bounded but undiagnosed. It recurred, so it was diagnosed.

**It is not a stall, it is a hard freeze.** `gMain.vblankCounter1` stops advancing
entirely (frozen at 11913 while the emulator ran 17231 -> 18731). The game's main
loop never runs again, which is why *nothing* incremented — not the position, not
`battleFrames`, not even the watchdogs. No in-ROM watchdog can ever fire, because
no in-ROM code runs at all.

**Proven by A/B, not inferred.** Same save state, same route, one line changed:

| build | result |
|---|---|
| `AutoWorker_IsAutoBattling()` as shipped | **GAME HUNG** — vblank frozen |
| same predicate forced `FALSE` | **GAME RUNNING** — vblank 11918 -> 12518 |

**Cause:** `SetControllerToPlayerPartner` was written for a partner in a trainer
or multi battle. Pointing it at the player's battler in a **wild** battle hangs
the engine outright.

**Fix:** `AutoWorker_LatchForBattle()` now additionally requires
`gBattleTypeFlags & BATTLE_TYPE_TRAINER`. Wild battles keep the normal player
controller and are driven by synthesised A presses (FIGHT, then the first move,
then message clears). Weaker play than the AI, but a hang is unrecoverable and a
weak turn is not.

**Result:** wild battles are now fought and survived — two in the measured run,
party intact, no white-out. Distinct tiles jumped 63 -> 110 purely because the
agent stops freezing and keeps exploring.

**Implication for the trainer/rival fight, stated honestly:** auto-battling is
now only exercised in trainer battles, and the rival fight has still not been
reached. The partner controller may be fine there — that is what it was written
for — but it is untested.

---

## Stall 7 — window thrashing on Route 29 (OPEN)

At stage 5 with goal (64,15) (Cherrygrove), the agent walks west to x~39, then
back east to x~50, repeatedly, alternating `navRet` LEFT and RIGHT, `blk=NONE`
throughout. It never approaches the west boundary (x=7) where the Cherrygrove
connection lies. The backstop now catches it at 18,000 frames.

Route 29 is wider than the 32-tile search window, so the connection target is
outside it and the edge-seed path is used: walk to the window edge, recentre,
search again. The oscillation says the recentred search sends it back the way it
came. Suspects, in order: the `failedSearchX/Y` recentre guard tripping and
falling into the 60-retry re-plan, which re-runs `ChooseIntraTarget` and may pick
the opposite connection; or the edge seeding picking the wrong window edge.

This is the **fourth** bug in the navigator's model of leaving somewhere, and the
first in the long-crossing path rather than the door/connection path.

---

## Stall 7 — window thrashing on Route 29 (FIXED — my own regression)

Cause was the connection fix from the previous cycle. The connection target used
the PLAYER's cross-axis coordinate (`sNav.tgtY = py` for a west crossing), so the
target drifted every time the player stepped sideways. `targetMoved` then fired
on nearly every tick and restarted the window search before it could finish, so
the gradient was rebuilt from scratch forever and the half-built field walked the
player back and forth — the x39<->x50 thrash.

**Fix:** hold the target still for the duration of one hop (reuse the previous
coordinates while the connection direction is unchanged). `SeedGoal` already
seeds the whole border line, so the exact cross-axis value never mattered.

Diagnosed by inspection rather than a host repro: the trace showed the player's
y oscillating 31<->32, which is exactly the input that makes a py-derived target
move, so the mechanism was identifiable directly from the code.

**Result:** Route 29 crossed. Cherrygrove City, Route 30 and Mr. Pokemon's house
all reached. The windowed edge-seed chain works for a long east-west crossing
once the target stops moving — no larger window or corridor pass needed, so the
EWRAM headroom is not under pressure.

---

## Trainer battle under the AI controller — TESTED, and it works

The open question since the wild-battle freeze: auto-battling now runs *only* in
trainer battles, and that path had never been exercised.

**Battle #4 of the run, frame 22131, `battleLatch = 1` (AI-driven), on Route 30.
It resolved and the agent continued to Mr. Pokemon's house.** No freeze, no hang.

So the split is sound: the partner controller works in the battle type it was
written for and freezes in the one it was not. Auto-battling is usable, and the
product-level worst case the coordinator raised — "auto-battling cannot be used
at all" — does not apply.

Three wild battles in the same run ran on the key-press path and were survived;
party stayed at 1 throughout and **no white-out has yet occurred**.

---

## Stall 8 — NAV_BLOCKED inside Mr. Pokemon's house (OPEN)

Stage 7, map (22,26), position (14,13), `NAV_BLOCKED` at f26381. Reached after
taking the Egg objective chain restored by quest3. Not yet diagnosed.

---

## Determinism — landed, and it corrected last cycle's diagnosis

**Last cycle I wrote that "wild encounters make run-to-run routes differ". That
was wrong, and it mattered.** The first thing this cycle did was test it:
`verify.py::runs-are-reproducible` runs the same build from the same state twice
and hashes the tile sequence. The hashes match exactly. libmgba is deterministic
and so is the agent.

What actually diverges is **build-to-build**. A code edit shifts how many
`Random()` calls precede any given frame, so the dice land differently in the
new build, and the build guard forces a fresh bootstrap per build anyway. The
confound was real; my explanation of it was not.

Two pieces now close it:

* **`gAWSuppressEncounters`** — one byte, default 0, driving the vanilla
  `DisableWildEncounters()` the game already uses for its own scripted
  sequences. Zero ROM behaviour change: `encounter-suppression-is-off-by-default`
  asserts it reads 0 at load and after 20,000 frames. With the only meaningful
  RNG consumer removed, a route is comparable across builds.
* **`harness/repro_stall8.py`** — a short, encounter-free run to one named
  verdict (`REACHED_EXIT` / `STUCK_IN_HOUSE` / `NEVER_GOT_THERE`), and it
  reports its own wild-battle count so a silent suppression failure cannot be
  mistaken for a clean run.

`runs-are-reproducible` is now a scenario in its own right, so a future change
that reintroduces nondeterminism fails loudly instead of quietly poisoning a
diagnosis.

---

## Stall 8 — FIXED, with the project's first interpretable A/B

The diagnosis held: an NPC parked on the exit warp of Mr. Pokemon's house made
all four approaches report `COLLISION_OBJECT_EVENT` (`tgt collision 0x4444`),
the search died with one seed and no expansion, and the only way out looked
impossible. Door detection treated `OBJECT_EVENT` as "not a wall" while
`CanStep` demanded `COLLISION_NONE`.

**The fix is the temporary-obstacle model, not either binary.** An NPC is
blocked *now*, not impassable:

* **Strict first.** The window search still refuses to plan through people, so
  a clear route always wins. (Attempt 1 last cycle lost exactly here.)
* **Permissive only as a second pass**, via `NavPermissiveRetry`, when the
  strict search could not reach the goal at all. The warp stays usable.
* **Wait, do not bump.** The follower re-probes each tick and holds still for up
  to `NAV_NPC_WAIT_LIMIT` frames rather than walking into somebody — bumping an
  NPC does not move the player, which is what burned attempt 2.

| | baseline | with fix |
|---|---|---|
| entered the house | frame 12160 | frame 12160 |
| left the house | **never** | **frame 14900** |
| wild battles | 0 | 0 |

Identical entry frame, so the route *to* the house is unchanged and the
difference is attributable to the edit. That is the first A/B in this project
that supports a causal claim.

**Horizon: objective 7 -> 8 of 44. Distinct tiles 142 -> 331, maps 4 -> 6.**

---

## Healing — Centre routing landed, one real bug fixed, still failing

The generated Centre table (30 Centres, approach tiles verified) is integrated.
Three things from it were load-bearing:

* **Route to a table Centre, never `lastHealLocation`.** Before the first Centre
  visit the game's own heal record is the player's front door — verified by
  reading the save block, which returned `(0,0) at (20,12)`. The opening is also
  the worst stretch in the game for this: no Centre exists until Cherrygrove.
* **`AW_FindNearestCentre` is a forward BFS**, because the unknown here is the
  destination; `dist[]` answers the opposite question. It reuses the navigator's
  existing graph scratch — a private queue cost 2,990 bytes of IWRAM (78.4% ->
  87.5%) for a search that runs on one rare frame, so it was moved into
  `auto_worker_nav.c`. IWRAM is back to 78.41%.
* **`FindNurse` cross-checks the table.** Sprite alone is not enough: some
  nurse-graphics objects heal nobody.

**Bug found and fixed: the heal branch called `AW_Nav_SetGoal` every frame.**
Each call rebuilds the route and resets the search to IDLE, so the navigator
restarted forever and the agent stood still — healing latched, goal never
progressed, 40,000 frames of nothing. The objective path had always guarded
this; the heal path had not. Now each `AW_HEAL_*` sub-state owns exactly one
`SetGoal` call. After the fix the agent does set off.

**It still does not heal, and the scenario fails honestly.**
`verify.py::low-hp-routes-to-a-pokemon-centre` loads a party at 1/20 HP and
asserts arrival on a map the Centre table recognises. It reports FAIL.

---

## Stall 9 — door oscillation at the Elm's Lab exit (NEW, open)

This is now the heal path's blocker, and it is the only failing scenario.

**A measurement correction worth recording:** the low-HP state is inside
**Elm's Lab**, not New Bark Town. Map `(1,0)` is `MAP_NEW_BARK_TOWN_LAB_HNS`;
New Bark Town is `(0,0)`. I had been reading `(1,0)` as the town.

The Centre choice is correct — replaying the BFS against `world_graph.json`
independently gives Cherrygrove City Pokemon Centre at 3 hops, which is what the
ROM picks. The failure is purely local: the agent oscillates `(13,14)` <->
`(13,15)` at the lab's exit door for ~17,000 frames until the agent-level
backstop disables it (`stopReason 9`). It fails safe, it just cannot get out.

**One fix was tried and reverted.** `reenterTries` resets on `targetMoved ||
mapChanged`, and a door oscillation alternates the target — so the oscillation
looked able to reset its own limit, the same defect class as the watchdog bug
already fixed in `auto_worker.c`. Under the deterministic repro the change was
**byte-identical**: that counter is not the one being exercised. Reverted rather
than left in as untested code with a real regression risk, and the dead end is
recorded in-code so it is not retried. The oscillation originates somewhere else
— `ChooseIntraTarget` is the place to look.

Unlike every previous stall, this one now has a deterministic repro to work
against from the first attempt.

---

## Tooling fixed along the way (this cycle)

* **`navprobe.py` was reading a stale offset table.** Adding two fields to
  `AwNavState` moved everything after `reenterTries`, so it silently reported
  `npcWait` as `waitTicks` — a wrong number, not an error. `navoffsets.py` now
  computes the layout with the compiler, as `genoffsets.py` already did for
  `sAW`. This is the fourth hand-written offset table in this project to be
  wrong; the generator is the fix for the class, not for the instance.
* **`make_lowhp.py` wrote an unstamped state.** An unstamped state loads happily
  against the wrong build and every address read through it is garbage — the
  exact hazard the build guard exists for. It stamps now.
* **I destroyed `verify.py::main()` with a bad slice and rebuilt it**, and the
  rebuild is better than what it replaced: `COULD_NOT_RUN` now exits non-zero.
  A suite that cannot run is not a suite that passed, and exiting 0 on it is how
  a broken harness goes unnoticed. There is no git in this tree, which is why a
  careless edit cost real time.

---

## Tooling fixed along the way

- **`sAW` offsets are now generated by the compiler**, not hand-written. Four
  separate hand-derived tables were wrong during this session; each returned a
  confident wrong value rather than an error. `/tmp/lay*.c` regenerates them.
- **The objective table stride was 28, not 24 or 32.** A host `sizeof` says 32
  because a host pointer is 8 bytes; the GBA pointer is 4. Reading the table at
  the host stride silently produced a plausible-looking but wrong `obj[1]`.
- **`navprobe.py`'s base address** was resolved from the linker map instead of
  hardcoded, since any edit to `auto_worker.c` relinks `.sbss` and moves it.
- **New permanent diagnostics in `sAW`**, so future stalls are readable from
  outside instead of inferred: `stopReason`, `navBlock`, `lastNavRet`,
  `dbgMapId/dbgGroup/dbgNum` (what `SetGoal` was actually handed), `dbgHere`,
  `dbgWhich`, `navFails`.

---

## Honest estimate

Two objectives of 36 are reachable. The remaining distance to the Elite Four is
**not** 34 more fixes of this size — most objectives share the same few
mechanisms (route, talk, fight), so fixing the intra-map search should unlock a
long run at once. But that is a prediction, not a measurement, and this session
produced two stalls whose causes were each invisible until instrumented.

Updated after this session. The intra-map blocker took one cycle, as estimated.
Three stalls are now understood, and their causes were: a wrong inferred
threshold, a wrong model of how doors work, and a missing objective. Only one of
those three was an engine bug.

That shifts the estimate. The engine is looking sound — routing now crosses
maps, opens doors, and survives 60,000 frames without disabling. What remains is
mostly **table completeness**, which is cheaper per cycle and more parallelisable
than engine debugging.

Realistic read after this cycle: the starter gap took one cycle, as estimated,
and confirmed the table-completeness thesis — the fix was a table split plus a
general rule, not engine work.

Four stalls are now understood. Causes: a wrong inferred threshold, a wrong
model of doors, a completion flag attached to the wrong step, and (still open)
something blocking the Lab exit. **Two of four were table errors, one was an
engine bug, one is undiagnosed.**

Six stalls now understood. Causes: a wrong inferred threshold, a wrong model of
doors, a completion flag on the wrong step, two missing objectives at the Lab
exit, an unreachable connection target, and an unexplained battle hang.
**Three table errors, three engine bugs** — the split is closer than it looked
last cycle, but every engine bug so far has been in the same place: the
navigator's model of how the player physically leaves somewhere.

The audit table changes the outlook materially: 35 DERIVED / 0 INFERRED /
4 UNKNOWN, one objective without a completion test (was 11), and the Elite Four
now testable via `VAR_LEAGUE_STATE`. It also fixed a Champion objective that
could never have completed.

First gym badge: **2–4 cycles**, revised back down. Two of the three risks named last cycle are now closed: the long-crossing path
works (Route 29, Route 30 both crossed with the existing window machinery), and
an AI-driven trainer battle has resolved without hanging. Routes 31 and Violet
City are the same shape as what now works.

Remaining risk is concentrated in two places: the first white-out, since healing
is still unimplemented and the agent is running a single Pokemon; and whatever is
blocking inside Mr. Pokemon's house. Four of the eight stalls so far were table
errors and four were engine bugs, but three of the four engine bugs were the same
bug wearing different hats — the navigator's model of leaving somewhere — and
that machinery is now shared. The audit names Ice Path (21), Victory Road (33), Rocket HQ
(19) and Cherrygrove (3) as the remaining suspects; pre-empting those is now
cheaper than rediscovering them one emulator cycle at a time.

---

## Cycle: heal approach tile, auto-battling off, trees, window, Mom

**Horizon: objective 6/44 -> 11/45** (measured with encounters suppressed; see
the measurement note below, which matters more than the number).

### Stall 10 — heal could never reach the nurse (FIXED)
The in-Centre branch aimed at `nurse + 1`. Gen 3 puts an impassable
`MB_COUNTER` between nurse and player -- Cherrygrove is nurse (7,2), counter
(7,3), player (7,4) -- so the target was a tile no path can ever reach. The
agent sat on the Centre doormat at local (7,8), which is the *entrance warp*,
until the NPC-wait timer expired: 641 `NAV_BLOCKED/UNREACH` stops in a 400k
run. Now uses the audited Centre table's `approachX/Y`, `facing` and
`counterSteps` (30/30 verified against map data) instead of guessing.

### Stall 11 — auto-battling freezes the game, in TRAINER battles too (FIXED)
The previous fix limited the AI controller to `BATTLE_TYPE_TRAINER`, on the
strength of one trainer battle that resolved. That was wrong. The first rival
battle froze with `battleLatch = 1`: `gMain.vblankCounter1` static at 38289
across 3,000 emulated frames, `battleFrames` frozen at 27. No in-ROM code runs
during a freeze, so no watchdog could fire -- which is exactly why it presented
as a *silent plateau with zero stops* rather than an error.

A/B, same save state, one line changed:

| latch | result |
|---|---|
| `enabled && TRAINER` | freezes, stalls at objective 8 |
| `FALSE` | rival battle resolves, reaches objective 9 |

`SetControllerToPlayerPartner` is simply not safe on a player-side battler in
this fork. Battles are now played by key presses everywhere. Much weaker --
FIGHT plus the first move -- but a weak turn is recoverable and a frozen
console is not.

**A false A/B to avoid repeating:** the first probe forced
`AutoWorker_IsAutoBattling()` to return FALSE but left `sAW.battleLatch` set.
The engine then used the normal controller while the tick still refused to
press A, producing `BATTLE_HANG` and a misleading result. The latch and the
predicate must be changed together.

### Stall 12 — a tree is not a person (FIXED)
Object-event collisions were treated as temporary obstacles, which is right for
NPCs: one wanders off, so planning through it as a last resort is reasonable.
Cut trees, smashable rocks and Strength boulders are object events too and
never move, so a route through one can never be walked. `IsPermanentBlocker()`
keeps them walls even in permissive mode.

### Stall 13 — the 32x32 search window was too small (FIXED)
On Route 29 the agent froze at local (30,23) with a correct-but-useless
`UNREACH`. BFS over the real blockdata settles it:

| scope | reachable | east-edge cells |
|---|---|---|
| full map | 778 tiles, x 0..69 | goal IS reachable |
| 32x32 window | **59 tiles** | **0** |
| 48x48 window | 556 tiles | 21 |

The escape route runs *west* of a 32-wide window before turning east, so the
agent sat in a pocket re-planning until the no-progress watchdog fired -- 383
times in a 400k run. `NAV_WIN` 32 -> 48. Cost: EWRAM 96.80% -> **97.91%**,
leaving ~5.4 KB. The budget is now mostly spent.

### Stall 14 — New Bark will not let you leave (FIXED, table gap)
The west exit tiles (0,12..14) carry coord events gated on
`VAR_NEWBARK_TOWN_STATE` 2 or 4, and `NewBarkTown_EventScript_LeaveTrigger`
runs `applymovement PLAYER, PlayerStepBack` -- it physically shoves the player
back east. The table went straight from the police episode to Sprout Tower, so
the agent pressed LEFT into the trigger until the backstop fired, 137 times.
`MomEventScript_GoodLuck` (PlayersHouse_1F/scripts.inc:45) sets the var to 5
and clears the gate. Added as objective 10.

**Generator trap, cost a cycle:** `gen/quest/` is a STALE older copy.
`gen/quest3/` is the live one -- the committed table says so in its header.
Regenerating from `gen/quest` silently dropped objectives (including the Lab
exit aide trigger) and the horizon fell from 10 to 5 before I caught it.

### Measurement note, which outranks the horizon number
`fullrun.py` is **noisy across builds**. Runs are deterministic run-to-run on a
fixed build, but any code change shifts how many `Random()` calls precede a
frame, so wild encounters differ and the run diverges. Adding one objective at
index 10 -- which cannot affect stages 0..9 -- moved the measured horizon from
10 to 6. The Mom fix looked like a regression and was not.

**Measure with `gAWSuppressEncounters = 1`.** That isolates navigation and
table correctness from battle luck and is the number worth tracking. With it,
this cycle: **11/45**, and the Mom objective visibly completes.

### Blocking now — stall 15: the router does not model HM gating
At objective 11 (Sprout Tower) the agent sits in Cherrygrove at local (64,11)
with `navRet=BLOCKED blk=SURF`, re-planning forever. The inter-map router picks
the fewest-hops path and has no notion of which edges need Surf, Cut, Strength
or Waterfall, so it keeps choosing a water route the player cannot take. This
will recur at every HM gate until edge traversal is conditioned on the badges
and HMs actually held.

---

## Cycle: HM-aware routing investigated; window widening rejected on measurement

**Horizon: 11/45, unchanged.** No objectives gained. Two findings and one free
memory win; the most useful output is a correction.

### `blk=SURF` was a diagnostic, not a cause
The brief was to build HM-aware routing because objective 11 stalls with
`blk=SURF`. That reading is wrong. `sawReason` records *the most actionable
rejection seen during a failed search* (`auto_worker_nav.c:212-225`) -- it means
"this search failed, and water was among the tiles I rejected", not "the route
requires Surf". The real failure is UNREACHABLE and the water was incidental.

**HM gating is not yet blocking anything.** It will matter at genuine Surf
gates later, but building it now would have been effort against a symptom.

### The actual blocker at objective 11 is the window again
Cherrygrove is 65x42; the agent sits at local (64,11), the Route 29 connection,
and needs the Route 30 connection on the north edge. BFS over the real
blockdata:

| scope | reachable | north-edge (y=0) cells |
|---|---|---|
| full map | 706 tiles, x 7..64 | reachable |
| 32 window (x 48..64) | 68 | **0** |
| 48 window (x 40..64) | 191 | **0** |
| 56 window (x 36..64) | 242 | **0** |
| 64 window (x 32..64) | 308 | 3 |

The escape runs *west* before turning north -- away from the goal -- so edge
seeding, which seeds the edge **facing** the goal, cannot find it.

### Widening to 64 was rejected by end-to-end measurement
It fixes the reachability above, and it still fits in memory once dirField is
packed. It also made the agent **worse: horizon 11 -> 9** with encounters
suppressed. Raising the frontier cap from NAV_CELLS/2 to 3/4 produced a
byte-identical trace, so the cap was not the cause. A 64-tile window is 4x the
cells of 32 and the frame-sliced search spends proportionally longer before it
can act. Reverted to 48.

**This is the lesson worth keeping: a change that is provably correct on the
map data can still be a regression in the loop.** The blockdata BFS proves what
is *reachable*; only the emulator shows what the agent *does*.

### Free memory win (kept)
`dirField` held values 0..15 in a full byte. Packed to a nibble:
**EWRAM 97.91% -> 97.03%**, ~2.3 KB reclaimed, horizon unchanged. Unit-tested
standalone -- write-all/read-all, neighbour isolation, full 0..15 range in both
halves -- because packing is the classic silent-corruption change.

### Harness: a scenario that failed because the agent succeeded
`survives-a-white-out` began reporting `COULD_NOT_RUN: this build got lucky`.
Not luck -- the heal fix works, so a 1 HP agent now walks to Cherrygrove Centre
and recovers before anything can kill it. Forcing the faint by re-poking HP to
1 each tick was worse: it stops `PartyFullyHealed()` ever becoming true, so the
agent loops in the Centre forever. The scenario now accepts **either** survival
path -- faint-and-recover, or heal-before-dying -- because the question it
exists to answer is "does a nearly-dead party end the run", and both answers
are no.

### Blocking now — stall 15 restated
Objective 11, Cherrygrove: the intra-map search cannot reach the north
connection because the only route leaves the window heading away from the goal.
The fix is **chained window hops that may move away from the goal**, not a
wider window: when the goal-facing edge is unreachable, seed the other reachable
window edges, walk to one, recentre, and search again, with visited-edge memory
to prevent oscillation.

---

## Cycle: chained escape hops — implemented, no horizon change

**Horizon: 11/45 before and after**, encounters suppressed. Maps visited 8 -> 9
and objective 11 is reached ~3,600 frames sooner, so the mechanism does
something, but it does **not** unblock objective 11. Reporting that plainly
rather than counting the map and the speedup as progress.

### What was built
When the window edge facing the goal is unreachable, the escape may run *away*
from the goal. Cherrygrove needs a westward detour to reach its north
connection -- BFS over the real blockdata finds **0** reachable north-edge
cells at both a 32 and a 48 window -- so goal-facing seeding cannot find it by
construction.

The navigator now seeds **every** window edge as a fallback, walks to whichever
boundary is reachable, records the side it left by so the next hop cannot
immediately return, recentres, and resumes ordinary goal-facing seeding.

Bounded at 4 hops. The budget resets **only on a new goal or a new map**, never
on a changed tile -- two earlier watchdogs here reset on movement, which an
oscillating agent satisfies every step, so they could never fire against the
thing they existed to catch.

### Why it was safe to try
It adds search **episodes**, not search **width**. EWRAM unchanged at 97.03%,
three bytes of state. That matters because the previous cycle's 64-tile window
was provably correct on reachability and cost two objectives, purely because
4x the cells means the frame-sliced search takes proportionally longer to act.

### What I could not establish
I did not confirm the escape path actually fires at Cherrygrove. `escapeTries`
is not exposed in `sAW`, and two attempts to trace stage 11 directly produced
no usable output -- the first because a `continue` in my probe skipped the
re-enable so the agent stayed disabled, the second because that run never got
past stage 8. **So "implemented and harmless" is established; "fires where
intended" is not.** Exposing `escapeTries`/`escapeMode` in `sAW` is the cheap
fix and should come before any further work on this path.

### Note for whoever picks this up
The build guard did its job mid-cycle: `low-hp` and `survives-a-white-out` both
returned `COULD_NOT_RUN` naming the ROM mismatch after a rebuild, instead of
reading garbage and reporting a confident wrong answer.

---

## Cycle: the objective-11 blocker was never Cherrygrove

**Horizon 11/44, unchanged. Maps traversed 8 -> 12, tiles 48 -> 88.**
`verify.py` 11/11, build EXIT=0, EWRAM 97.04%, IWRAM 78.41%. Falkner not reached.

### The inherited diagnosis was wrong
Objective 11 was recorded as stalling in Cherrygrove at local (64,11), needing
chained window hops. That was **inferred from a failed search and never
observed**. With `escapeTries`/`escapeMode`/`escapeFired` exposed in `sAW` and
latched every tick, the agent is plainly stalling on **Route 31 at local
(4,11)** -- a different map entirely.

The escape mechanism built last cycle **does** fire: 58 times in one run. Only
6 hops ever landed. It was never the missing piece.

### Root cause: hop count is not reachability
Route 31 reaches Violet City two ways -- a **left map connection** (1 hop) and
the **gate warp at local (4,10)** (2 hops). The router picks fewest hops, so it
chose the connection and aimed at local (0,22).

BFS over Route 31's real blockdata, elevation-aware:

| probe | result |
|---|---|
| target local (0,22) collision | **1 -- a wall** |
| whole x=0 column reachable | **0 tiles** |
| reachable from player | 407 tiles |
| reachable far-edge cells | only (32,22),(33,22) on the south row |

The agent ground against a walled column while standing **one tile below the
gate door that actually works**.

### Fix: blacklist an edge the map proves unwalkable, then re-route
`ChooseIntraTarget` now runs two passes. Pass 0 takes a shortest-path edge,
skipping blacklisted ones. Pass 1 runs only if every shortest edge is
blacklisted and accepts the best remaining edge **even though it is a longer
route** -- a route one hop longer beats a route through a wall. On terminal
intra-map failure the chosen edge is blacklisted and planning restarts.

This carried the agent off Route 31, through Violet City, into Sprout Tower 1F
and up to 2F -- two maps it had never reached.

### Three resets were erasing the fix before it could act
Each one individually made the mechanism inert, and each looked correct in
isolation:

1. `AW_Nav_SetGoal` reset the escape budget and blacklist unconditionally. The
   runner re-plans on **every** BLOCKED verdict, so both were wiped before they
   could accumulate. Now cleared only on a genuinely different goal.
2. `AW_Nav_Reset` writes `lastGroup = 0xFF`, which the next tick read as a map
   change -- resetting the budget again. `escapeTries` was pinned at 1 forever,
   so the limit that triggers a re-route was never reached. Only a real
   map-to-map transition counts now.
3. Re-enabling a stopped agent calls `AW_Nav_Reset` before `SetGoal`, clearing
   `haveGoal` and so defeating the same-goal check. **Not fixed** -- in a run
   that re-enables on every stop (as `fullrun.py` does by design) the budget
   still cannot reach its limit. This is the next thing to address.

### New blocker: Sprout Tower 2F is internally partitioned
The agent stands at local (6,5) on 2F. Elevation-aware BFS over the real
blockdata: **59 tiles reachable**, and the 3F staircase at local (10,16) is
**not among them** -- every approach unreachable.

2F has four warps. The agent came up warp 1 at (6,4) from 1F and landed in a
pocket that does not connect to the 3F stairs. **The world graph models a map
as a single node**, so all three 1F->2F staircases look identical to the
router; they are not.

The blacklist is the right mechanism but needs to reach one level further: when
a map's hop target is unreachable, the **inbound** edge that delivered the
agent into the wrong pocket must also be blacklisted, so the router picks a
different staircase rather than re-entering the same dead region.

### Follow-up: the three resets fixed, and the limit of inbound blacklisting

**Reset 3 fixed.** `AW_Nav_Reset` clears `haveGoal`, and the agent calls it on
every re-enable -- so in any run that re-enables after a stop (which
`fullrun.py` does **by design**, because a player would try again) the
same-goal test failed and the blacklist was wiped. The headline measurement was
running with blacklisting effectively disabled. Replaced with `goalEverSet`,
which records only that the stored goal coordinates are meaningful and which
Reset leaves alone.

**Effect, measured.** The agent now routes Route 31 -> gate (22,1) -> Violet
City -> Sprout Tower 1F -> 2F, reaching 2F at **frame 77,004 instead of
~214,000**. It takes the gate rather than grinding the walled connection.

**A caution on the reach metrics.** `maps` and `tiles` went 12 -> 9 and 88 ->
73 when blacklisting was enabled, which reads as a regression and is not one:
with the blacklist wiped the agent wandered more and so touched more maps
without getting further. Stage was 11 in both. **Maps-visited measures
wandering, not progress** -- the map LIST is the honest version of that metric,
and it shows strictly more of the critical path reached.

### Inbound blacklisting: implemented, fires, and is not sufficient
`badEdges` is now exposed in `sAW`, so "did the re-route fire" is observable
rather than inferred. On Sprout Tower 2F:

```
f432604  2F @(13,11)  badEdges=3  stage=11  stop=NO_PROGRESS  blk=NO_EDGE
```

The mechanism **works**: it blacklists the unreachable 3F edge and the entrance
that delivered the agent into the pocket, then exhausts 2F's remaining edges
and correctly reports `NO_EDGE` rather than grinding. The agent oscillates one
tile and the runner stops it.

Also added: when the whole map fits inside the search window (2F is 25x25, the
window is 48) a completed failed search is **conclusive**, so the escape budget
is skipped and the edge is blacklisted immediately. Without this the
no-progress watchdog fired long before four escape hops could complete, which
is why the blacklist never triggered here before.

**So the cheap fix is done and it is not enough.** The answer to "will it need
the generator change" is **yes**. The world graph models each map as a single
node; Sprout Tower 2F has three staircases from 1F landing in three
disconnected regions, and no amount of edge blaming can express "this floor has
four separate rooms" to a router whose vocabulary is whole maps. Blacklisting
can only remove edges, and removing them all yields `NO_EDGE` -- which is what
it now honestly reports.

**The real fix is splitting partitioned maps into multiple graph nodes in
`gen/nav/`**: flood-fill each map's walkable tiles, emit one node per connected
region, and attach each warp to the region containing it. Multi-floor dungeons
later in the game are exactly where single-node maps will keep failing.

**Falkner not reached.** Horizon 11/44, `verify.py` 11/11, EWRAM 97.04%.
