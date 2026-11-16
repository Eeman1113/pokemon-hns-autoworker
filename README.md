# Auto Worker

An autoplay option for **Pokémon Heart & Soul**, added to the start menu under `SAVE`.
Hand it the controls for the parts you don't want to play; press **B** and you have them
back.

**→ [Patch your own ROM in the browser](https://eeman1113.github.io/pokemon-hns-autoworker/)**

No game data is distributed here. You supply your own Pokémon Emerald ROM, the patch is
applied in your browser, and nothing is uploaded anywhere.

---

## Honest status

This is an early build, and the gap between what is implemented and what was asked for
matters more than a feature list.

| Capability | State |
|---|---|
| `AUTO WORKER` entry in the start menu under `SAVE` | **works** |
| Battles played by the game's own AI | **works** |
| Instant hand-back on **B** | **works** |
| Stage detection — resume from any save | **works** |
| Dialogue advance, patrol-to-encounter | **works** |
| Cross-map travel | built, **not yet wired to movement** |
| Finishing the game unattended | **no** |

Auto-battling and hand-off are real. Walking itself across Johto is not yet. Puzzle
sections — Ice Path, the Rocket HQ password — are marked hand-back and always will be:
an agent that silently flails is worse than one that stops and says so.

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
