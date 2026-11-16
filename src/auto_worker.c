#include "global.h"
#include "auto_worker.h"
#include "main.h"
#include "overworld.h"
#include "script.h"
#include "field_player_avatar.h"
#include "event_data.h"
#include "constants/songs.h"
#include "sound.h"
#include "aw_quests.h"

// Raw keys bypass the L=A remap and come straight off REG_KEYINPUT, so they are
// the human's unmediated intent. There is a JOY_HELD_RAW in global.h but no
// newly-pressed equivalent; the yield check needs the edge, not the level.
#define JOY_NEW_RAW(button) TEST_BUTTON(gMain.newKeysRaw, button)

enum {
    AW_IDLE,        // waiting for the field to be ours to drive
    AW_ADVANCE,     // a textbox or script is up: tap A to move it along
    AW_PATROL,      // walking to provoke wild encounters
    AW_TURN,        // blocked: face a new direction before walking again
};

// IWRAM has ~2.2 KB free across the whole ROM, so none of this may live there.
// EWRAM has ~18 KB and this struct is 16 bytes of it.
struct AutoWorkerState
{
    bool8 enabled;
    bool8 battleLatch;      // decided at battle setup, stable for the battle
    u8 mode;
    u8 dir;                 // DIR_* currently being walked
    u8 holdFrames;          // frames left to hold the synthetic press
    u8 cooldown;            // frames to wait before acting again
    u8 stuckCount;          // consecutive checks with no position change
    u8 turnsTried;          // directions tried since last successful step
    u16 synthKeys;          // asserted into the cooked fields this frame
    u16 stage;              // index into gAwObjectives, resolved at hand-off
    u16 watchdog;           // frames since the agent last made progress
    s16 lastX, lastY;
};

static EWRAM_DATA struct AutoWorkerState sAW = {0};

// Walking is deliberately driven by synthesised D-pad presses rather than by
// ScriptMovement_*. The movement-script API is what cutscenes use and it
// bypasses the normal field-input path, which means tall grass, ledges and
// trigger tiles would silently not fire -- the agent would appear to work while
// quietly playing a different game.
static const u8 sPatrolDirs[] = { DIR_NORTH, DIR_EAST, DIR_SOUTH, DIR_WEST };

static u16 DirToKey(u8 dir)
{
    switch (dir)
    {
    case DIR_NORTH: return DPAD_UP;
    case DIR_SOUTH: return DPAD_DOWN;
    case DIR_WEST:  return DPAD_LEFT;
    case DIR_EAST:  return DPAD_RIGHT;
    }
    return 0;
}

static void ResetRun(void)
{
    sAW.mode = AW_IDLE;
    sAW.dir = DIR_NORTH;
    sAW.holdFrames = 0;
    sAW.cooldown = 0;
    sAW.stuckCount = 0;
    sAW.turnsTried = 0;
    sAW.synthKeys = 0;
    sAW.watchdog = 0;
    sAW.lastX = -1;
    sAW.lastY = -1;
}

bool32 AutoWorker_IsEnabled(void)
{
    return sAW.enabled;
}

void AutoWorker_Disable(void)
{
    sAW.enabled = FALSE;
    sAW.synthKeys = 0;
    // The latch is intentionally left alone: a battle already handed to the AI
    // finishes under the AI rather than stranding the player mid-turn.
}

void AutoWorker_SetEnabled(bool32 enable)
{
    if (enable)
    {
        const struct AwObjective *obj;

        ResetRun();
        sAW.stage = AW_DetectStage();
        obj = AW_CurrentObjective();

        // Refuse to start rather than start and flail. If the table is
        // exhausted or the next step is one the agent knows it cannot do, the
        // honest move is to say so immediately.
        if (obj == NULL || obj->action == AW_HAND_BACK)
        {
            PlaySE(SE_FAILURE);
            sAW.enabled = FALSE;
            return;
        }
        sAW.enabled = TRUE;
    }
    else
    {
        AutoWorker_Disable();
    }
}

void AutoWorker_LatchForBattle(void)
{
    sAW.battleLatch = sAW.enabled;
}

bool32 AutoWorker_IsAutoBattling(void)
{
    return sAW.battleLatch;
}

// ---------------------------------------------------------------------------

static bool32 ShouldRefuseToRun(void)
{
    // Injecting while linked would desync the other console: main.c zeroes
    // newKeys when keys are arriving over the link, so the two machines would
    // disagree about what was pressed.
    if (IsOverworldLinkActive())
        return TRUE;
    return FALSE;
}

static void ChooseOverworldAction(void)
{
    s16 x, y;

    if (sAW.cooldown)
    {
        sAW.cooldown--;
        return;
    }

    // A script owning the field means a textbox, a cutscene or an NPC. Tapping A
    // advances text; it is also how the agent gets through the post-battle
    // messages that bracket every wild encounter.
    if (ArePlayerFieldControlsLocked() || ScriptContext_IsEnabled())
    {
        sAW.mode = AW_ADVANCE;
        sAW.synthKeys = A_BUTTON;
        sAW.cooldown = 8;       // text advances on a press, not on a hold
        sAW.watchdog = 0;
        return;
    }

    PlayerGetDestCoords(&x, &y);

    // Stuck detection: if the agent has not moved for a while it is walking into
    // a wall or a corner, so turn rather than grind against it forever. A
    // silently flailing agent is worse than one that stops.
    if (x == sAW.lastX && y == sAW.lastY)
    {
        if (++sAW.stuckCount >= 12)
        {
            sAW.stuckCount = 0;
            sAW.turnsTried++;
            sAW.dir = sPatrolDirs[(sAW.dir + sAW.turnsTried) & 3];
            sAW.mode = AW_TURN;

            // Four failed directions means the agent is boxed in and no amount
            // of turning will help. Hand back rather than pretend.
            if (sAW.turnsTried >= 8)
            {
                PlaySE(SE_FAILURE);
                AutoWorker_Disable();
                return;
            }
        }
    }
    else
    {
        sAW.stuckCount = 0;
        sAW.turnsTried = 0;
        sAW.lastX = x;
        sAW.lastY = y;
    }

    sAW.mode = AW_PATROL;
    sAW.synthKeys = DirToKey(sAW.dir);
}

void AutoWorker_Tick(void)
{
    sAW.synthKeys = 0;

    if (!sAW.enabled)
    {
        // Outside a battle the latch tracks the setting, so the next battle
        // starts under whichever regime is current.
        if (!gMain.inBattle)
            sAW.battleLatch = FALSE;
        return;
    }

    // The human always wins. Raw keys cannot have been written by the agent, so
    // this is a real press with no ambiguity and no race.
    if (JOY_NEW_RAW(B_BUTTON))
    {
        PlaySE(SE_SELECT);
        AutoWorker_Disable();
        return;
    }

    if (ShouldRefuseToRun())
    {
        AutoWorker_Disable();
        return;
    }

    if (gMain.inBattle)
    {
        // Nothing to drive: the battle is being played by the engine's own AI
        // through the player-side controller. Synthesising keys here would
        // fight it for the same inputs.
        sAW.battleLatch = TRUE;
        return;
    }

    sAW.battleLatch = sAW.enabled;
    ChooseOverworldAction();

    // Assert into the cooked fields only. heldKeysRaw/newKeysRaw are left alone
    // so the A+B+START+SELECT soft reset always remains available to the human.
    gMain.newKeys |= sAW.synthKeys;
    gMain.heldKeys |= sAW.synthKeys;
    gMain.newAndRepeatedKeys |= sAW.synthKeys;
}
