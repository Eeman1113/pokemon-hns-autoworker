#ifndef GUARD_AW_QUESTS_H
#define GUARD_AW_QUESTS_H

#include "global.h"

enum AwAction { AW_TALK_TO, AW_WALK_TO, AW_USE_ITEM, AW_FIGHT_TRAINER, AW_TAKE_ITEM, AW_WAIT_FOR_SCRIPT, AW_HAND_BACK };
enum AwConfidence { AW_DERIVED, AW_INFERRED, AW_UNKNOWN };

// 12 bytes/entry, const -> ROM. EWRAM holds only the u8 cursor.
struct AwObjective {
    u16 mapId;
    u8  x, y;
    u16 preFlag;   // 0 = none
    u16 doneFlag;  // 0 = none, agent must use a var test
    u8  action;    // enum AwAction
    u8  confidence;// enum AwConfidence
    const u8 *name;
};

#define AW_NUM_OBJECTIVES 30
extern const struct AwObjective gAwObjectives[AW_NUM_OBJECTIVES];

#endif
