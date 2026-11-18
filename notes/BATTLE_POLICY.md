# Battle policy — design for the next cycle

Expected-damage move choice beat Falkner. It will not beat the Elite Four, and
the reason is structural rather than a tuning problem: the current policy has
**no concept of surviving**. It picks the move that deals the most damage this
turn and has no way to express "I am about to faint", "this matchup is losing",
or "I should heal". A single Pokemon with one decision type is a speedrun
strategy, and the back half of the game is not a speedrun.

## What already exists and must be reused

Everything expensive is in the engine. Nothing here needs new damage maths.

| call | header | use |
|---|---|---|
| `AI_CalcDamage` | `battle_ai_util.h:178` | real damage, same formula the game uses |
| `CalcTypeEffectivenessMultiplier` | `battle_util.h:267` | matchup, `uq4_12_t` fixed point |
| `GetMoveType` / `GetMovePower` / `GetMoveAccuracy` | `move.h:239,246,251` | move facts, two are `static inline` |
| `GetMostSuitableMonToSwitchInto` | `battle_ai_switch.h:46` | switch target |
| `ShouldSwitch` | `battle_ai_switch.h:47` | **only computed for true AI battlers — verify before calling for the player** |
| `CheckBagHasItem` | `item.h:210` | item availability |

Two hard constraints: **no FPU**, so integer or `uq4_12_t` only; and **EWRAM is
at 97.36% with ~6.9 KB free**, so the policy must compute rather than store.
Per-turn scratch is fine; per-battle tables are not.

## The decision, in priority order

Evaluate cheapest-and-most-decisive first and return early. Most turns should
exit at step 3 having done one damage calculation per move.

1. **Can I be killed this turn?** Estimate the opponent's best damage against the
   active Pokemon. If it exceeds current HP and something in the party survives
   that hit, **switch**. If nothing survives and a heal would take us out of
   range, **heal**. Otherwise fight — dying with a wasted turn is worse.
2. **Can I kill this turn?** If any move's expected damage >= opponent HP and we
   move first, take it. Speed order decides whether step 1 or 2 wins; use
   `AI_WhoStrikesFirst` if it is callable for the player's battler.
3. **Otherwise pick the best damaging move.** Current behaviour: power x type
   effectiveness x STAB x accuracy, integer, plus PP > 0.
4. **Status and setup moves: ignore them.** This is deliberate. The engine's own
   AI was measured at *20 faints in 25 battles* against mashing's 1, precisely
   because it rationally picks setup moves that only pay off for a trainer with a
   full team to switch and sweep with. A single Pokemon that never switches
   cannot cash those in. Revisit only once switching works.

## Switching

The piece with the best ratio of value to effort, because the engine already
chooses the target. Needed for: type-disadvantaged leads, a fainted active
Pokemon (forced), and preserving a low-HP Pokemon that still has a role.

Risk to measure, not assume: switching costs a turn and takes a hit on the way
in. A naive "switch whenever disadvantaged" loop can be worse than mashing. Gate
it on *surviving* the incoming hit, and measure win rate before and after on the
same state with the same seed.

## Items

Potions and status heals from the bag. Cheap to add once switching exists, and
the same "does this change whether I die this turn" test governs it. Must never
use a rare or key item — restrict to an explicit allowlist, not a denylist.

## Party building

Catching and training are prerequisites for the Elite Four, not optional polish.
No move policy wins a 5-Pokemon gauntlet with one under-levelled starter.

- **Catching**: a verb plus a policy for when it is worth a turn and a ball.
- **Training**: the objective table already sequences the gyms; it needs a
  "reach level N first" precondition per gym, with grinding as the action.

## How to measure it

The only honest test is **win rate on the same save state with the same seed**,
policy on versus off, counting wins, turns and faints. Three battle approaches
have already been reported working on this project and then failed on a harder
case, so the bar is gym-level evidence, and the Elite Four is the real bar.

Specific trap: `fullrun.py` is deterministic run-to-run but noisy **across
builds**, because any edit shifts how many `Random()` calls precede a frame. Use
a runtime switch to compare policies within one build, exactly as the
expected-damage work did.
