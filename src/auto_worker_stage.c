#include "global.h"
#include "aw_quests.h"
#include "auto_worker.h"
#include "event_data.h"
#include "constants/flags.h"

// Where is the player in the story?
//
// The agent never infers this from dialogue -- it cannot, and projects that try
// are the ones that fail. The whole story is pre-declared as an ordered table
// and the answer is read out of the save file's own flags, which is why
// handing off at a fresh boot and handing off nine hours in are the same code
// path with no resume logic between them.
//
// The subtlety is that not every objective has a completion flag. Some steps
// only advance a VAR_*_STATE machine, and the derivation could not pin those
// down reliably. So detection does not look for the first incomplete objective
// -- that would stall forever on the first entry with no test. It looks for the
// furthest objective we can PROVE is complete, and resumes at the one after it.
// Unprovable entries in between are simply replayed, which is safe: every verb
// is idempotent (talking to a finished NPC does nothing).
u16 AW_DetectStage(void)
{
    u16 i;
    u16 resume = 0;

    for (i = 0; i < AW_NUM_OBJECTIVES; i++)
    {
        u16 done = gAwObjectives[i].doneFlag;

        if (done != 0 && FlagGet(done) == TRUE)
            resume = i + 1;     // proven past this one
    }

    return resume;              // == AW_NUM_OBJECTIVES means table exhausted
}

const struct AwObjective *AW_CurrentObjective(void)
{
    u16 i = AW_DetectStage();

    if (i >= AW_NUM_OBJECTIVES)
        return NULL;            // nothing left we know how to do: hand back
    return &gAwObjectives[i];
}

// Advisory only. The quest derivation attributes a setvar to its entry script
// while the guard that selects the branch usually lives in a parent label, so
// most preconditions came out as wildcards and cannot be trusted as gates. A
// failing precondition means "be warier" -- shorten the watchdog, hand back
// sooner -- never "refuse to try".
bool32 AW_PreconditionHolds(const struct AwObjective *o)
{
    if (o == NULL || o->preFlag == 0)
        return TRUE;
    return FlagGet(o->preFlag) == TRUE;
}
