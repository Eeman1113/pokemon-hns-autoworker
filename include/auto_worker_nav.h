#ifndef GUARD_AUTO_WORKER_NAV_H
#define GUARD_AUTO_WORKER_NAV_H

// Auto Worker — navigation.
//
// Two levels, both built on the same idea: never store a path, store a field.
//
//   * Inter-map: a backward BFS over the reverse warp graph writes dist[] in
//     hops-to-destination. At any map the agent steps to the neighbour with the
//     lower distance.
//   * Intra-map: a backward BFS from the target tile over a window of the
//     current map writes, for every tile, the direction to press when standing
//     on it.
//
// A field is self-healing in a way a path is not. If a script teleports the
// player, a battle ends somewhere unexpected, or the agent falls down a ledge,
// the next tick simply reads the field wherever it now is and keeps going. A
// stored path would have to notice the displacement and replan, and the code
// that notices is exactly the code that tends to be missing when it matters.
//
// The router returns a DIRECTION TO PRESS and never moves the player itself.
// Driving the player through ScriptMovement_* would bypass the normal field
// input path, which means tall grass, ledges, warps and trigger tiles would
// silently not fire — the agent would look like it was working while quietly
// playing a different game.

enum AwNavResult
{
    AW_NAV_WAIT,        // still searching, or the field is not ours — press nothing
    AW_NAV_UP,
    AW_NAV_DOWN,
    AW_NAV_LEFT,
    AW_NAV_RIGHT,
    AW_NAV_ARRIVED,     // standing on the goal tile of the goal map
    AW_NAV_BLOCKED,     // cannot proceed; AW_Nav_BlockReason() says why
    AW_NAV_NO_ROUTE,    // no goal set, or the goal map is unreachable at all
};

enum AwNavBlock
{
    AW_BLOCK_NONE,
    AW_BLOCK_NEEDS_SURF,
    AW_BLOCK_NEEDS_CUT,
    AW_BLOCK_NEEDS_STRENGTH,
    AW_BLOCK_NEEDS_WATERFALL,
    AW_BLOCK_NEEDS_WHIRLPOOL,
    AW_BLOCK_WINDOW,        // goal lies outside the search window — recentre and retry
    AW_BLOCK_UNREACHABLE,   // genuinely walled off on this map
    AW_BLOCK_NO_EDGE,       // on-route map has no usable outgoing edge
};

// Forget everything. Call on hand-off, and whenever the objective changes.
void AW_Nav_Reset(void);

// Ask to travel to (x, y) on the given map, in map-local coordinates — the same
// numbers that appear in map.json and in the objective table, not the +7
// grid-offset coordinates the engine uses internally.
// Returns FALSE if the destination map is not in the graph or not reachable.
bool32 AW_Nav_SetGoal(u8 mapGroup, u8 mapNum, s16 x, s16 y);

// Call once per frame while Auto Worker owns the field. Returns the button to
// press this frame, or a terminal status.
u8 AW_Nav_Tick(void);

u8 AW_Nav_BlockReason(void);
u16 AW_Nav_DebugHere(void);
u8 AW_Nav_DebugDistGoal(void);
u8 AW_Nav_DebugDistHere(void);
u16 AW_Nav_DebugGoalIdx(void);
u16 AW_Nav_DebugQ(void);
u16 AW_Nav_DebugTgtCollision(void);
u16 AW_Nav_DebugPlayerCollision(void);
u8 AW_Nav_DebugWhich(void);
u16 AW_Nav_DebugEscape(void);
u8 AW_Nav_DebugState(void);
u8 AW_Nav_DebugBadEdges(void);
u16 AW_Nav_DebugTgt(void);
u16 AW_Nav_DebugWin(void);

// Whether a goal is currently set. Lets the caller distinguish "no route yet"
// from "route finished".
bool32 AW_Nav_HasGoal(void);

#endif // GUARD_AUTO_WORKER_NAV_H
