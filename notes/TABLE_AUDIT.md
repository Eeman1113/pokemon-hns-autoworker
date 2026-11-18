# Auto Worker — objective table audit

Read-only audit of the live table (`gen/quest/`, 37 objectives), with a
corrected table emitted to `gen/quest2/` (39 objectives). Nothing in
`pokehns-expansion` or `gen/quest/` was modified.

---

## 1. Mis-attributed completion tests — the hit list

> **A completion test belongs to the objective whose own script sets it.**

`misattrib.py` indexes every `setflag` / `setvar` / `giveitem` in all 1,499 maps,
then for each objective compares the real producers against its own target:

| verdict | meaning |
|---|---|
| `SAME_SCRIPT` | the target's own closure sets it — sound |
| `SAME_MAP` | a different trigger on the same map sets it — **needs splitting** |
| `OTHER_MAP` | only other maps set it — **mis-attributed, can never complete** |

### Live table (37 objectives)

```
SAME_SCRIPT 25 · NO_TEST 11 · OTHER_MAP 1
```

**One fatal hit, and it is exactly the predicted pattern:**

| # | objective | test | actually set by |
|---|---|---|---|
| 36 | **Champion — Lance** | `FLAG_IS_CHAMPION` | `MAP_POKEMON_LEAGUE_HALL_OF_FAME_HNS :: SetFirstGameClearFlags:62` |

The Hall of Fame is a **different map**, entered only *after* Lance is beaten, so
the objective could never complete while it was current. Corrected to
`VAR_LEAGUE_STATE >= 6`, which Lance's own defeat script sets
(`LanceOnDefeat:136/206`).

**Zero `SAME_MAP` hits**, which is the useful negative result: after the loop
agent's Elm split there were no remaining un-split multi-step objectives
detectable this way. The starter was the only one of its kind.

### Corrected table (39 objectives)

```
SAME_SCRIPT 37 · SAME_MAP 1 · NO_TEST 1
```

Both survivors are `HAND_BACK` objectives, where the agent stops regardless:
Ice Path (test set by `KimonoGirlPush` at `(49,25)`, not the coord trigger) and
Victory Road (nothing records completion).

---

## 2. Stall 4 solved — and it was **two** missing objectives, not one

The exit-gating NPC hunt found it. Indexing coord events near warps on the Lab
map exposed the whole opening chain:

| step | trigger | `VAR_NEWBARKTOWN_LABSTATE` |
|---|---|---|
| walk into the Lab | coord `(6,7)` `TriggerStartLab` | → 1 |
| take a starter ball | `CyndaquilBall` | unchanged (sets `FLAG_SYS_POKEMON_GET`) |
| **talk to Elm again** | `Elm` `(6,3)` → `MrPokemon:295` | **→ 2** |
| **step on the exit tile** | coord `(6,12)` `TriggerAideGivePotion:357` | **→ 3** |

The aide trigger sits **on the Lab's exit tile**. That is precisely why the
agent oscillated at the doorway: stepping onto the exit fires a script, and
until `LABSTATE` reaches 3 it fires again every time. Two objectives were
missing, and neither is reachable by re-interacting with anything the table
already knew about.

Both are now in the table as `DERIVED` with citations. This also generalises
the coordinator's rule: **a coord trigger on a warp tile is an objective**, not
scenery — it gates the exit as surely as a locked door.

---

## 3. The per-objective audit

> Is the declared `done` condition produced by the transitive closure of the
> script attached to **this objective's own target**?

| outcome | meaning |
|---|---|
| produced, same value | sound |
| produced, different value | **wrong threshold** (the objective-0 bug) |
| produced by a *different* trigger on the map | **multi-step** (the objective-1 bug) |
| produced nowhere | **untestable / wrong condition** |

`audit.py` + `run_audit.py` implement this over all 1,499 maps and 37 objectives.

## Headline

| | before | after |
|---|---|---|
| objectives | 37 | **39** |
| DERIVED / INFERRED / UNKNOWN | 25 / 8 / 4 | **35 / 0 / 4** |
| objectives with **no completion test** | **11** | **1** |
| mis-attributed tests | **1 fatal** | **0** |
| coordinates verified | 37/37 | **39/39** |

Table size ~936 B, `const` → ROM. EWRAM untouched (cursor only).

**No wrong thresholds and no un-split multi-step objectives remain.** The two
failures that motivated this audit were the only instances of their kind; the
loop agent had already fixed both by the time the sweep completed.

## What was rescued, and with what evidence

Ten objectives had `done = NONE` — permanently unprovable, so the resume
detector could never advance past them. All but one now have a real test, each
cited to the line that produces it:

| objective | new completion test | source |
|---|---|---|
| Cianwood Shop: Secret Potion | `VAR_OLIVINE_CITY_STATE >= 4` | Pharmacist |
| Lake of Rage: Red Gyarados | `VAR_MAHOGANY_TOWN_STATE >= 2` | `LakeOfRage_EventScript_Gyarados` |
| Rocket HQ | `VAR_GOLDENROD_CITY_STATE >= 7` | `RadioTower_5F_EventScript_Petrel` |
| Ice Path | `VAR_ICE_PATH_STATE >= 1` | `IcePath_1F_EventScript_KimonoGirlPush:60` |
| **Elite Four — Will** | `VAR_LEAGUE_STATE >= 2` | `WillOnDefeat:97` |
| **Elite Four — Koga** | `VAR_LEAGUE_STATE >= 3` | `KogaOnDefeat:55` |
| **Elite Four — Bruno** | `VAR_LEAGUE_STATE >= 4` | `BrunoOnDefeat:55` |
| **Elite Four — Karen** | `VAR_LEAGUE_STATE >= 5` | `KarenOnDefeat:55` |
| **Champion — Lance** | `VAR_LEAGUE_STATE >= 6` | `LanceOnDefeat:136/206` |
| Cianwood: HM Fly | `HAS_ITEM ITEM_HM_FLY` | `ChucksWife` gives it, sets nothing |
| Goldenrod: Card Key | `HAS_ITEM ITEM_CARD_KEY` | `Director` gives it, sets nothing |

**The Elite Four is the significant one.** It was believed untestable because it
uses `FLAG_TEMP_*`. It does not: `VAR_LEAGUE_STATE` runs a clean 2→3→4→5→6 spine
across the five rooms. The agent *can* tell how far into the gauntlet it got, so
a loss no longer costs the engine its place.

**Lance's test was also wrong, not merely missing.** It tested
`FLAG_IS_CHAMPION`, which is set in the Hall of Fame — a different map — so it
could never be true while the objective was current.

### A new condition kind

Two key items are given with no flag and no var, so possession is the only
evidence they were ever obtained. `AW_COND_HAS_ITEM` evaluates via
`CheckBagHasItem` (already exported, `include/item.h:210`). The enum and its
evaluator both live in the generated table, so this costs the engine nothing.

## Corrected target

**Victory Road** pointed at `(23,16)`, which resolves to
`VictoryRoadKanto_EventScript_Item_Dark_Pulse` — an item ball, not an obstacle.
The real blockers on that floor are the `EventScript_RockSmash` cluster at
`(23,12..14)`, so the target is now the approach tile `(23,15)` and the
objective is renamed "blocked by Rock Smash". It remains a `HAND_BACK`.

## Two analyser bugs found and fixed — worth knowing, because both produced *false* bug reports

1. **The call-graph closure did not follow `case N, Label`**, which is how
   `multichoice` dispatches. Every quiz and branching menu in the game stopped
   the closure dead. This made Dragon's Den's badge look unreachable from its
   own quiz trigger when it was always reachable.
2. **The closure depth limit of 10 was too shallow.** The Elder quiz is five
   nested question levels; the chain
   `quiz → Quiz1..5 → QuizFinish → ClairEnter → setflag FLAG_BADGE08_GET`
   exceeded it. Raised to 40.

Both would have caused "fixes" to code that was already correct.

## Confidence is now backed by evidence

9 objectives were labelled `DERIVED` with **no citation** — asserted, not shown.
Rather than trust them, citations are now **generated** by locating the exact
`script:line` inside each target's own closure that produces its completion
condition. An objective that cannot be cited cannot be labelled `DERIVED`.

`INFERRED` has fallen to zero because every surviving non-hand-back objective's
completion is now traced to a source line. **Preconditions remain advisory** —
136 of 201 var transitions still have an unknown "from" state, and nothing here
changed that.

## Where to expect the next stall

Ordered by my confidence that they are wrong:

1. **`Ice Path` (objective 21)** — still multi-step. The coord trigger at
   `(49,29)` is not what sets `VAR_ICE_PATH_STATE`; `KimonoGirl` at `(49,25)`
   is. A `HAND_BACK`, so the agent stops regardless, but the test will read as
   complete after the first boulder rather than after the puzzle.
2. **`Victory Road` (objective 31)** — the only remaining `done = NONE`. The
   coordinate is now sane but nothing records completion.
3. **`Rocket HQ` (objective 19)** — `VAR_GOLDENROD_CITY_STATE >= 7` is set by
   Petrel, but the password/trapdoor sequence sits between arriving and Petrel.
   Expect the agent to hand back long before the test can become true.
4. **The 8 Kanto gyms** — citations are auto-generated and the coordinates
   verify, but no run has ever reached them. Badge-chain preconditions are
   inferred orderings, not derived gates.
5. **`Cherrygrove tutorial` (objective 3)** — `VAR_CHERRYGROVE_CITY_STATE >= 1`
   is the vaguest surviving threshold; the original note was just
   "setvar derived".

## Files

```
gen/quest2/audit.py          the closure/effects analyser
gen/quest2/run_audit.py      per-objective verdicts -> audit.json
gen/quest2/gen_table.py      corrected generator
gen/quest2/quests.{c,h}      37 objectives, 33 DERIVED / 4 UNKNOWN
gen/quest2/verify_targets.py coordinate verifier (37/37 PASS)
```

Integration note: `quests.h` gains `AW_COND_HAS_ITEM`, and `quests.c` now
includes `item.h` and `constants/items.h`.

---

# Pass 2 — pre-emptive fixes to the suspect list (`gen/quest3/`, 44 objectives)

Based on the current `gen/quest/` (39 objectives). Output in `gen/quest3/`.

| | pass 1 | pass 2 |
|---|---|---|
| objectives | 39 | **44** |
| DERIVED / INFERRED / UNKNOWN | 35 / 0 / 4 | **41 / 0 / 3** |
| hand-backs | 4 | **3** |
| coordinates verified | 39/39 | **44/44** |
| mis-attributed tests | 0 | **0** (1 intentional) |

## The big one: the Mr. Pokémon errand was missing entirely

`VAR_CHERRYGROVE_CITY_STATE` runs **1 → 2 → 3 → 4** across three maps, and the
table jumped straight from "Cherrygrove tutorial" to Sprout Tower. The agent —
which is sitting at objective 5 right now — would have walked past the game's
entire opening quest. Four objectives added:

| beat | where | test |
|---|---|---|
| Receive the Egg | Mr. Pokémon's house | `CHERRYGROVE_CITY_STATE >= 2` |
| Elm's phone call | Route 30 `(35,6)` | `>= 3` |
| Rival battle | Cherrygrove `(56,10)` | `>= 4` |
| **Return to the Lab** (police episode) | Lab | `LABSTATE >= 5` |

## A third structural pattern: `map_script_2` is an objective

Two of my new objectives initially failed their own audit. The cause is a
pattern neither existing rule covers:

> **An `ON_FRAME_TABLE` (`map_script_2`) entry belongs to the MAP, not to any
> tile — it fires on *entering* while its gate var matches.**

Modelling those as `TALK_TO` aims the agent at an NPC that never produces the
test. Both the Egg and the Mahogany Rocket trigger are this shape, and so is
the police episode. A sweep of every main-line map found **10** such beats, two
of which were missing objectives.

This joins the two existing rules:
* a completion test belongs to the objective whose own script sets it;
* a coord trigger on a warp tile is an objective, not scenery.

## Per-suspect outcome

1. **Cherrygrove (3)** — threshold `>= 1` was *correct*; the Gent tour and the
   cancel branch both set 1. The real defect was everything that came after it.
2. **Ice Path (21)** — **still a hand-back, and now I can say why precisely.**
   B1F and B2F carry **4 `PUSHABLE_BOULDER` objects each**; it is a genuine
   Strength puzzle, and no Ice Path floor records solving it. Given a resumable
   completion test instead — `FLAG_VISITED_BLACKTHORN_CITY`, the far-side
   arrival signal — so the agent picks up again once a human has pushed through.
3. **Victory Road (33)** — **hand-back eliminated.** `VictoryRoadKanto_1F_Trigger`
   at `(28,7)` is gated on `VAR_ROUTE27_STATE == 2` and sets 3. Now a `WALK_TO`
   with a real test. The old `(23,16)` target was an item ball.
4. **Rocket HQ (19)** — the missing intervening beat is Mahogany:
   `VAR_GOLDENROD_CITY_STATE` goes 3/4/5 (Whitney) → **6 (Mahogany entry)** → 7
   (Petrel). Added. The hideout itself is var-driven
   (`VAR_ROCKET_PASSWORD`, `VAR_ROCKET_HIDEOUT_STATUE_1/2`,
   `VAR_MAHOGANY_TOWN_STATE` 10→12→13→14), so it is enumerable later — the
   Radio Tower password stays a hand-back for now.
5. **The 8 Kanto gyms** — **all clean.** Every badge flag `09`–`16` is set by a
   script on its own gym map. No second Lance.

## Revised stall forecast

1. **Ice Path (28)** — the one genuine remaining wall; needs Strength pushes the
   engine cannot express.
2. **Rocket HQ / Radio Tower (20)** — hand-back; the password sequence sits
   between arrival and Petrel.
3. **Dragon's Den (24)** — hand-back, but the quiz answers are known
   (`Q1{0,2} Q2{0,1} Q3{1,2} Q4{0,2} Q5{2}`); an `ANSWER_MULTI` verb converts it.
4. **Cianwood HM Fly (16)** — the real gate is `VAR_GETFLY` (ON_FRAME, 1→2); the
   `HAS_ITEM` test works but the *precondition* may be wrong.
5. **Olivine / Cianwood ordering** — Secret Potion and Jasmine interleave across
   two cities by sea; preconditions here are inferred orderings, not gates.

---

# Pass 3 — Pokémon Centre table (`gen/quest4/`)

Closes the healing data gap: `lastHealLocation` reads the player's own front
door until a Centre has actually been used, so the agent routed home, found no
nurse and looped.

## How a Centre was identified — data, not names

A Centre is **an object whose graphics is a NURSE variant (not CHANSEY, not
UNION_ROOM) whose script is non-NULL.** Name matching fails in both directions,
and both failure modes are present in this ROM:

* `BattleFrontier_Lounge9_hns` carries `OBJ_EVENT_GFX_NURSE_HNS` with
  **`script = NULL`** — a nurse sprite that heals nobody.
* Centre 2F trade rooms carry the Centre's name and contain no nurse at all.

Of 70 nurse-graphics objects game-wide, 68 have scripts; restricting to
reachable `_HNS` maps gives **30 Centres**. 27 share one healing script
(`OldaleTown_PokemonCenter_1F_EventScript_Nurse`), which is the hack's common
handler.

## The approach tile is not the adjacent tile

Gen 3 puts the nurse **behind a counter**. Verified on Azalea:

```
 (7,2) nurse     metatile 656  collision 0
 (7,3) counter   metatile 664  collision 1   <- MB_COUNTER, interaction crosses it
 (7,4) player    metatile 695  collision 0
```

A naive "nearest passable neighbour" returns `(8,2)` — **the Chansey's square**.
The table therefore records the real standing tile, the facing direction, and
whether the interaction is adjacent or across a counter.

**24 of 30 share the standard geometry** (nurse `(7,2)`, approach `(7,4)`, face
north). Six differ: Goldenrod, Indigo Plateau, Melemele House, Trainer Hill
Entrance and both Saffron Fighting Dojo rooms — the Dojo pair is the only
`counterSteps = 1` case, with no counter at all.

## Verification

**30/30 approach tiles verified** independently of the builder: in bounds,
collision 0, not occupied by any object event, and exactly `counterSteps` from
the nurse.

Table: `u16 mapId` + 7 × `u8` → **~10 B/entry, ~300 B total, `const` → ROM.**
EWRAM unchanged.

## Coverage — where a white-out is most likely to end a run

Hops to the nearest Centre, over the warp+connection graph:

| stretch | hops | note |
|---|---|---|
| **Objectives 0–4 (New Bark Town opening)** | **3** | **no Centre exists until Cherrygrove** |
| Rocket HQ / Radio Tower (26) | **6** | worst on the main line |
| Goldenrod Underground, Card Key (24) | 5 | |
| Champion — Lance (43) | 5 | and the E4 is a single run |
| Sprout Tower (10), Dragon's Den (29), Blaine (36), Karen (42) | 4 | |

**7 main-line maps sit 4+ hops from a Centre; none is unreachable.**

The opening is the sharpest risk and it is exactly where the agent is now: it is
carrying a single Pokémon, three hops from Cherrygrove, with no heal destination
recorded. `AW_FindCentre()` gives the runner a destination from the first frame.
