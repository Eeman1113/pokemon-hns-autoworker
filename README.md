# Auto Worker

An autoplay option for **Pokémon Heart & Soul**, added to the start menu under `SAVE`.
Hand it the controls for the parts you don't want to play; press **B** and you have them
back.

**→ [Patch your own ROM in the browser](https://eeman1113.github.io/pokemon-hns-autoworker/)**

No game data is distributed here. You supply your own Pokémon Emerald ROM, the patch is
applied in your browser, and nothing is uploaded anywhere.

---

## Honest status

It plays the opening of the game by itself and then stops. That is the whole claim.

**Measured on the current build: objective 15 of 47, 1 of 16 gym badges** — roughly
the first 90 minutes of a ~30-hour game. From a fresh file it leaves home, takes the
starter, runs Elm's errand, fights the rival, crosses to Violet City, climbs Sprout
Tower, **beats Falkner**, collects the Togepi Egg, and catches a Pokémon of its own.

Progress is read from the game's own `FlagGet`, not from scanning memory — eight
hand-derived address reads in this project returned confident wrong numbers, so
anything not computed by the ROM itself is treated as unverified.

| Capability | State |
|---|---|
| `AUTOASH` in the start menu under `SAVE` | works |
| Resumes from any save, including a fresh boot | works |
| Cross-map routing — doors, warps, connections, region-aware | works |
| Instant hand-back on **B** | works, measured |
| Cannot touch your save file | works, measured |
| Survives a white-out and carries on | works, measured |
| Routes to a Pokémon Centre when hurt | works, measured |
| Battle moves chosen by expected damage | works; beat Falkner |
| Catching, with weaken-not-kill and nicknames | works; caught one in a live run |
| Switching, items, level-gated training | not implemented |
| Beating gyms 2–16, Elite Four | unmeasured |
| Finishing the game | no |

**What stops it.** Field moves are gated on **badges, not on owning the HM** — this
hack puts Surf behind badge 4 — so half the map is unreachable until the agent wins
more gyms. Teaching HMs, shopping for Poké Balls, and the remaining progression gates
are built but not yet merged (`notes/GATES.md`).

**Back up your save before using this.**

## How it works

**Battles are not scripted.** `pokeemerald-expansion` already ships a controller that can
drive a *player-side* battler with the full scoring AI — real damage calculation,
switching, item use. Auto Worker points that existing controller at your side of the
field rather than reimplementing a battle AI.

Four of the eleven places the engine tests `IsAiVsAiBattle()` are deliberately **not**
enabled, because they are written on the assumption that *both* sides are AI. One of them
(`battle_script_commands.c`) awards **zero EXP**; another makes the AI refuse to use your
items. Auto Worker uses a parallel predicate ORed into exactly three sites.

**Stage detection is a lookup, not an inference.** The agent cannot read dialogue and will
not pretend to. The route is pre-declared as an ordered objective table, and on hand-off
the agent reads your save file's own flags to find the furthest point it can *prove* you
reached, resuming there. A fresh file and a nine-hour save take the same code path, which
is why there is no "resume" logic anywhere.

**Hand-back is unambiguous by construction.** The agent writes only the remapped key
fields and reads your input only from the raw hardware register, so a real B press can
never be confused with one of its own — no heuristics. Raw keys are never written, so the
A+B+START+SELECT soft reset always keeps working.

**The agent cannot save.** That single structural rule turns every possible bug from
"ruined save file" into "reset and lose the session".

## Repository layout

```
docs/          the web patcher (GitHub Pages) + the .ups patch
src/           auto_worker.c, auto_worker_stage.c, aw_quests.c
include/       public headers
patches/       the integration diff against pokehns-expansion
gen/nav/       world-graph extractor — 1,495 maps, 4,451 edges from the game's own data
gen/quest/     quest-chain deriver — objectives extracted from the map scripts
harness/       headless mGBA verification (libmgba via a C shim, ~4,000 fps)
tools/         UPS create/apply, with full checksum verification
```

## Building from source

```bash
git clone https://github.com/PokemonHnS-Development/pokehns-expansion
cd pokehns-expansion && git checkout Release-v2.0.6
cp /path/to/this/src/*.c src/ && cp /path/to/this/include/*.h include/
git apply /path/to/this/patches/0001-auto-worker-integration.patch
export DEVKITPRO=/opt/devkitpro DEVKITARM=$DEVKITPRO/devkitARM
make hns -j$(nproc)
```

Builds clean on devkitARM r68 / GCC 16.1.0. Footprint: EWRAM 94.65%, IWRAM 78.41%.
IWRAM is the binding constraint in this codebase, so all agent state lives in EWRAM and
nothing uses the task system (`ResetTasks()` is called in 93 places and state would not
survive entering a battle).

## Verification

`harness/` drives libmgba through a compiled C shim at roughly 4,000 fps — 68× realtime,
which is what makes "does it finish the game?" an overnight question rather than a never
question. Scenarios are data, not code.

The harness distinguishes **pass**, **fail** and **could-not-run**, because a harness that
reports a missing save state as a pass is worse than no harness.

## Credits

Pokémon Heart & Soul is the work of **Lil Dill and the HnS team** —
[pokehns-expansion](https://github.com/PokemonHnS-Development/pokehns-expansion) — itself
built on [pokeemerald-expansion](https://github.com/rh-hideout/pokeemerald-expansion) and
the pret decompilation. All credit for the game belongs to them; this project adds one
menu option.

Pokémon is a trademark of Nintendo, Creatures Inc. and GAME FREAK Inc. This project is
unaffiliated with them and distributes none of their work.
