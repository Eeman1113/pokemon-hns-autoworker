#ifndef GUARD_AW_QUESTS_H
#define GUARD_AW_QUESTS_H

#include "global.h"

enum AwAction { AW_TALK_TO, AW_WALK_TO, AW_USE_ITEM, AW_FIGHT_TRAINER,
                AW_TAKE_ITEM, AW_WAIT_FOR_SCRIPT, AW_HAND_BACK };
enum AwConfidence { AW_DERIVED, AW_INFERRED, AW_UNKNOWN };

// Progression in this hack runs on per-area VAR_*_STATE machines, not flags, so
// a condition must be able to test either. Flags are mostly receipts.
enum AwCondKind { AW_COND_NONE, AW_COND_FLAG_SET, AW_COND_FLAG_CLEAR,
                  AW_COND_VAR_EQ, AW_COND_VAR_GE, AW_COND_HAS_ITEM };

struct AwCond
{
    u16 id;        // flag id or var id
    u16 value;     // for the VAR_* kinds
    u8  kind;      // enum AwCondKind
};

struct AwObjective
{
    u16 mapId;
    u8  x, y;
    struct AwCond pre;    // ADVISORY. Guards usually live in a parent label, so
                          // most derived preconditions are wildcards; treat a
                          // failure as "be warier", never as "refuse to try".
    struct AwCond done;   // trustworthy wherever kind != AW_COND_NONE
    u8  action;           // enum AwAction
    u8  confidence;       // enum AwConfidence
    u8  localId;          // object-event local_id, or 0 when the target is a
                          // tile rather than an NPC. Scripts relocate NPCs
                          // permanently, so identity resolves the live
                          // position; the coordinates are only a routing hint.
    const u8 *name;
};

#define AW_NUM_OBJECTIVES 47
extern const struct AwObjective gAwObjectives[AW_NUM_OBJECTIVES];

// Evaluating a condition is the table's own semantics, so it ships with the
// table rather than being reimplemented in the engine. Replaces the old
// `FlagGet(obj->doneFlag)` pattern, which could not express var tests.
bool32 AW_EvalCond(const struct AwCond *c);

#endif
