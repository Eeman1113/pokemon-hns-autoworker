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
#include "auto_worker_nav.h"
#include "constants/map_groups.h"
#include "constants/maps.h"
#include "string_util.h"
#include "strings.h"
#include "constants/characters.h"
#include "constants/battle.h"
#include "battle.h"
#include "pokemon.h"
#include "constants/event_objects.h"
#include "constants/species.h"
#include "event_object_movement.h"
#include "global.fieldmap.h"
#include "fieldmap.h"
#include "wild_encounter.h"
#include "aw_centres.h"

// Raw keys bypass the L=A remap and come straight off REG_KEYINPUT, so they are
// the human's unmediated intent. There is a JOY_HELD_RAW in global.h but no
// newly-pressed equivalent; the yield check needs the edge, not the level.
#define JOY_NEW_RAW(button) TEST_BUTTON(gMain.newKeysRaw, button)

// A cutscene can legitimately hold the field for a long time, so these are
// generous: escalate at ~15s, give up at ~45s. Giving up hands control back,
// which is always safe; flailing forever is not.
// These count ACTIONS, not frames. The dialogue branch returns after setting a
// cooldown, so the counter advances roughly once every 8-10 frames -- measured
// at 40 increments per 360 frames. Thresholds expressed in frames would be an
// order of magnitude too slow to ever fire inside a test run.
#define AW_DIALOGUE_ESCALATE    6    // ~55 frames: text should have moved by now
#define AW_DIALOGUE_GIVEUP    400    // ~3600 frames before handing back

// ~5 minutes of game time stuck on one tile, one map and one objective.
// Long cutscenes run well under this; a genuine loop runs forever without it.
#define AW_BACKSTOP_FRAMES  18000

enum {
    AW_IDLE,        // waiting for the field to be ours to drive
    AW_ADVANCE,     // a textbox or script is up: tap A to move it along
    AW_TRAVEL,      // routing toward the current objective
    AW_INTERACT,    // arrived: talk, take, or trigger a battle
};

// IWRAM has ~2.2 KB free across the whole ROM, so none of this may live there.
// EWRAM has ~18 KB and this struct is 16 bytes of it.
// Heal sub-state. The point of naming these is that each one owns exactly one
// AW_Nav_SetGoal call, so the "set the goal once" rule is visible rather than
// remembered.
enum
{
    AW_HEAL_NEED_GOAL,      // no route yet; pick a Centre
    AW_HEAL_TO_CENTRE,      // walking to the Centre map
    AW_HEAL_TO_NURSE,       // inside a Centre, walking to the counter
};

struct AutoWorkerState
{
    bool8 enabled;
    bool8 battleLatch;      // decided at battle setup, stable for the battle
    u8 mode;
    u8 holdFrames;          // frames left to hold the synthetic press
    u8 cooldown;            // frames to wait before acting again
    u16 synthKeys;          // asserted into the cooked fields this frame
    u16 stage;              // index into gAwObjectives, resolved at hand-off
    bool8 objDone;
    bool8 navGoalSet;
    u16 noProgress;         // frames since the player's tile last changed
    u8 faceFrames;          // turn-toward-NPC presses before interacting
    u16 dialogueFrames;     // consecutive frames the field has been script-locked
    bool8 sawScript;        // an interaction we performed actually started a script
    u8 stopReason;          // AW_STOP_* -- why the last run ended, for diagnosis
    u8 lastMapGroup, lastMapNum;  // to notice a warp and re-plan
    u8 navBlock;            // AW_Nav_BlockReason() captured at the moment of a stop
    u8 lastNavRet;          // last AW_Nav_Tick() return, so the loop is observable
    u16 dbgHere;            // navigator's view of 'which map am I on'
    u8 dbgWhich;            // which UNREACHABLE branch fired
    u8 navFails;            // consecutive BLOCKED/NO_ROUTE verdicts
    u8 dbgDistGoal, dbgDistHere;
    u16 dbgGoalIdx, dbgQ, dbgTgtCol, dbgPlrCol;
    u16 dbgMapId;           // obj->mapId as the runner saw it, at SetGoal time
    u8 dbgGroup, dbgNum;    // what was actually handed to the navigator
    // Latched EVERY tick, not only at a stop. The escape-hop mechanism was
    // built and shipped without anyone being able to tell whether it ever
    // fired, because its counters were only readable from a stop that the
    // mechanism itself was supposed to prevent.
    u16 dbgEscape;          // packed: fired<<10 | arrived<<4 | mode<<3 | tries
    u8 dbgNavState;         // navigator's own state machine
    u8 dbgBadEdges;         // blacklist size -- 'did the re-route mechanism fire'
    u16 dbgNavTgt;          // current intra-map target, x<<8|y
    u16 dbgNavWin;          // window origin, x<<8|y
    u16 doneVarBase;        // the done-condition var as it was when this objective began
    bool8 doneVarValid;     // doneVarBase holds a real reading
    // Backstop watchdog. Deliberately reset ONLY by real progress -- a changed
    // tile, a changed map, or a changed objective -- and never by any per-mode
    // branch. The two existing watchdogs are both resettable from inside the
    // dialogue path (`noProgress` explicitly, `dialogueFrames` whenever the
    // field unlocks), so a lock -> unlock -> lock loop escapes both and stalls
    // forever. That is a latent permanent hang anywhere in the objective table.
    u16 watchdog;
    u16 wdStage;            // the objective the watchdog is timing
    s16 wdX, wdY;           // the tile it was last reset at
    u8 wdMapGroup, wdMapNum;
    u16 battleFrames;       // consecutive frames spent inside a battle
    bool8 healing;          // detoured to a Pokemon Centre
    u8 healStep;            // AW_HEAL_* sub-state; each owns one SetGoal call
    s16 lastX, lastY;
    s16 curGoalX, curGoalY;  // goal last handed to the navigator
};

static EWRAM_DATA struct AutoWorkerState sAW = {0};

// Test seam: 1 = choose moves with the engine's scoring AI, 0 = fall back to
// mashing A. Ships ON; the harness pokes it to 0 to measure the baseline on
// the SAME build, so the comparison has exactly one variable.
// Test seam, mirroring gAWSuppressEncounters: EWRAM_DATA must be zero-init
// (.sbss rejects a non-zero initialiser, and a plain .data global was dropped
// by --gc-sections even though it was referenced). Hence inverted sense:
// 0 = AI move choice (the shipped default), 1 = mash A for the baseline.
EWRAM_DATA u8 gAWBattleAIOff = 0;

// Walking is deliberately driven by synthesised D-pad presses rather than by
// ScriptMovement_*. The movement-script API is what cutscenes use and it
// bypasses the normal field-input path, which means tall grass, ledges and
// trigger tiles would silently not fire -- the agent would appear to work while
// quietly playing a different game.

static void ResetRun(void)
{
    sAW.mode = AW_IDLE;
    sAW.holdFrames = 0;
    sAW.cooldown = 0;
    sAW.synthKeys = 0;
    sAW.watchdog = 0;
    sAW.dialogueFrames = 0;
    sAW.sawScript = FALSE;
    sAW.watchdog = 0;
    sAW.battleFrames = 0;
    sAW.healing = FALSE;
    sAW.healStep = AW_HEAL_NEED_GOAL;
    sAW.wdX = -1;
    sAW.wdY = -1;
    sAW.faceFrames = 0;
    sAW.lastX = -1;
    sAW.lastY = -1;
    sAW.curGoalX = -1;
    sAW.curGoalY = -1;
}

bool32 AutoWorker_IsEnabled(void)
{
    return sAW.enabled;
}

void AutoWorker_Disable(void)
{
    sAW.enabled = FALSE;
    sAW.synthKeys = 0;
    sAW.navGoalSet = FALSE;
    // The latch is intentionally left alone: a battle already handed to the AI
    // finishes under the AI rather than stranding the player mid-turn.
}

static void SnapshotDoneVar(const struct AwObjective *obj);

// The rival's name, applied when the agent takes over a file early enough to
// meet the naming step itself. Mashing A at a naming screen yields whatever the
// cursor happens to sit on, so the name is written directly instead -- it is a
// constant, not a decision, and the naming grid is fiddly input work for no
// benefit.
//
// Only ONE field matters here. ExpandPlaceholder_RivalName (string_util.c:481)
// reads gSaveBlock2Ptr->rivalName under IS_HNS; the SaveBlock1 copy is the FRLG
// path and is dead in this hack, so writing it would be noise.
static const u8 sAwRivalName[] = _("RAVANA");

// Never overwrite a name a human chose. A new file is stamped "SILVER" by
// new_game.c:308 rather than left empty, so "unset" means empty OR still that
// default -- checking only for empty would silently rewrite part of someone's
// save.
static void ApplyRivalName(void)
{
    if (gSaveBlock2Ptr->rivalName[0] == EOS
     || StringCompare(gSaveBlock2Ptr->rivalName, gText_ExpandedPlaceholder_Silver) == 0)
    {
        StringCopy(gSaveBlock2Ptr->rivalName, sAwRivalName);
    }
}

void AutoWorker_SetEnabled(bool32 enable)
{
    if (enable)
    {
        const struct AwObjective *obj;

        ResetRun();
        ApplyRivalName();
        AW_Nav_Reset();
        sAW.navGoalSet = FALSE;
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
        // Baseline the completion variable at hand-off too, not only when the
        // cursor advances -- otherwise the very first objective of a session
        // has no reference point for the change check.
        SnapshotDoneVar(obj);
        sAW.lastMapGroup = gSaveBlock1Ptr->location.mapGroup;
        sAW.lastMapNum = gSaveBlock1Ptr->location.mapNum;
        sAW.enabled = TRUE;
    }
    else
    {
        AutoWorker_Disable();
    }
}

void AutoWorker_LatchForBattle(void)
{
    // The latch now means "drive the battle menus", NOT "swap the controller".
    //
    // Handing the player's battler to SetControllerToPlayerPartner froze the
    // console outright -- gMain.vblankCounter1 stopped advancing, so no in-ROM
    // code ran and no watchdog could fire. It was first blamed on wild battles
    // and "fixed" by limiting it to trainer battles, on the strength of one
    // trainer battle that happened to resolve; the first rival battle then
    // froze identically. The controller swap is gone for good.
    //
    // What replaced it drives the normal player controller with synthesised
    // key presses, choosing the move the engine's own scoring AI picked. Same
    // input path a human uses, so the engine's control flow is untouched.
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

// ---------------------------------------------------------------------------
// The objective runner.
//
// One objective at a time: route to its map and tile, then perform its verb.
// Everything is expressed as button presses because that is the only way to
// reach the game the same way a human does -- wild encounters, ledges, warps
// and trigger tiles all hang off the normal field-input path, and driving the
// player with ScriptMovement_* would silently skip every one of them.


// An objective's coordinates are where the NPC *starts*, not where it is.
// Scripts relocate object events permanently through setobjectxyperm, which
// rewrites the save-block copy of the template while the ROM's template keeps
// the original tile. The first room of the game demonstrates it: Mom's
// compiled template is (2,2), but after the intro she stands at (8,4), so an
// agent routed to (2,2) arrives at an empty tile and presses A at a wall
// forever. Identity survives that; coordinates do not.
//
// Returns TRUE and fills *x/*y with the NPC's CURRENT tile when the objective
// names one and it is loaded on this map.
static bool32 LiveTargetCoords(const struct AwObjective *obj, s16 *x, s16 *y)
{
    u8 id;

    if (obj->localId == 0)
        return FALSE;
    // NOTE the inverted convention: this returns TRUE when the object was NOT
    // found (src/event_object_movement.c:1542 sets *id to OBJECT_EVENTS_COUNT
    // and returns TRUE). Reading it the natural way round silently disabled
    // live NPC targeting and sent the agent to the stale template tile.
    if (TryGetObjectEventIdByLocalIdAndMap(obj->localId,
                                           MAP_NUM(obj->mapId),
                                           MAP_GROUP(obj->mapId), &id))
        return FALSE;
    if (!gObjectEvents[id].active)
        return FALSE;

    *x = gObjectEvents[id].currentCoords.x;
    *y = gObjectEvents[id].currentCoords.y;
    return TRUE;
}

// Which way to press to face a tile one step away.
static u16 KeyToward(s16 dx, s16 dy)
{
    if (dx == 1 && dy == 0)  return DPAD_RIGHT;
    if (dx == -1 && dy == 0) return DPAD_LEFT;
    if (dx == 0 && dy == 1)  return DPAD_DOWN;
    if (dx == 0 && dy == -1) return DPAD_UP;
    return 0;
}

// Thresholds for VAR-based completion tests are the least trustworthy part of
// the objective table: the derivation reads every setvar on a variable and has
// to guess which one THIS step produces. Objective 0 shipped with ">= 5" when
// Mom's script sets 2, so the agent talked to her forever waiting for a state
// three objectives away.
//
// Snapshotting the variable when an objective begins turns that class of error
// from a permanent stall into a single redundant interaction: if our own script
// moved the variable at all, the step did something, and the agent moves on.
static bool32 DoneCondIsVar(const struct AwObjective *obj)
{
    return obj->done.kind == AW_COND_VAR_EQ || obj->done.kind == AW_COND_VAR_GE;
}

static void SnapshotDoneVar(const struct AwObjective *obj)
{
    if (DoneCondIsVar(obj))
    {
        sAW.doneVarBase = VarGet(obj->done.id);
        sAW.doneVarValid = TRUE;
    }
    else
    {
        sAW.doneVarValid = FALSE;
    }
}

// Stop reasons exist so a halt is diagnosable from outside. Without one, every
// FinishRun() looks identical in memory and the only way to tell a dialogue
// timeout from an unreachable goal is to guess.
#define AW_STOP_NONE        0
#define AW_STOP_TABLE_END   1
#define AW_STOP_HAND_BACK   2
#define AW_STOP_DIALOGUE    3
#define AW_STOP_NO_PROGRESS 4
#define AW_STOP_NO_ROUTE    5
#define AW_STOP_NAV_BLOCKED 6
#define AW_STOP_HUMAN_B     7
#define AW_STOP_LINK        8
#define AW_STOP_BACKSTOP    9
#define AW_STOP_BATTLE_HANG 10

// A run that cannot survive losing a fight cannot finish the game, so low HP
// pre-empts the objective table entirely.
static bool32 PartyNeedsHealing(void)
{
    u32 i, cur = 0, max = 0;
    bool32 fainted = FALSE;

    for (i = 0; i < gPlayerPartyCount && i < PARTY_SIZE; i++)
    {
        u32 hp, mx;

        if (GetMonData(&gPlayerParty[i], MON_DATA_IS_EGG))
            continue;
        if (GetMonData(&gPlayerParty[i], MON_DATA_SPECIES) == SPECIES_NONE)
            continue;
        hp = GetMonData(&gPlayerParty[i], MON_DATA_HP);
        mx = GetMonData(&gPlayerParty[i], MON_DATA_MAX_HP);
        cur += hp;
        max += mx;
        if (hp == 0)
            fainted = TRUE;
    }
    if (max == 0)
        return FALSE;
    // A third is the point past which one unlucky wild encounter can end the
    // run. Waiting for an actual faint is waiting too long on a one-mon party.
    return fainted || cur * 3 < max;
}

static bool32 PartyFullyHealed(void)
{
    u32 i;

    for (i = 0; i < gPlayerPartyCount && i < PARTY_SIZE; i++)
    {
        if (GetMonData(&gPlayerParty[i], MON_DATA_IS_EGG))
            continue;
        if (GetMonData(&gPlayerParty[i], MON_DATA_SPECIES) == SPECIES_NONE)
            continue;
        if (GetMonData(&gPlayerParty[i], MON_DATA_HP)
         != GetMonData(&gPlayerParty[i], MON_DATA_MAX_HP))
            return FALSE;
    }
    return TRUE;
}

// Find the nurse by GRAPHICS ID rather than by coordinates: Centre layouts
// differ, but the counter attendant is the same sprite in every one of them.
// Locate the nurse by sprite, then confirm against the table.
//
// Sprite alone is not enough: of the nurse-graphics objects in the game, some
// heal nobody -- BattleFrontier_Lounge9 carries the sprite with a NULL script.
// A runtime scan cannot see the script, so this cross-checks the map against
// the generated Centre table, which was built from the script data.
static void FinishRun(u32 sound)
{
    PlaySE(sound);
    AutoWorker_Disable();
}

static void StopWith(u8 reason, u32 sound)
{
    sAW.stopReason = reason;
    FinishRun(sound);
}

// Re-read progress from the save file. Cheap (a few hundred flag tests) and
// only needed when something might have changed, not every frame.
static void RefreshObjective(void)
{
    sAW.stage = AW_DetectStage();
    sAW.objDone = FALSE;
    sAW.noProgress = 0;
    sAW.doneVarValid = FALSE;
    if (sAW.stage < AW_NUM_OBJECTIVES)
        SnapshotDoneVar(&gAwObjectives[sAW.stage]);
    AW_Nav_Reset();
}

static void ChooseOverworldAction(void)
{
    const struct AwObjective *obj;
    s16 x, y, tx, ty;
    s16 goalX, goalY;

    if (sAW.cooldown)
    {
        sAW.cooldown--;
        return;
    }

    // A script owning the field means a textbox, a cutscene or an NPC talking.
    // Tapping A advances it, and this is also how the agent gets through the
    // messages that bracket every wild encounter.
    if (ArePlayerFieldControlsLocked() || ScriptContext_IsEnabled())
    {
        // A script starting right after our own A press is the only evidence
        // available that the interaction landed, for objectives the game gives
        // us no flag to test.
        if (sAW.mode == AW_INTERACT)
            sAW.sawScript = TRUE;

        sAW.mode = AW_ADVANCE;

        // This path needs its own watchdog. The position-based stuck detector
        // below is unreachable from here, so without this the agent taps A
        // forever at any screen A cannot dismiss -- and HnS puts one in the
        // first room of the game: bumping the bedroom wall clock opens a
        // clock-setting UI where A means "confirm" and the D-pad is what it
        // actually wants. Observed: 4 distinct tiles in 12,000 frames.
        if (++sAW.dialogueFrames > AW_DIALOGUE_GIVEUP)
        {
            StopWith(AW_STOP_DIALOGUE, SE_FAILURE);
            return;
        }

        if (sAW.dialogueFrames > AW_DIALOGUE_ESCALATE)
        {
            // A alone cannot clear every blocking screen, and the specific way
            // it fails is worth stating: a Yes/No menu whose cursor STARTS on
            // NO. The wall clock in the player's own bedroom is exactly that --
            // CreateYesNoMenu(..., 1) in src/wallclock.c:1 puts the cursor on
            // NO, so an agent that advances dialogue by pressing A selects NO
            // forever and walks back into the clock it just left. Measured: an
            // 18-frame cycle through HandleConfirmInput -> HandleInput ->
            // AskConfirm, 12,000 frames, 4 tiles, no progress.
            //
            // Menu_ProcessInputNoWrapClearOnChoose does not wrap, so UP from NO
            // lands on YES and UP from YES stays on YES. Pressing UP before A
            // therefore reaches YES from either default, which is what makes
            // this a general fix rather than a wall-clock special case.
            // Order matters, and getting it wrong is invisible without a
            // frame trace. The confirm menu does not exist until A opens it,
            // so pressing UP first just feeds a D-pad press to the clock face
            // and the following A merely re-opens the menu with the cursor
            // still on NO. Measured that way round: a 44-frame cycle that
            // never escapes.
            //
            //   A  -- opens the Yes/No (cursor starts on NO)
            //   UP -- no-wrap, so NO becomes YES, and YES stays YES
            //   A  -- takes YES
            //
            // On plain dialogue this degrades to A, A with a harmless UP in
            // between, so it costs nothing where text was already advancing.
            // Strict A/UP alternation, and the period must be 2 rather than
            // 3. A three-phase A,UP,A cycle looks right in isolation but wraps
            // to ...A,UP,A,A,UP,A..., and that doubled A answers the menu NO
            // before UP ever arrives. Traced: the A landed on
            // HandleConfirmInput every time while UP hit HandleInput, where it
            // does nothing. Alternating gives the sequence the menu needs:
            //
            //   A  -> AskConfirm opens, cursor on NO
            //   UP -> cursor moves to YES (no-wrap, so YES is a fixed point)
            //   A  -> YES taken
            if ((sAW.dialogueFrames - AW_DIALOGUE_ESCALATE) & 1)
                sAW.synthKeys = DPAD_UP;
            else
                sAW.synthKeys = A_BUTTON;
            sAW.cooldown = 10;
            return;
        }

        sAW.synthKeys = A_BUTTON;
        sAW.cooldown = 8;           // text advances on a press, not a hold
        sAW.noProgress = 0;
        return;
    }
    sAW.dialogueFrames = 0;         // the field is ours again

    // ---- healing pre-empts everything else on the field --------------------
    if (sAW.healing || PartyNeedsHealing())
    {
        s16 px2, py2;

        if (sAW.healing && PartyFullyHealed())
        {
            sAW.healing = FALSE;        // done; fall through to the objective
            sAW.healStep = AW_HEAL_NEED_GOAL;
            sAW.navGoalSet = FALSE;
            AW_Nav_Reset();
        }
        else
        {
            if (!sAW.healing)
            {
                // Entering heal mode. Forget the objective's route once, here,
                // rather than re-deciding every frame below.
                sAW.healing = TRUE;
                sAW.healStep = AW_HEAL_NEED_GOAL;
                AW_Nav_Reset();
            }
            PlayerGetDestCoords(&px2, &py2);

            {
                const struct AwCentre *centreHere = AW_FindCentre(
                    (u16)((gSaveBlock1Ptr->location.mapGroup << 8)
                          | gSaveBlock1Ptr->location.mapNum));

                if (centreHere != NULL)
                {
                    // Stand on the table's approach tile, not next to the nurse.
                    //
                    // Gen 3 puts an impassable MB_COUNTER tile between the two:
                    // in Cherrygrove the nurse is at local (7,2), the counter at
                    // (7,3) and the player at (7,4), and the engine forwards the
                    // interaction across the counter. The old code aimed at
                    // nurse+1 -- the counter itself -- which no path can ever
                    // reach, so the agent sat on the Centre's doormat at (7,8)
                    // until the NPC-wait timer expired and reported UNREACH.
                    // Every coordinate here was verified against map data by the
                    // Centre audit (30/30), so it is used rather than guessed.
                    s16 ax = centreHere->approachX + MAP_OFFSET;
                    s16 ay = centreHere->approachY + MAP_OFFSET;

                    if (px2 == ax && py2 == ay)
                    {
                        // In position. Face the nurse along the counter axis --
                        // the delta can be two tiles, so it is reduced to a unit
                        // step before being turned into a key.
                        s16 fx = (s16)centreHere->nurseX - (s16)centreHere->approachX;
                        s16 fy = (s16)centreHere->nurseY - (s16)centreHere->approachY;

                        if (fx != 0) fx = (fx > 0) ? 1 : -1;
                        if (fy != 0) fy = (fy > 0) ? 1 : -1;

                        if (sAW.faceFrames < 6)
                        {
                            sAW.faceFrames++;
                            sAW.synthKeys = KeyToward(fx, fy);
                            sAW.cooldown = 2;
                        }
                        else
                        {
                            sAW.synthKeys = A_BUTTON;
                            sAW.cooldown = 12;
                        }
                        return;
                    }

                    sAW.faceFrames = 0;
                    if (sAW.healStep != AW_HEAL_TO_NURSE)
                    {
                        if (!AW_Nav_SetGoal(gSaveBlock1Ptr->location.mapGroup,
                                            gSaveBlock1Ptr->location.mapNum,
                                            centreHere->approachX,
                                            centreHere->approachY))
                        {
                            // Cannot reach the counter from inside the Centre.
                            // Not fatal: fainting heals too. Give up the detour
                            // rather than the run.
                            sAW.healing = FALSE;
                            sAW.healStep = AW_HEAL_NEED_GOAL;
                            sAW.navGoalSet = FALSE;
                            AW_Nav_Reset();
                            return;
                        }
                        sAW.healStep = AW_HEAL_TO_NURSE;
                    }
                }
                else
                {
                // Not in a Centre yet. Route to the nearest real one.
                //
                // NOT lastHealLocation. That reads as the obvious choice and is
                // wrong early: the game only records a heal destination once the
                // player has used a Centre, so before the first visit it points
                // at the player's own front door in New Bark Town -- verified by
                // reading the save block, which returned (0,0) at (20,12). The
                // agent dutifully walked home, found no nurse and looped. The
                // opening is also the worst stretch in the game for this: no
                // Centre exists at all until Cherrygrove, three hops away.
                // Once, not every frame. AW_Nav_SetGoal rebuilds the route
                // and resets the search to IDLE, so calling it per tick left
                // the navigator permanently restarting and the agent standing
                // still -- healing latched, goal never progressed, 40,000
                // frames of nothing. The objective path has always guarded
                // this; the heal path did not.
                if (sAW.healStep == AW_HEAL_NEED_GOAL)
                {
                    const struct AwCentre *centre =
                        AW_FindNearestCentre(gSaveBlock1Ptr->location.mapGroup,
                                             gSaveBlock1Ptr->location.mapNum);

                    if (centre == NULL
                     || !AW_Nav_SetGoal(MAP_GROUP(centre->mapId),
                                        MAP_NUM(centre->mapId),
                                        centre->approachX, centre->approachY))
                    {
                        // No reachable Centre is not a failure, and stopping
                        // here was wrong. Fainting is the game's own recovery
                        // mechanism: the party is restored in full and the
                        // player wakes at their last heal point. In the opening
                        // it is the ONLY heal available, because no Centre
                        // exists before Cherrygrove -- so an agent that quits
                        // when it cannot reach one quits precisely where the
                        // game expects you to faint and carry on.
                        //
                        // Abandon the detour and keep playing. Losing a battle
                        // costs time; stopping costs the run.
                        sAW.healing = FALSE;
                        sAW.healStep = AW_HEAL_NEED_GOAL;
                        sAW.navGoalSet = FALSE;
                        AW_Nav_Reset();
                        return;
                    }
                    sAW.healStep = AW_HEAL_TO_CENTRE;
                }
                }
            }
            sAW.navGoalSet = TRUE;
            switch (AW_Nav_Tick())
            {
            case AW_NAV_UP:    sAW.synthKeys = DPAD_UP;    return;
            case AW_NAV_DOWN:  sAW.synthKeys = DPAD_DOWN;  return;
            case AW_NAV_LEFT:  sAW.synthKeys = DPAD_LEFT;  return;
            case AW_NAV_RIGHT: sAW.synthKeys = DPAD_RIGHT; return;
            case AW_NAV_ARRIVED:
            case AW_NAV_WAIT:  return;
            default:
                sAW.navBlock = AW_Nav_BlockReason();
                StopWith(AW_STOP_NAV_BLOCKED, SE_FAILURE);
                return;
            }
        }
    }

    if (sAW.stage >= AW_NUM_OBJECTIVES)
    {
        // Nothing left in the table we know how to do. Stopping is correct;
        // inventing an objective is not.
        StopWith(AW_STOP_TABLE_END, SE_FAILURE);
        return;
    }

    obj = &gAwObjectives[sAW.stage];

    if (obj->action == AW_HAND_BACK)
    {
        // A section the agent knows it cannot do -- an ice puzzle, a password.
        // Announce and stop rather than flail against it.
        StopWith(AW_STOP_HAND_BACK, SE_FAILURE);
        return;
    }

    // Completion is detected from the save file, never assumed from having
    // performed the action: talking to an NPC does not mean the flag flipped.
    if (AW_EvalCond(&obj->done))
    {
        RefreshObjective();
        return;
    }

    // Eleven objectives -- including the whole Elite Four -- have no completion
    // test, because the game never sets a flag for them. A condition that can
    // never be proven true would pin the agent on that objective forever, so
    // when we have interacted and a script actually ran, advance on that
    // evidence instead. The cursor is session-local: AW_DetectStage() still
    // resumes from proven progress on the next hand-off, so an optimistic step
    // can never be mistaken for a durable one.
    // The variable moved while we were working on this objective and a script
    // of ours ran: the step landed even if the inferred threshold says it did
    // not. Trust the observed change over the guessed number.
    if (DoneCondIsVar(obj) && sAW.doneVarValid && sAW.sawScript
     && VarGet(obj->done.id) != sAW.doneVarBase)
    {
        sAW.sawScript = FALSE;
        RefreshObjective();
        return;
    }

    if (obj->done.kind == AW_COND_NONE && sAW.sawScript)
    {
        sAW.sawScript = FALSE;
        if (sAW.stage + 1 < AW_NUM_OBJECTIVES)
        {
            sAW.stage++;
            sAW.navGoalSet = FALSE;
            sAW.noProgress = 0;
            SnapshotDoneVar(&gAwObjectives[sAW.stage]);
            AW_Nav_Reset();
        }
        else
        {
            FinishRun(SE_PC_OFF);   // table exhausted: stop cleanly, not in error
        }
        return;
    }

    PlayerGetDestCoords(&x, &y);
    goalX = obj->x + MAP_OFFSET;
    goalY = obj->y + MAP_OFFSET;

    // Progress watchdog. An agent grinding against a wall forever is the worst
    // outcome, so an objective that stops producing movement is abandoned. A
    // guessed objective gets a shorter leash than a derived one, because the
    // table itself tells us how much to trust the target.
    if (x == sAW.lastX && y == sAW.lastY)
    {
        u16 limit = (obj->confidence == AW_DERIVED) ? 600 : 240;

        if (++sAW.noProgress > limit)
        {
            StopWith(AW_STOP_NO_PROGRESS, SE_FAILURE);
            return;
        }
    }
    else
    {
        sAW.noProgress = 0;
        sAW.lastX = x;
        sAW.lastY = y;
    }

    // Route toward the objective. The navigator returns a direction to press
    // rather than moving the player, so the press goes through the same code a
    // human's would.
    // When the objective names an NPC, interact with it where it actually is.
    // Standing ON an NPC is impossible, so the goal is the tile beneath it --
    // the one a player walks to in order to talk upward. Falling back to the
    // objective's own tile keeps coord triggers and item balls working.
    if (LiveTargetCoords(obj, &tx, &ty))
    {
        s16 dx = tx - x, dy = ty - y;

        if ((dx == 0 && (dy == 1 || dy == -1)) || (dy == 0 && (dx == 1 || dx == -1)))
        {
            // Already beside it. Press toward it to turn -- a step into an
            // occupied tile turns the player without moving -- then interact.
            sAW.mode = AW_INTERACT;
            if (sAW.faceFrames < 6)
            {
                sAW.faceFrames++;
                sAW.synthKeys = KeyToward(dx, dy);
                sAW.cooldown = 2;
            }
            else
            {
                sAW.synthKeys = A_BUTTON;
                sAW.cooldown = 12;
            }
            return;
        }
        sAW.faceFrames = 0;
        goalX = tx;
        goalY = ty + 1;         // stand below and face up
    }

    // Re-target whenever the goal moves, not just the first time. The goal is
    // derived from the NPC's LIVE position, so it changes when the NPC walks --
    // and a one-shot `if (!navGoalSet)` would compute the right destination and
    // then never tell the navigator about it. That exact mistake left the agent
    // routing to the NPC's long-abandoned starting tile.
    // A warp changes the map under the navigator's feet. The goal is unchanged,
    // so a goal-equality check alone keeps the stale plan and the next search
    // runs against the map we just left -- which presents as an immediate
    // BLOCKED the moment the agent steps through a door.
    if (gSaveBlock1Ptr->location.mapGroup != sAW.lastMapGroup
     || gSaveBlock1Ptr->location.mapNum != sAW.lastMapNum)
    {
        sAW.lastMapGroup = gSaveBlock1Ptr->location.mapGroup;
        sAW.lastMapNum = gSaveBlock1Ptr->location.mapNum;
        sAW.navGoalSet = FALSE;
    }

    if (!sAW.navGoalSet || goalX != sAW.curGoalX || goalY != sAW.curGoalY)
    {
        // Recorded so a wrong destination is readable from outside instead of
        // being inferred from navigator internals whose offsets keep moving.
        sAW.dbgMapId = obj->mapId;
        sAW.dbgGroup = MAP_GROUP(obj->mapId);
        sAW.dbgNum = MAP_NUM(obj->mapId);
        if (!AW_Nav_SetGoal(MAP_GROUP(obj->mapId), MAP_NUM(obj->mapId),
                            goalX - MAP_OFFSET, goalY - MAP_OFFSET))
        {
            StopWith(AW_STOP_NO_ROUTE, SE_FAILURE);  // no route exists at all
            return;
        }
        sAW.navGoalSet = TRUE;
        sAW.curGoalX = goalX;
        sAW.curGoalY = goalY;
    }

    sAW.lastNavRet = AW_Nav_Tick();
    if (sAW.lastNavRet != AW_NAV_BLOCKED && sAW.lastNavRet != AW_NAV_NO_ROUTE)
        sAW.navFails = 0;
    switch (sAW.lastNavRet)
    {
    case AW_NAV_WAIT:
        sAW.mode = AW_TRAVEL;
        return;                         // still searching; inject nothing
    case AW_NAV_UP:    sAW.mode = AW_TRAVEL; sAW.synthKeys = DPAD_UP;    return;
    case AW_NAV_DOWN:  sAW.mode = AW_TRAVEL; sAW.synthKeys = DPAD_DOWN;  return;
    case AW_NAV_LEFT:  sAW.mode = AW_TRAVEL; sAW.synthKeys = DPAD_LEFT;  return;
    case AW_NAV_RIGHT: sAW.mode = AW_TRAVEL; sAW.synthKeys = DPAD_RIGHT; return;

    case AW_NAV_ARRIVED:
        sAW.mode = AW_INTERACT;
        switch (obj->action)
        {
        case AW_TALK_TO:
        case AW_TAKE_ITEM:
            sAW.synthKeys = A_BUTTON;   // facing was set by the final step
            sAW.cooldown = 10;
            return;
        case AW_FIGHT_TRAINER:
            // Arriving in the trainer's line of sight is enough; the trainer
            // starts the battle. If it has not fired, nudge with A for the
            // gym-leader case where you must talk first.
            sAW.synthKeys = A_BUTTON;
            sAW.cooldown = 16;
            return;
        case AW_WALK_TO:
        default:
            RefreshObjective();         // standing here was the whole task
            return;
        }

    case AW_NAV_BLOCKED:
    case AW_NAV_NO_ROUTE:
    default:
        // A single BLOCKED is not proof of a dead end. The frames right after a
        // warp are the worst moment to ask: the new map's object events are
        // still being populated and the search can transiently find nothing.
        // Treating that as fatal is why the agent died the instant it stepped
        // out of its own front door.
        //
        // So re-plan and try again, and only give up once the verdict has been
        // stable for a while. A genuine dead end stays blocked and still halts
        // within a second; a transient one costs a few frames.
        sAW.navBlock = AW_Nav_BlockReason();
        sAW.dbgHere = AW_Nav_DebugHere();
        sAW.dbgWhich = AW_Nav_DebugWhich();
        sAW.dbgDistGoal = AW_Nav_DebugDistGoal();
        sAW.dbgDistHere = AW_Nav_DebugDistHere();
        sAW.dbgGoalIdx = AW_Nav_DebugGoalIdx();
        sAW.dbgQ = AW_Nav_DebugQ();
        sAW.dbgTgtCol = AW_Nav_DebugTgtCollision();
        sAW.dbgPlrCol = AW_Nav_DebugPlayerCollision();
        sAW.navGoalSet = FALSE;          // force a fresh plan next frame
        if (++sAW.navFails < 60)
            return;
        StopWith(AW_STOP_NAV_BLOCKED, SE_FAILURE);
        return;
    }
}

// Harness determinism hook. The emulator is deterministic for a fixed build and
// state, but a CODE change shifts how many Random() calls happen before any
// given frame, so wild encounters differ between builds and a navigator A/B
// cannot attribute a route change to the edit rather than to the dice. A test
// poking this byte gets an encounter-free world and a comparable route.
//
// Zero gameplay risk: it defaults to 0, nothing in the ROM ever writes it, and
// it drives the vanilla DisableWildEncounters() the game already uses for its
// own scripted sequences.
EWRAM_DATA u8 gAWSuppressEncounters = 0;

void AutoWorker_Tick(void)
{
    sAW.synthKeys = 0;

    // Unconditional: a test may want a quiet world with the agent switched off.
    {
        static u8 applied = 0;
        if (gAWSuppressEncounters != applied)
        {
            applied = gAWSuppressEncounters;
            DisableWildEncounters(applied ? TRUE : FALSE);
        }
    }

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
        sAW.stopReason = AW_STOP_HUMAN_B;
        AutoWorker_Disable();
        return;
    }

    if (ShouldRefuseToRun())
    {
        AutoWorker_Disable();
        return;
    }

    // Backstop watchdog, evaluated before any per-mode branch can touch it.
    // A battle legitimately freezes the overworld position, so it does not count.
    if (!gMain.inBattle)
    {
        s16 wx, wy;

        PlayerGetDestCoords(&wx, &wy);
        // Deliberately NOT reset by a changed tile. An agent bouncing between
        // two tiles changes tile every single step, so a position-based reset
        // never fires -- observed at the Lab exit, oscillating (13,14)<->(13,15)
        // for 145,000 frames. Only genuine progress counts: a new objective or
        // a new map. Crossing the largest map in the game costs well under the
        // budget, so this cannot punish honest travel.
        if (sAW.stage != sAW.wdStage
         || gSaveBlock1Ptr->location.mapGroup != sAW.wdMapGroup
         || gSaveBlock1Ptr->location.mapNum != sAW.wdMapNum)
        {
            sAW.wdX = wx;
            sAW.wdY = wy;
            sAW.wdStage = sAW.stage;
            sAW.wdMapGroup = gSaveBlock1Ptr->location.mapGroup;
            sAW.wdMapNum = gSaveBlock1Ptr->location.mapNum;
            sAW.watchdog = 0;
        }
        else if (++sAW.watchdog > AW_BACKSTOP_FRAMES)
        {
            sAW.stopReason = AW_STOP_BACKSTOP;
            PlaySE(SE_FAILURE);
            AutoWorker_Disable();
            return;
        }
    }

    if (gMain.inBattle)
    {
        // The overworld backstop deliberately ignores battles, so a battle that
        // never ends is an unbounded hang -- observed: a battle began at frame
        // 11,931 and inBattle stayed true for the remaining 190,000 frames.
        // No legitimate battle runs for five minutes, so bound it separately.
        if (++sAW.battleFrames > AW_BACKSTOP_FRAMES)
        {
            sAW.stopReason = AW_STOP_BATTLE_HANG;
            PlaySE(SE_FAILURE);
            AutoWorker_Disable();
            return;
        }

        // Tap A on a timer. This advances the "X used Y!" messages, the
        // faint and level-up text and the end-of-battle summary -- everything
        // between turns that is not a menu.
        //
        // It deliberately does NOT choose moves. While an action or move menu
        // is up, the driver in battle_controller_player.c ASSIGNS gMain.newKeys
        // and so overrides this, because A on a menu selects whatever happens
        // to be highlighted. The two cannot fight: text advance here, menu
        // choice there.
        if (sAW.cooldown)
        {
            sAW.cooldown--;
        }
        else
        {
            sAW.synthKeys = A_BUTTON;
            sAW.cooldown = 10;
            gMain.newKeys |= sAW.synthKeys;
            gMain.heldKeys |= sAW.synthKeys;
            gMain.newAndRepeatedKeys |= sAW.synthKeys;
        }
        return;
    }

    sAW.battleFrames = 0;
    sAW.battleLatch = sAW.enabled;
    ChooseOverworldAction();

    // Observable while it runs, not only once it has given up.
    sAW.dbgEscape   = AW_Nav_DebugEscape();
    sAW.dbgNavState = AW_Nav_DebugState();
    sAW.dbgBadEdges = AW_Nav_DebugBadEdges();
    sAW.dbgNavTgt   = AW_Nav_DebugTgt();
    sAW.dbgNavWin   = AW_Nav_DebugWin();

    // Assert into the cooked fields only. heldKeysRaw/newKeysRaw are left alone
    // so the A+B+START+SELECT soft reset always remains available to the human.
    gMain.newKeys |= sAW.synthKeys;
    gMain.heldKeys |= sAW.synthKeys;
    gMain.newAndRepeatedKeys |= sAW.synthKeys;
}
