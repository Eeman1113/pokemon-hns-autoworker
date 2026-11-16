#ifndef GUARD_AUTO_WORKER_H
#define GUARD_AUTO_WORKER_H

// Auto Worker — hands the controls to the game itself for the boring parts.
//
// Two mechanisms, deliberately kept separate:
//   * In battle, nothing is synthesised. The engine already ships an AI that can
//     drive a player-side battler, so the player's battler is simply assigned
//     that controller. This is why auto-battling is strong rather than scripted.
//   * Outside battle, the agent injects button presses at the single choke point
//     in ReadKeys(), so every consumer -- field, menu, dialogue -- sees exactly
//     what a human press would produce.
//
// Hand-back is unambiguous by construction: the agent writes only the *cooked*
// key fields and reads the human's intent only from the *raw* ones, which come
// straight off REG_KEYINPUT. They cannot alias, so a real B press is always
// distinguishable from a synthetic one with no heuristics.

void AutoWorker_Tick(void);              // after ReadKeys(), before any callback
bool32 AutoWorker_IsEnabled(void);
void AutoWorker_SetEnabled(bool32 enable);
void AutoWorker_Disable(void);

// Latched once at battle setup. IsAiVsAiBattle() is read at setup time, so the
// auto-battle decision must not change mid-battle; pressing B during a fight
// stops the agent but lets the AI finish the turn sequence it already owns.
void AutoWorker_LatchForBattle(void);
bool32 AutoWorker_IsAutoBattling(void);

struct AwObjective;
u16 AW_DetectStage(void);
const struct AwObjective *AW_CurrentObjective(void);
bool32 AW_PreconditionHolds(const struct AwObjective *o);

#endif // GUARD_AUTO_WORKER_H
