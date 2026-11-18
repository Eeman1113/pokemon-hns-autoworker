# Gates on the Heart & Soul main line

What stops an unattended agent, what it costs, and the data to get past it.
Audited against `pokehns-expansion @ 94660ab0`; the game repo was read only.
Machine-readable copies of everything below are in `gen/gates/`.

A gate here means something no amount of routing or battling solves. Losing a
fight is not a gate — you grind and come back. Holding HM Surf while nobody in
the party knows Surf is a gate, and it looks exactly like a navigation bug.

## Ranked

### Hard stops — the run ends here

**1. Field moves are gated on badges, not on the HM.** `src/field_move.c`
(`IS_HNS` branch) gates each field move behind a badge flag, and HnS re-assigns
that column relative to vanilla: Flash and Rock Smash on badge 1, Cut on 2,
Strength on 3, **Surf on 4**, Fly on 5, Dive on 7, Waterfall on 8. Three
conditions have to hold at once — hold the HM, have a party member that knows
the move, hold the badge — and the objective table checked none of them.
*Latent ordering hazard:* objective [18] grants HM Surf behind
`FLAG_BADGE03_GET`, but Surf stays unusable until `FLAG_BADGE04_GET` at [19],
and [20]–[22] all test `pre = FLAG_RECEIVED_HM_SURF`. Running in order happens
to work; a router that reorders on possession plans an unwalkable water route.
**Implemented.**

**2. Nobody teaches the HMs.** The game hands over the item and stops there.
Teaching is a bag-and-party UI flow (`ItemUseCB_TMHM`, `src/party_menu.c:5608`)
that ends in a move-replacement prompt when the target already knows four moves,
and picking wrong there deletes a move the team needs. **Implemented** as a
direct state write that prefers a Pokémon with a free slot — which skips the
prompt entirely — and never overwrites another HM move when it has to replace.

**3. The Dragon's Den elder quiz.** Objective [30], five three-option questions.
A multichoice opens with the cursor on option 0 and A commits it, and two of the
five questions reject 0. The existing dialogue escalation presses UP, which
cannot move a cursor already at 0, so the agent loops to `AW_DIALOGUE_GIVEUP`
and stops at the eighth badge. Accepted indices are in
`gen/gates/prompt_answers.json`; canonical answer is `0,0,1,0,2`.
**Implemented.**

**4. HM Waterfall is a ground item.** `finditem` at Ice Path 1F `(41,11)`. It is
the only HM in the game lying on the floor, nothing later hands it over, and
losing it costs badge 8's field move. **Data emitted, not yet routed.**

**5. The Machine Part is an invisible sign.** `bg_event` of type `sign` at
Cerulean Gym `(0,13)`, live only while `VAR_KANTO_ROCKET_STORY_STATE == 5`, and
the only thing that advances that var to 6. There is no object to walk up to.
**Data emitted, not yet routed.**

### Soft stops — expensive, not fatal

**6. Rocket HQ "password + trapdoors"** (objective [27], was a hand-back). Not a
puzzle. The two passwords are two conversations on B3F — GruntF5 at `(29,9)` and
Eto at `(4,16)`, each flag-guarded against double-counting — after which the
door at `(12,12)` opens on `VAR_ROCKET_PASSWORD == 2`. The trapdoor statues have
one master switch, a `bg_event` at B1F `(31,11)`. **It toggles**: a second press
re-arms all five traps, so press once and verify `FLAG_ROCKETHIDEOUT_SWITCH`.

**7. The Ice Path kimono girl.** She sits at 1F `(49,25)`, directly north of the
Blackthorn exit. The push is only offered when `VAR_FACING == DIR_SOUTH`, so the
agent has to approach from `(49,24)`. Answering No loops forever
(`IcePath_1F_EventScript_KimonoGirlRefused` jumps to itself) — survivable only
because `MSGBOX_YESNO` opens on YES, so plain A agrees. **Implemented**:
objective [29] rewritten from a hand-back to this interaction.

**8. Poké Balls are unbuyable until `VAR_NEWBARK_TOWN_STATE >= 5`.** The
Cherrygrove clerk serves a hardcoded Potion+Antidote list below that and the
badge-scaled list above it. Not a stop, but an early shopping attempt reads as a
failure. And **Revive first appears in the 8-badge tier** — an Elite Four
attempt stocked earlier cannot buy any.

### Inefficiencies

**9. The Ice Path boulder puzzle is not on the main line.** This was objective
[29]'s hand-back and the suspicion was wrong. 1F has no `PUSHABLE_BOULDER` at
all and warps Route 44 `(14,24)` straight through to Blackthorn `(49,32)`; the
eight boulders are on B1F/B2F, and the only thing behind them is a NeverMeltIce
on B4F. Downgraded from suspected hard stop to skippable.

**10. Shop inventory scales with badge count** via `SetShopItemsForSale(NULL)`.
Worth knowing when stocking, but `GetNumberOfBadges()` is `static` in
`src/shop.c:972` — a consumer outside that file has to re-derive it from the
badge flags rather than call it.

## Cleared — suspected gates that are not gates

**Time of day gates nothing on the main line.** The only time-of-day predicates
in any `*_hns` map are in `AkalaIsle_hns`, `MelemeleIsle_hns` and
`UlaUla_Forest_hns`, all optional Alola content. Zero hits across the sixteen
gyms and the League. The feared "agent waits forever for night" stall does not
exist. Ice Path 1F calls `SetTimeBasedEncounters`, which varies the wild table
and gates nothing.

**Yes/No prompts are safe, by accident.** Two prompts on the critical path
livelock on No: the Ice Path kimono girl above, and the New Bark lab police
episode (`NewBarkTown_Lab_hns/scripts.inc:374-400`, two nested No-loops, firing
inside the first hour). Both are survivable only because `MSGBOX_YESNO` opens
with the cursor on YES. **Anything that teaches this agent to answer prompts
with B livelocks it twice over**, once in the first hour.

**The Route 7 Saffron guard does not block.** `MSGBOX_NPC` flavour text about
the Power Plant; `FLAG_RETURNED_MACHINE_PART` only changes what he says.

## Undetermined

- **Fly destinations.** Not audited. Fly is unlocked at badge 5 and its
  destination rules were not traced.
- **The Elite Four as a single run.** Not audited beyond the Revive finding
  above. Whether the four rooms can be re-entered after a loss is unverified.
- **Which species can learn which HM** is left to the engine
  (`CanLearnTeachableMove`) rather than baked into a table, so a species change
  upstream cannot silently invalidate it.
- `MOVE_WHIRLPOOL`'s badge is **inferred**: `field_move.c` has no
  `IsFieldMoveUnlocked_Whirlpool`, and badge 8 is a guess by grouping with
  Waterfall.
- The objective table has **47** entries (0–46), not the 45 the brief assumed.
  Three were hand-backs; two are now resolved ([27] by data, [29] by code).

## What was touched

Everything is in `/Users/eemanmajumder/hns/gates-tree`, branched from
`94660ab0`. Patch: `gen/gates/gates.patch`.

- `include/aw_gates.h` — new
- `src/aw_gates.c` — new
- `src/script_menu.c` — one exported predicate appended (`AW_IsMultichoiceActive`;
  the multichoice task is `static`, so it cannot live elsewhere)
- `src/auto_worker.c` — one include, one tick call, one prompt-override branch
- `src/aw_quests.c` — one include; objective [29] rewritten

Builds clean, `EXIT=0`. EWRAM 97.36%, IWRAM 78.43% — unchanged from baseline;
all new tables are `const` and ROM-resident. Nothing under `harness/` was
touched and no save state was written.
