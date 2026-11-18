# HM locations, derived from the scripts

Extracted by scanning `data/maps/*_hns/scripts.inc` for `ITEM_HM_*` and pairing
each with the `FLAG_RECEIVED_HM_*` set nearby. Eight HMs found.

| HM | map | line | receipt flag |
|---|---|---|---|
| CUT | IlexForest_hns | 192 | FLAG_RECEIVED_HM_CUT |
| FLASH | SproutTower_3F_hns | 111 | FLAG_RECEIVED_HM_FLASH |
| FLY | CianwoodCity_hns | 42 | — |
| ROCK_SMASH | Route36_hns | 21 | FLAG_RECEIVED_HM_ROCK |
| STRENGTH | OlivineCity_Cafe_hns | 33 | FLAG_RECEIVED_HM_STRENGTH |
| SURF | AzaleaTown_hns | 48 | — |
| WATERFALL | IcePath_1F_hns | 17 | — |
| WHIRLPOOL | RocketHideout_B2F_hns | 502 | — |

## Why this matters to the router

HMs are **gates**, not rewards. When the navigator is blocked by water it should
be able to look up where Surf is obtained and make that the goal, instead of
treating the block as terminal. The objective table already carries most HMs as
main-line steps, so this index is the lookup the *router* needs, distinct from
the sequence the *objective runner* follows.

Three HMs matter most because they gate large regions: Surf (all water),
Strength (boulder rooms), Waterfall (Blackthorn onward).

## Caveats — do not treat this as final

1. **Four HMs have no receipt flag in the window searched.** Fly, Surf,
   Waterfall and Whirlpool may set a var instead, or set their flag further from
   the `ITEM_HM_*` reference than the +/-600 character window used here. Their
   completion tests need deriving properly before use.
2. **The SURF row is unverified and disagrees with the objective table**, which
   places HM Surf at Ecruteak Theater (objective 17). This extraction took the
   first alphabetical match, and `AzaleaTown_hns` merely *mentions* `ITEM_HM_SURF`
   -- a mention is not a grant. `EcruteakCity_Theater_hns` also references it.
   **Resolve which script actually gives it before relying on this row.**
3. This is a text scan, not a script-closure analysis. The objective table's
   entries were derived properly and should be preferred where they disagree.
