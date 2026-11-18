// AW_NAV_HOST_TEST lets test/aw_nav_test.c compile THIS FILE on the development
// machine with the engine replaced by a synthetic map, so the pathfinder that
// ships is the pathfinder that was tested rather than a reimplementation of it
// that drifts.
#ifdef AW_NAV_HOST_TEST
#include "aw_nav_hostdefs.h"
#else
#include "global.h"
#include "auto_worker_nav.h"
#include "world_graph.h"
#include "aw_centres.h"
#include "fieldmap.h"
#include "metatile_behavior.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "overworld.h"
#include "script.h"
#include "constants/global.h"
#include "constants/event_objects.h"
#endif

// ---------------------------------------------------------------------------
// Budget
//
// IWRAM is at 78% with about 6 KB free across the WHOLE ROM, so nothing here
// may live there -- every buffer below is EWRAM_DATA. EWRAM has roughly 13 KB
// free and this file claims about 6.3 KB of it.
//
// The window is 32x32 rather than the whole map because the largest map is
// 170x160 = 27,200 tiles, and one byte per tile is 27 KB -- twice the entire
// EWRAM headroom. 32x32 reaches 16 tiles in every direction, comfortably more
// than a screen, which covers the overwhelming majority of warp approaches;
// longer crossings are made as a chain of windowed hops (see SeedGoal).
// ---------------------------------------------------------------------------

/* 48, not 64. Widening to 64 makes Cherrygrove's north edge reachable in
   theory -- BFS over the real blockdata: 32-window 68 tiles and 0 north-edge
   cells, 48-window 191 and 0, 64-window 308 and 3 -- but measured end to end
   it made the agent WORSE, horizon 11 -> 9 with encounters suppressed, and
   raising the frontier cap changed nothing so the cap was not the cause. A
   64-tile window is 4x the cells of 32 and the frame-sliced search spends
   proportionally longer before it can act. Reverted on the measurement.
   Cherrygrove therefore still needs a different fix: chained window hops that
   can move AWAY from the goal, not a wider window. */
#define NAV_WIN             48
#define NAV_CELLS           (NAV_WIN * NAV_WIN)
/* 3/4, not 1/2. At a 64-tile window the search can now cover a whole mid-size
   map, and large open maps exceed 2048 reachable cells -- the cap then dropped
   frontier cells and the search reported unreachable, costing two objectives
   (horizon 11 -> 9 with encounters suppressed, which is a clean measurement).
   Measured in-window reachability: Route 29 556, Cherrygrove 308, whole-map
   Cherrygrove 706; 3072 clears those with margin at 6144 bytes. */
#define NAV_QCAP            ((NAV_CELLS * 3) / 4)
#define NAV_NODES_PER_TICK  110                      // ~1 ms at ~150 cyc/node
#define NAV_DIR_GOAL        0x0F                     // dirField marker for a goal cell
#define NAV_NO_MAP          0xFFFF
#define NAV_UNREACHABLE     255

// How long the agent may stand still while it believes it is walking before we
// assume the field is lying to us and re-plan. At 60 fps a normal step takes
// 16 frames, so 90 is generous without being a hang.
#define NAV_STALL_REPLAN    90
#define NAV_REPLAN_LIMIT    3
// Long enough for a wandering NPC to take a step (they move on a multi-second
// cycle), short enough that a genuinely immovable one is not waited on forever.
#define NAV_NPC_WAIT_LIMIT  240

// At 110 nodes/frame a full 32x32 window drains in ~10 frames, so 300 frames
// is roughly thirty times the worst legitimate search. Anything beyond it is a
// defect, not deep thought.
#define NAV_MAX_WAIT        300
#define NAV_REENTER_LIMIT   3
/* Escape hops. When the window edge FACING the goal is unreachable, the way
   out may run away from the goal -- Cherrygrove's north connection needs a
   westward detour, and BFS over the real blockdata finds 0 reachable
   north-edge cells at both a 32 and a 48 window. Seeding every edge lets the
   agent walk to whichever boundary it can reach, recentre, and try again.
   Bounded, because an unbounded version is just oscillation with extra steps. */
#define NAV_ESCAPE_LIMIT    4

enum
{
    NAV_STATE_IDLE,
    NAV_STATE_SEARCHING,
    NAV_STATE_FOLLOWING,
    NAV_STATE_DONE,
    NAV_STATE_FAILED,
};

// Small on purpose: this exists to route around a handful of physically
// impossible shortest-paths, not to memoise the graph.
#define NAV_BAD_EDGES   8
#define NAV_NO_EDGE     0xFFFF

struct AwNavState
{
    // Persistent: hops from each map to the destination map. Written once per
    // travel request by a backward BFS; read every time the agent lands on a
    // new map. 255 means unreachable.
    u8  dist[WORLD_MAP_COUNT];

    // Persistent for the duration of one intra-map search: for every tile in
    // the window, the direction to press while standing on it.
    // One NIBBLE per tile, not one byte. Values are 0..15 (four directions
    // plus NAV_DIR_GOAL), so half the storage was always padding. Packing is
    // what makes a 64-tile window affordable: unpacked it cost 4096 bytes and
    // left EWRAM at 99.96% with 108 bytes free, which is not a budget anyone
    // can build on.
    u8  dirField[NAV_CELLS / 2];

    // The two searches never overlap -- the inter-map BFS runs to completion
    // inside AW_Nav_SetGoal before any intra-map search starts -- so they share
    // one queue and we pay for the larger of the two rather than both.
    union
    {
        u16 inter[WORLD_MAP_COUNT];
        // Not NAV_CELLS. BFS enqueues each reachable cell at most once, and
        // measured in-window reachability is far below this cap: Route 29's
        // worst window held 556 cells, Cherrygrove's 308, the largest whole
        // map 706. Overflow degrades to a failed search -- the same BLOCKED
        // path a genuinely unreachable goal takes -- rather than corrupting
        // the frontier.
        u16 intra[NAV_QCAP];
    } q;

    u16 qHead, qTail;

    s16 winX, winY;             // window origin, grid coords
    s16 goalX, goalY;           // final tile, grid coords, on the goal map
    s16 tgtX, tgtY;             // current intra-map target, grid coords
    u16 goalMapIdx;
    u8  goalGroup, goalNum;

    /* Escape state. Reset ONLY on a new goal or a new map -- never on a
       changed tile. Two earlier watchdogs in this project reset on movement,
       which an oscillating agent satisfies on every single step, so they could
       never fire against the thing they existed to catch. */
    u8  escapeMode;             // seed every edge, not just the goal-facing one
    u8  escapeTries;
    u8  escapeFromDir;          // edge we last left by; excluded next time
    // Lifetime counters, never reset. escapeTries is per-goal and so reads 0
    // the moment a goal changes, which is exactly when you want to know it
    // fired. The previous cycle built this whole mechanism and could not tell
    // whether it ever ran.
    u8  escapeFired;            // times escapeMode was entered
    u8  escapeArrived;          // times an escape hop reached a boundary

    /* Edges the router chose and the map then proved unwalkable.
       Route 31 reaches Violet City two ways: a left map connection (1 hop) and
       a gate warp (2 hops). Fewest-hops picks the connection -- and the entire
       west column of Route 31 is walled, so the agent ground against it
       forever while standing one tile below the gate door. Hop count is not
       reachability, and the router cannot know the difference until the
       intra-map search has actually failed. So record the failure and let the
       next choice route around it. */
    u16 badEdge[NAV_BAD_EDGES];
    u8  badEdges;
    u16 chosenEdge;             // global edge index backing the current hop
    /* `haveGoal` is cleared by AW_Nav_Reset, which the agent calls on every
       re-enable -- and a run that re-enables after each stop (as fullrun.py
       does by design, because a player would try again) therefore wiped the
       same-goal test and with it the blacklist. This marker records only that
       goalGroup/goalNum/goalX/goalY are meaningful, and Reset leaves it alone.
       Without it the headline measurement runs with blacklisting disabled. */
    bool8 goalEverSet;
    /* The edge most recently committed to -- i.e. the one that delivered the
       agent onto the map it is standing on. Blacklisting only the OUTBOUND
       edge is not enough when a map is internally partitioned: Sprout Tower 2F
       has three staircases from 1F landing in three disconnected regions, and
       the world graph models the whole floor as one node, so all three look
       identical to the router. Blaming the entrance is what lets it pick a
       different staircase instead of re-entering the same dead pocket. */
    u16 inboundEdge;

    u8  state;
    u8  blockReason;
    u8  sawReason;              // most actionable rejection seen this search
    u8  replans;
    bool8 tgtIsEdgeSeed;        // target is "the window edge", not a real tile

    s16 lastX, lastY;
    u8  lastGroup, lastNum;
    u16 stall;
    bool8 haveGoal;

    // Standing on a warp tile does not trigger it -- warps fire when the
    // player STEPS ON to them. An agent that spawns on a staircase must step
    // off and back on, or it will stand there forever looking like a hang.
    u8  reenterDir;
    u8  reenterTries;
    u8  permissive;     // this window search may plan through people
    u8  npcWait;        // frames spent waiting for a mover to clear

    // Hard bound on thinking. AW_Nav_Tick must never be able to return WAIT
    // forever: a navigator that silently thinks is indistinguishable from a
    // hang, and the caller cannot recover from a status it never receives.
    u16 waitTicks;
    s16 failedSearchX, failedSearchY;
    // Captured at the moment of an UNREACHABLE verdict. Without it the only
    // way to tell "map not in graph" from "no path" is to guess.
    u16 dbgHere;
    u8  dbgDist;
    u8  dbgWhich;   // 1 = map absent from graph, 2 = no path after re-solve
    // Doors are impassable tiles with a warp attached: the player stands
    // beside one and presses into it, never occupying it. When the chosen
    // warp is such a tile we aim at the approach tile instead and remember
    // which way to press on arrival.
    u8  doorDir;    // DIR_* to press once standing on tgt, or DIR_NONE
    // Map connections have no tile: the target is placed deliberately OUTSIDE
    // the map so the window seeding walks us to the border. But when that
    // phantom tile happens to fall inside the window, SeedGoal seeds it as a
    // real cell -- an out-of-map cell nothing can reach -- and the search dies
    // with one seed and no expansion, exactly like aiming at a door.
    u8  tgtConnDir; // DIR_* of the connection being crossed, or DIR_NONE
};

static EWRAM_DATA struct AwNavState sNav = {0};

#ifndef AW_NAV_HOST_TEST
// Scratch object for collision probing. GetCollisionAtCoords WRITES
// objectEvent->directionOverwrite (src/event_object_movement.c:6898), so
// probing with the real player object would corrupt the player's movement
// state mid-step. It also returns COLLISION_IMPASSABLE for any camera-tracked
// object the camera cannot follow (src/event_object_movement.c:6847-6848),
// which for the player is every tile near a map edge -- a BFS probing with the
// player would be told the edges of the world are walls.
static EWRAM_DATA struct ObjectEvent sProbe = {0};
#endif // !AW_NAV_HOST_TEST

// ---------------------------------------------------------------------------

static const s8 sDx[] = { [DIR_SOUTH] = 0, [DIR_NORTH] = 0, [DIR_WEST] = -1, [DIR_EAST] = 1 };
static const s8 sDy[] = { [DIR_SOUTH] = 1, [DIR_NORTH] = -1, [DIR_WEST] = 0, [DIR_EAST] = 0 };

static u8 DirToResult(u8 dir)
{
    switch (dir)
    {
    case DIR_NORTH: return AW_NAV_UP;
    case DIR_SOUTH: return AW_NAV_DOWN;
    case DIR_WEST:  return AW_NAV_LEFT;
    case DIR_EAST:  return AW_NAV_RIGHT;
    }
    return AW_NAV_WAIT;
}

static bool32 InWindow(s16 x, s16 y)
{
    return x >= sNav.winX && y >= sNav.winY
        && x < sNav.winX + NAV_WIN && y < sNav.winY + NAV_WIN;
}

static u16 CellOf(s16 x, s16 y)
{
    return (u16)((y - sNav.winY) * NAV_WIN + (x - sNav.winX));
}

// ---------------------------------------------------------------------------
// Stepping on an unintended warp derails the whole route, so every warp tile
// that is not the one we are aiming at is treated as a wall.
//
// This also disposes of a trap in the shipped hack without naming it: New Bark
// Town's warps 4 and 7 lead to a developer test map with no way back, and warp
// 5 is a debug teleport. A router that treats warp tiles as ordinary floor will
// walk out of the player's home town into a test map on its second hop. Making
// every off-route warp an obstacle handles those three, and every other stray
// warp in the game, without a hand-maintained blacklist.
// ---------------------------------------------------------------------------
#ifndef AW_NAV_HOST_TEST
static bool32 IsOffRouteWarp(s16 gx, s16 gy)
{
    const struct WarpEvent *w;
    u8 count, i;
    s16 lx = gx - MAP_OFFSET, ly = gy - MAP_OFFSET;

    if (gMapHeader.events == NULL)
        return FALSE;

    // The tile we are deliberately aiming at is allowed to be a warp.
    if (gx == sNav.tgtX && gy == sNav.tgtY)
        return FALSE;

    w = gMapHeader.events->warps;
    count = gMapHeader.events->warpCount;
    for (i = 0; i < count; i++, w++)
    {
        if (w->x == lx && w->y == ly)
            return TRUE;
    }
    return FALSE;
}

static void NoteRejection(s16 gx, s16 gy)
{
    u8 b = MapGridGetMetatileBehaviorAt(gx, gy);

    // Only the reasons the objective layer can actually act on are worth
    // reporting. "I need Surf" is actionable; "I am stuck" is not, and dressing
    // the second up as the first would send the caller off to use a field move
    // that changes nothing.
    if (MetatileBehavior_IsWaterfall(b))
        sNav.sawReason = AW_BLOCK_NEEDS_WATERFALL;
    else if (MetatileBehavior_IsSurfableWaterOrUnderwater(b) && sNav.sawReason != AW_BLOCK_NEEDS_WATERFALL)
        sNav.sawReason = AW_BLOCK_NEEDS_SURF;
}

// Can the player step from (fx,fy) to (tx,ty) by pressing `dir`?
//
// Calls the game's own collision rather than reimplementing it. This fork adds
// eight direction-dependent sideways-stairs cases ahead of the vanilla checks
// (src/event_object_movement.c:6900-6916); any reimplementation gets those
// wrong and the agent walks into a staircase forever.
static enum Collision ProbeCollision(s16 fx, s16 fy, s16 tx, s16 ty, u8 dir)
{
    sProbe.currentCoords.x = fx;
    sProbe.currentCoords.y = fy;
    sProbe.previousCoords.x = fx;
    sProbe.previousCoords.y = fy;
    sProbe.currentElevation = MapGridGetElevationAt(fx, fy);
    sProbe.currentMetatileBehavior = MapGridGetMetatileBehaviorAt(fx, fy);
    return GetCollisionAtCoords(&sProbe, tx, ty, dir);
}

// A tree is not a person. Object-event collisions are treated as *temporary*
// obstacles -- an NPC wanders off, so planning through one as a last resort is
// reasonable and the follower waits rather than bumping. Cut trees, smashable
// rocks and Strength boulders are object events too, but they never move: a
// route through one is a route the agent can never walk.
//
// Observed cost of conflating them: on Route 29 a cuttable tree sits at local
// (31,23), directly across the corridor the agent was crossing. It planned
// through the tree, pressed east into it until the no-progress watchdog fired,
// and repeated -- 274 stops in a 300,000-frame run -- while a completely open
// corridor existed four tiles south at y=26.
static bool32 IsPermanentBlocker(s16 tx, s16 ty)
{
    u32 i;

    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
    {
        if (!gObjectEvents[i].active)
            continue;
        if (gObjectEvents[i].currentCoords.x != tx
         || gObjectEvents[i].currentCoords.y != ty)
            continue;

        switch (gObjectEvents[i].graphicsId)
        {
        case OBJ_EVENT_GFX_CUTTABLE_TREE:
        case OBJ_EVENT_GFX_CUTTABLE_TREE_HNS:
        case OBJ_EVENT_GFX_CUTTABLE_TREE_FRLG:
        case OBJ_EVENT_GFX_BREAKABLE_ROCK:
        case OBJ_EVENT_GFX_BREAKABLE_ROCK_HNS:
        case OBJ_EVENT_GFX_BREAKABLE_ROCK_FRLG:
        case OBJ_EVENT_GFX_PUSHABLE_BOULDER:
        case OBJ_EVENT_GFX_PUSHABLE_BOULDER_HNS:
        case OBJ_EVENT_GFX_PUSHABLE_BOULDER_FRLG:
            return TRUE;
        default:
            return FALSE;
        }
    }
    return FALSE;
}

static bool32 CanStep(s16 fx, s16 fy, s16 tx, s16 ty, u8 dir)
{
    enum Collision col;

    if (IsOffRouteWarp(tx, ty))
        return FALSE;

    // IsMetatileDirectionallyImpassable reads currentMetatileBehavior rather
    // than deriving it from currentCoords, so the probe must set it.
    col = ProbeCollision(fx, fy, tx, ty, dir);

    // OBJECT_EVENT is not geometry -- it is somebody standing there, and people
    // move. Door detection already treats it as "not a wall"; CanStep demanding
    // COLLISION_NONE made the two disagree, so a warp tile with an NPC on it
    // rejected all four approaches and the search died with one seed and no
    // expansion. Observed as 0x4444 on the exit of Mr. Pokemon's house.
    //
    // Planning through a mover is right: if they wander off we walk through, and
    // if they genuinely never move the stall watchdog and replan handle it
    // honestly rather than the route being declared impossible up front.
    if (col == COLLISION_NONE)
        return TRUE;

    // An NPC is not geometry. People move, so a tile they occupy is blocked
    // *now* rather than impassable, and the two deserve different treatment.
    //
    // Strict by default: planning through a mover when a clear route exists is
    // how the first attempt at this failed -- bumping an NPC does not move the
    // player, so the route "worked" on paper and stalled instantly in fact.
    //
    // Permissive only as a second pass, when the strict search could not reach
    // the goal at all (NavPermissiveRetry below). That is the Mr. Pokemon's
    // house case: an NPC parked on the exit warp made all four approaches
    // report COLLISION_OBJECT_EVENT (tgt collision 0x4444), the search died
    // with one seed and no expansion, and the only way out looked impossible.
    // Treating it as passable-but-last-resort keeps the warp usable; the
    // follower then WAITS on the blocked step instead of bumping it, which is
    // what the second attempt got wrong.
    if (col == COLLISION_OBJECT_EVENT && sNav.permissive
     && !IsPermanentBlocker(tx, ty))
        return TRUE;

    NoteRejection(tx, ty);
    return FALSE;
}
#endif // !AW_NAV_HOST_TEST

// ---------------------------------------------------------------------------
// Intra-map search: backward BFS from the target over the window.
//
// Backward matters. Collision is directional -- ledges are one-way and the
// sideways stairs are direction-dependent -- so the question "can I reach the
// goal from here" must be asked as "can I step FROM this tile TOWARDS the
// goal", which is exactly what expanding predecessors does.
// ---------------------------------------------------------------------------

static u8 DirGet(u16 c)
{
    u8 b = sNav.dirField[c >> 1];

    return (c & 1) ? (b >> 4) : (b & 0x0F);
}

static void DirSet(u16 c, u8 d)
{
    u8 *b = &sNav.dirField[c >> 1];

    if (c & 1)
        *b = (*b & 0x0F) | (u8)(d << 4);
    else
        *b = (*b & 0xF0) | (d & 0x0F);
}

static void PushCell(s16 x, s16 y, u8 dir)
{
    u16 c = CellOf(x, y);

    if (DirGet(c) != 0)
        return;
    // Drop the cell rather than the invariant if the frontier is full. The
    // search then reports unreachable, which is already a handled outcome.
    if (sNav.qTail >= NAV_QCAP)
        return;
    DirSet(c, dir);
    sNav.q.intra[sNav.qTail++] = c;
}

// Seed the search. If the goal is inside the window we aim at it directly;
// otherwise we aim at the window edge facing it, walk there, recentre and
// search again. Long crossings therefore happen as a chain of windowed hops
// rather than failing outright, which is what a fixed window would otherwise
// force on a 170-tile-wide map.
static void SeedGoal(void)
{
    s16 i;

    sNav.qHead = sNav.qTail = 0;
    sNav.sawReason = AW_BLOCK_NONE;
    for (i = 0; i < (s16)(NAV_CELLS / 2); i++)
        sNav.dirField[i] = 0;

    // Escape seeding takes priority: the normal seeds are known-unreachable
    // from here, so re-laying them would reproduce the failure exactly.
    if (sNav.escapeMode)
    {
        sNav.tgtIsEdgeSeed = TRUE;
        for (i = 0; i < NAV_WIN; i++)
        {
            if (sNav.escapeFromDir != DIR_EAST)
                PushCell(sNav.winX + NAV_WIN - 1, sNav.winY + i, NAV_DIR_GOAL);
            if (sNav.escapeFromDir != DIR_WEST)
                PushCell(sNav.winX, sNav.winY + i, NAV_DIR_GOAL);
            if (sNav.escapeFromDir != DIR_SOUTH)
                PushCell(sNav.winX + i, sNav.winY + NAV_WIN - 1, NAV_DIR_GOAL);
            if (sNav.escapeFromDir != DIR_NORTH)
                PushCell(sNav.winX + i, sNav.winY, NAV_DIR_GOAL);
        }
        if (sNav.qTail != 0)
            return;
        sNav.escapeMode = FALSE;   // nothing reachable at all; fall through
    }

    if (sNav.tgtConnDir != DIR_NONE)
    {
        const struct MapLayout *lay = gMapHeader.mapLayout;
        s16 w = lay ? (s16)lay->width : 64;
        s16 h = lay ? (s16)lay->height : 64;

        // Any tile on that border will do, so seed the whole line. Aiming at a
        // single boundary tile on the player's own row fails whenever that one
        // tile happens to be a cliff, a fence or water.
        sNav.tgtIsEdgeSeed = FALSE;
        for (i = 0; i < NAV_WIN; i++)
        {
            switch (sNav.tgtConnDir)
            {
            case DIR_NORTH: if (InWindow(sNav.winX + i, MAP_OFFSET))
                                PushCell(sNav.winX + i, MAP_OFFSET, NAV_DIR_GOAL);
                            break;
            case DIR_SOUTH: if (InWindow(sNav.winX + i, h - 1 + MAP_OFFSET))
                                PushCell(sNav.winX + i, h - 1 + MAP_OFFSET, NAV_DIR_GOAL);
                            break;
            case DIR_WEST:  if (InWindow(MAP_OFFSET, sNav.winY + i))
                                PushCell(MAP_OFFSET, sNav.winY + i, NAV_DIR_GOAL);
                            break;
            default:        if (InWindow(w - 1 + MAP_OFFSET, sNav.winY + i))
                                PushCell(w - 1 + MAP_OFFSET, sNav.winY + i, NAV_DIR_GOAL);
                            break;
            }
        }
        if (sNav.qTail != 0)
            return;
        // The border is outside this window; fall through to edge seeding.
    }

    if (InWindow(sNav.tgtX, sNav.tgtY))
    {
        sNav.tgtIsEdgeSeed = FALSE;
        DirSet(CellOf(sNav.tgtX, sNav.tgtY), NAV_DIR_GOAL);
        sNav.q.intra[sNav.qTail++] = CellOf(sNav.tgtX, sNav.tgtY);
        return;
    }

    // Out of range: seed the edge(s) of the window that face the target.
    sNav.tgtIsEdgeSeed = TRUE;
    if (sNav.tgtX >= sNav.winX + NAV_WIN)
        for (i = 0; i < NAV_WIN; i++)
            PushCell(sNav.winX + NAV_WIN - 1, sNav.winY + i, NAV_DIR_GOAL);
    else if (sNav.tgtX < sNav.winX)
        for (i = 0; i < NAV_WIN; i++)
            PushCell(sNav.winX, sNav.winY + i, NAV_DIR_GOAL);

    if (sNav.tgtY >= sNav.winY + NAV_WIN)
        for (i = 0; i < NAV_WIN; i++)
            PushCell(sNav.winX + i, sNav.winY + NAV_WIN - 1, NAV_DIR_GOAL);
    else if (sNav.tgtY < sNav.winY)
        for (i = 0; i < NAV_WIN; i++)
            PushCell(sNav.winX + i, sNav.winY, NAV_DIR_GOAL);
}

static void StartIntraSearch(s16 px, s16 py)
{
    sNav.permissive = FALSE;
    sNav.winX = px - NAV_WIN / 2;
    sNav.winY = py - NAV_WIN / 2;
    SeedGoal();
    sNav.state = NAV_STATE_SEARCHING;
}

// Re-run the window search allowing people to be stepped through. Returns
// FALSE if this search was already permissive, so a caller cannot loop.
static bool32 NavPermissiveRetry(s16 px, s16 py)
{
    if (sNav.permissive)
        return FALSE;
    StartIntraSearch(px, py);
    sNav.permissive = TRUE;
    return TRUE;
}

// Returns TRUE when the search has finished (queue drained).
static bool32 StepIntraSearch(void)
{
    u16 budget = NAV_NODES_PER_TICK;

    while (budget-- && sNav.qHead < sNav.qTail)
    {
        u16 c = sNav.q.intra[sNav.qHead++];
        s16 tx = sNav.winX + (c % NAV_WIN);
        s16 ty = sNav.winY + (c / NAV_WIN);
        u8 dir;

        for (dir = DIR_SOUTH; dir <= DIR_EAST; dir++)
        {
            // Predecessor that reaches (tx,ty) by pressing `dir`.
            s16 nx = tx - sDx[dir];
            s16 ny = ty - sDy[dir];

            if (InWindow(nx, ny) && DirGet(CellOf(nx, ny)) == 0
             && CanStep(nx, ny, tx, ty, dir))
                PushCell(nx, ny, dir);

            // Ledge predecessor. Pressing `dir` at (lx,ly) enters the ledge
            // tile and lands two tiles away, so the predecessor is two back,
            // not one. This edge exists in one direction only: the reverse
            // never generates because the in-between tile is not a ledge
            // facing the other way. A BFS that expanded back across a ledge
            // would cheerfully plan a return route up a cliff.
            {
                s16 mx = tx - sDx[dir], my = ty - sDy[dir];
                s16 lx = tx - 2 * sDx[dir], ly = ty - 2 * sDy[dir];

                if (InWindow(lx, ly) && DirGet(CellOf(lx, ly)) == 0
                 && GetLedgeJumpDirection(mx, my, dir) != DIR_NONE)
                    PushCell(lx, ly, dir);
            }
        }
    }

    return sNav.qHead >= sNav.qTail;
}

// ---------------------------------------------------------------------------
// Inter-map search: backward BFS over the reverse warp graph.
//
// One shot, at SetGoal time: ~4,500 edge relaxations. Deliberately not run per
// tick. Afterwards the agent only ever reads dist[] at whatever map it is on,
// which is what makes the route survive being teleported by a script.
// ---------------------------------------------------------------------------
static bool32 BuildInterRoute(u16 destIdx)
{
    u16 head = 0, tail = 0, i;

    for (i = 0; i < WORLD_MAP_COUNT; i++)
        sNav.dist[i] = NAV_UNREACHABLE;

    sNav.dist[destIdx] = 0;
    sNav.q.inter[tail++] = destIdx;

    while (head < tail)
    {
        u16 m = sNav.q.inter[head++];
        u16 s = gWorldRevStart[m], e = gWorldRevStart[m + 1];
        u8 nd = sNav.dist[m] + 1;

        for (; s < e; s++)
        {
            u16 p = gWorldRev[s];

            if (sNav.dist[p] == NAV_UNREACHABLE)
            {
                sNav.dist[p] = nd;
                if (tail < WORLD_MAP_COUNT)
                    sNav.q.inter[tail++] = p;
            }
        }
    }

    // A map with no path back to the destination -- a developer sink like the
    // test map wired into New Bark Town -- simply never receives a finite
    // distance, so gradient descent can never choose to walk into one.
    return TRUE;
}

// ---------------------------------------------------------------------------
// Nearest Pokemon Centre, by forward BFS from the player's map.
//
// Forward, not backward: dist[] is a backward BFS from one KNOWN destination,
// which answers "how far am I from there". Here the destination is the unknown,
// so the search runs the other way. Thirty backward searches would answer the
// same question at thirty times the cost.
//
// Reuses sNav.q.inter and sNav.dist as scratch. Safe, and deliberate: the only
// caller routes to the Centre immediately afterwards, and AW_Nav_SetGoal
// rebuilds dist[] from scratch anyway. A private queue would cost ~3KB of a
// 32KB IWRAM budget for a search that runs on the rare frame the party drops
// below the heal threshold.

// A node is now a REGION of a map, not a whole map, so resolving "where am I"
// needs the player's tile as well as the map. Sprout Tower 2F has staircases
// landing in disconnected rooms; without the tile the router picks whichever
// room is listed first and blacklists its way to NO_EDGE.
// gSaveBlock1Ptr->pos is map-local, which is exactly what IndexOfAt wants.
static u16 CurrentNode(void)
{
    return WorldGraph_IndexOfAt(gSaveBlock1Ptr->location.mapGroup,
                                gSaveBlock1Ptr->location.mapNum,
                                gSaveBlock1Ptr->pos.x,
                                gSaveBlock1Ptr->pos.y);
}

const struct AwCentre *AW_FindNearestCentre(u8 group, u8 num)
{
    u16 head = 0, tail = 0, i;
    u16 start = CurrentNode();
    (void)group; (void)num;

    if (start >= WORLD_MAP_COUNT)
        return NULL;

    for (i = 0; i < WORLD_MAP_COUNT; i++)
        sNav.dist[i] = NAV_UNREACHABLE;

    sNav.dist[start] = 0;
    sNav.q.inter[tail++] = start;

    while (head < tail)
    {
        u16 m = sNav.q.inter[head++];
        u16 s = gWorldEdgeStart[m], e = gWorldEdgeStart[m + 1];
        const struct AwCentre *c =
            AW_FindCentre((u16)((gWorldMaps[m].group << 8) | gWorldMaps[m].num));

        // BFS order means the first Centre dequeued is a nearest one. Ties
        // resolve to table order, which is deterministic -- worth having, since
        // an A/B whose heal destination changed on a tie would be unreadable.
        if (c != NULL)
            return c;

        for (; s < e; s++)
        {
            u16 d = gWorldEdges[s].dest;

            if (d < WORLD_MAP_COUNT && sNav.dist[d] == NAV_UNREACHABLE)
            {
                sNav.dist[d] = sNav.dist[m] + 1;
                if (tail < WORLD_MAP_COUNT)
                    sNav.q.inter[tail++] = d;
            }
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------

void AW_Nav_Reset(void)
{
    sNav.state = NAV_STATE_IDLE;
    sNav.haveGoal = FALSE;
    sNav.blockReason = AW_BLOCK_NONE;
    sNav.sawReason = AW_BLOCK_NONE;
    sNav.replans = 0;
    sNav.stall = 0;
    sNav.qHead = sNav.qTail = 0;
    sNav.goalMapIdx = NAV_NO_MAP;
    sNav.lastX = -1;
    sNav.lastY = -1;
    sNav.lastGroup = 0xFF;
    sNav.lastNum = 0xFF;
    sNav.reenterDir = 0;
    sNav.reenterTries = 0;
    sNav.waitTicks = 0;
    sNav.failedSearchX = -32000;
    sNav.failedSearchY = -32000;
}

bool32 AW_Nav_HasGoal(void)
{
    return sNav.haveGoal;
}

u16 AW_Nav_DebugHere(void) { return sNav.dbgHere; }
// Reported by the ROM itself. Every hand-derived offset for sNav has been
// wrong at least once this session, and a wrong offset returns a confident
// number rather than an error.
u8  AW_Nav_DebugDistGoal(void) { return sNav.goalMapIdx < WORLD_MAP_COUNT ? sNav.dist[sNav.goalMapIdx] : 254; }
u8  AW_Nav_DebugDistHere(void) { return sNav.dist[CurrentNode()]; }
u16 AW_Nav_DebugGoalIdx(void) { return sNav.goalMapIdx; }
u16 AW_Nav_DebugQ(void) { return (u16)(sNav.qTail * 256 + (sNav.qHead & 0xFF)); }

// Collision for stepping onto the current target from each of its four
// neighbours, packed one nibble per direction (S,N,W,E as DIR_SOUTH..DIR_EAST).
// The BFS expands predecessors of the target, so if every nibble is non-zero
// the search can never leave its seed -- which is exactly the observed
// qHead==qTail==1.
// Control experiment: the same probe aimed at the player's own neighbours.
// The player is standing on walkable ground by definition, so if this reports
// IMPASSABLE in every direction the probe is lying and the geometry is fine.
u16 AW_Nav_DebugPlayerCollision(void)
{
    u16 packed = 0;
    u8 dir;
    s16 px, py;

    PlayerGetDestCoords(&px, &py);
    for (dir = DIR_SOUTH; dir <= DIR_EAST; dir++)
    {
        s16 tx = px + sDx[dir], ty = py + sDy[dir];
        enum Collision col;

        sProbe.currentCoords.x = px;
        sProbe.currentCoords.y = py;
        sProbe.previousCoords.x = px;
        sProbe.previousCoords.y = py;
        sProbe.currentElevation = MapGridGetElevationAt(px, py);
        sProbe.currentMetatileBehavior = MapGridGetMetatileBehaviorAt(px, py);
        col = GetCollisionAtCoords(&sProbe, tx, ty, dir);
        packed |= (u16)((col & 0xF) << ((dir - DIR_SOUTH) * 4));
    }
    return packed;
}

u16 AW_Nav_DebugTgtCollision(void)
{
    u16 packed = 0;
    u8 dir;

    for (dir = DIR_SOUTH; dir <= DIR_EAST; dir++)
    {
        s16 nx = sNav.tgtX - sDx[dir];
        s16 ny = sNav.tgtY - sDy[dir];
        enum Collision col;

        sProbe.currentCoords.x = nx;
        sProbe.currentCoords.y = ny;
        sProbe.previousCoords.x = nx;
        sProbe.previousCoords.y = ny;
        sProbe.currentElevation = MapGridGetElevationAt(nx, ny);
        sProbe.currentMetatileBehavior = MapGridGetMetatileBehaviorAt(nx, ny);
        col = GetCollisionAtCoords(&sProbe, sNav.tgtX, sNav.tgtY, dir);
        packed |= (u16)((col & 0xF) << ((dir - DIR_SOUTH) * 4));
    }
    return packed;
}
u8 AW_Nav_DebugWhich(void) { return sNav.dbgWhich; }

// Packed so one u16 carries the whole escape story: did it fire, did a hop
// ever land, is it mid-hop right now, and how many tries are left on this goal.
u16 AW_Nav_DebugEscape(void)
{
    return (u16)((sNav.escapeFired & 0x3F) << 10)
         | (u16)((sNav.escapeArrived & 0x3F) << 4)
         | (u16)((sNav.escapeMode ? 1 : 0) << 3)
         | (u16)(sNav.escapeTries & 0x07);
}
u8 AW_Nav_DebugState(void) { return sNav.state; }
u8 AW_Nav_DebugBadEdges(void) { return sNav.badEdges; }
u16 AW_Nav_DebugTgt(void) { return (u16)(((sNav.tgtX & 0xFF) << 8) | (sNav.tgtY & 0xFF)); }
u16 AW_Nav_DebugWin(void) { return (u16)(((sNav.winX & 0xFF) << 8) | (sNav.winY & 0xFF)); }

u8 AW_Nav_BlockReason(void)
{
    return sNav.blockReason;
}

bool32 AW_Nav_SetGoal(u8 mapGroup, u8 mapNum, s16 x, s16 y)
{
    // The runner re-plans on every BLOCKED verdict, which calls straight back
    // into here. Resetting the escape budget and the blacklist unconditionally
    // meant neither could ever accumulate: escapeTries was pinned at 1 forever,
    // so the limit that triggers a re-route was never reached, and the agent
    // re-chose the edge it had just proven impossible. Both are facts about
    // this journey, so both must outlive a re-plan -- but not the goal.
    {
        bool32 sameGoal = sNav.goalEverSet
                       && sNav.goalGroup == mapGroup && sNav.goalNum == mapNum
                       && sNav.goalX == x + MAP_OFFSET && sNav.goalY == y + MAP_OFFSET;
        if (!sameGoal)
        {
            sNav.escapeTries = 0;
            sNav.escapeFromDir = DIR_NONE;
            sNav.badEdges = 0;
            sNav.inboundEdge = NAV_NO_EDGE;   // never blame a stale entrance
        }
    }
    sNav.escapeMode = FALSE;        // never mid-hop across a re-plan
    sNav.chosenEdge = NAV_NO_EDGE;

    u16 destIdx = WorldGraph_IndexOfAt(mapGroup, mapNum, (u8)x, (u8)y);
    u16 here;

    AW_Nav_Reset();

    if (destIdx >= WORLD_MAP_COUNT)
        return FALSE;

    // Objective tables and map.json use map-local coordinates; the engine's
    // grid is offset by MAP_OFFSET. Converting here keeps the offset out of
    // every caller.
    sNav.goalGroup = mapGroup;
    sNav.goalNum = mapNum;
    sNav.goalX = x + MAP_OFFSET;
    sNav.goalY = y + MAP_OFFSET;
    sNav.goalMapIdx = destIdx;

    BuildInterRoute(destIdx);

    here = CurrentNode();
    if (here >= WORLD_MAP_COUNT || sNav.dist[here] == NAV_UNREACHABLE)
    {
        sNav.blockReason = AW_BLOCK_UNREACHABLE;
        sNav.state = NAV_STATE_FAILED;
        return FALSE;
    }

    sNav.haveGoal = TRUE;
    sNav.goalEverSet = TRUE;
    sNav.state = NAV_STATE_IDLE;
    return TRUE;
}

// Pick the next hop off this map and turn it into a tile to walk to.
// Returns FALSE when this map has no edge that reduces the distance.
static bool32 EdgeIsBad(u16 idx)
{
    u8 i;
    for (i = 0; i < sNav.badEdges; i++)
        if (sNav.badEdge[i] == idx)
            return TRUE;
    return FALSE;
}

static void MarkEdgeBad(u16 idx)
{
    if (idx == NAV_NO_EDGE || sNav.badEdges >= NAV_BAD_EDGES || EdgeIsBad(idx))
        return;
    sNav.badEdge[sNav.badEdges++] = idx;
}

static bool32 ChooseIntraTarget(u16 hereIdx, s16 px, s16 py)
{
    u16 s = gWorldEdgeStart[hereIdx], e = gWorldEdgeStart[hereIdx + 1];
    u8 best = sNav.dist[hereIdx];

    u8 prevConnDir = sNav.tgtConnDir;
    s16 prevConnX = sNav.tgtX, prevConnY = sNav.tgtY;

    sNav.doorDir = DIR_NONE;    // cleared for every hop, not only warp hops
    sNav.tgtConnDir = DIR_NONE;

    if (hereIdx == sNav.goalMapIdx)
    {
        sNav.tgtX = sNav.goalX;
        sNav.tgtY = sNav.goalY;
        return TRUE;
    }

    // Pass 0 takes a shortest-path edge. Pass 1 runs only if every shortest
    // edge is blacklisted, and accepts the best remaining edge even though it
    // is a longer route -- a route one hop longer beats a route through a wall.
    {
    u8 pass;
    u16 s0 = s;
    for (pass = 0; pass < 2; pass++)
    {
    u8 relaxed = 255;
    if (pass == 1)
    {
        u16 k;
        for (k = s0; k < e; k++)
            if (!EdgeIsBad(k) && sNav.dist[gWorldEdges[k].dest] < relaxed)
                relaxed = sNav.dist[gWorldEdges[k].dest];
        if (relaxed == 255)
            break;              // nothing left at all
    }
    for (s = s0; s < e; s++)
    {
        const struct WorldEdge *ed = &gWorldEdges[s];

        if (EdgeIsBad(s))
            continue;
        if (pass == 0 ? (sNav.dist[ed->dest] != best - 1)
                      : (sNav.dist[ed->dest] != relaxed))
            continue;
        sNav.chosenEdge = s;

        if (ed->kind == WORLD_EDGE_WARP)
        {
            u16 w = gWorldWarpStart[hereIdx] + ed->param;
            s16 wx, wy;
            u8 d;

            if (w >= gWorldWarpStart[hereIdx + 1])
                continue;
            wx = gWorldWarps[w].x + MAP_OFFSET;
            wy = gWorldWarps[w].y + MAP_OFFSET;
            // Staircases, ladders and floor warps are walkable, so aim at them
            // directly. A building door is a wall: every approach reports
            // IMPASSABLE, and aiming at it seeds a search that cannot expand
            // a single cell. Detect that and aim at the approach tile instead.
            for (d = DIR_SOUTH; d <= DIR_EAST; d++)
            {
                s16 ax = wx - sDx[d], ay = wy - sDy[d];

                enum Collision c = ProbeCollision(ax, ay, wx, wy, d);

                // Only true geometry disqualifies a tile. An OBJECT_EVENT
                // verdict means something movable is standing there -- often
                // the player themselves, since the probe is a stranger to
                // DoesObjectCollideWithObjectAt. Treating that as a wall
                // reclassified the 2F staircase the player spawns on as a
                // door and walked the agent off it forever.
                if (c == COLLISION_NONE || c == COLLISION_OBJECT_EVENT)
                {
                    sNav.tgtX = wx;
                    sNav.tgtY = wy;
                    return TRUE;           // reachable warp: normal case
                }
            }

            // Unreachable from every side. Stand on the tile below it -- the
            // way a player approaches a door -- and press up on arrival.
            sNav.tgtX = wx;
            sNav.tgtY = wy + 1;
            sNav.doorDir = DIR_NORTH;
            return TRUE;
        }
        else
        {
            // A map connection has no single tile: we just have to reach the
            // border on that side. Aim far past the edge and let the window
            // seeding walk us there.
            const struct MapLayout *lay = gMapHeader.mapLayout;
            s16 w = lay ? (s16)lay->width : 64;
            s16 h = lay ? (s16)lay->height : 64;

            // Aim at the last tile INSIDE the map on that side and record the
            // direction to press on arrival. Same physical pattern as a door:
            // stand on the boundary tile, press outward, cross.
            switch (ed->param)
            {
            case 0: sNav.tgtX = px; sNav.tgtY = MAP_OFFSET;
                    sNav.tgtConnDir = DIR_NORTH; break;
            case 1: sNav.tgtX = px; sNav.tgtY = h - 1 + MAP_OFFSET;
                    sNav.tgtConnDir = DIR_SOUTH; break;
            case 2: sNav.tgtX = MAP_OFFSET; sNav.tgtY = py;
                    sNav.tgtConnDir = DIR_WEST; break;
            default: sNav.tgtX = w - 1 + MAP_OFFSET; sNav.tgtY = py;
                    sNav.tgtConnDir = DIR_EAST; break;
            }

            // The cross-axis coordinate above is the PLAYER's, so the target
            // drifts every time they step sideways. `targetMoved` then fires on
            // nearly every tick and restarts the window search before it can
            // finish, so the gradient is rebuilt from scratch forever and the
            // half-built field walks the player back and forth -- the observed
            // x39<->x50 thrash on Route 29. Hold the target still for the
            // duration of one hop; SeedGoal seeds the whole border line anyway,
            // so the exact cross-axis value never mattered.
            if (prevConnDir == sNav.tgtConnDir)
            {
                sNav.tgtX = prevConnX;
                sNav.tgtY = prevConnY;
            }
            sNav.doorDir = sNav.tgtConnDir;
            return TRUE;
        }
    }
    }
    }
    return FALSE;
}

static u8 NavTickInner(void)
{
    s16 px, py;
    u16 hereIdx;
    u8 group, num;
    bool32 mapChanged, targetMoved;
    s16 prevTgtX, prevTgtY;

    if (!sNav.haveGoal)
        return AW_NAV_NO_ROUTE;
    if (sNav.state == NAV_STATE_FAILED)
        return AW_NAV_BLOCKED;

    // A script, a cutscene or a textbox owns the field. Pressing a direction
    // now would be swallowed at best and would mash through a dialogue choice
    // at worst, so the router stands down and lets the caller handle it.
    if (ArePlayerFieldControlsLocked() || ScriptContext_IsEnabled())
        return AW_NAV_WAIT;

    group = gSaveBlock1Ptr->location.mapGroup;
    num = gSaveBlock1Ptr->location.mapNum;
    PlayerGetDestCoords(&px, &py);

    hereIdx = CurrentNode();
    if (hereIdx >= WORLD_MAP_COUNT)
    {
        // Script-entered dynamic maps are not in the static graph at all.
        sNav.dbgHere = hereIdx; sNav.dbgDist = 0; sNav.dbgWhich = 1;
        sNav.blockReason = AW_BLOCK_UNREACHABLE;
        sNav.state = NAV_STATE_FAILED;
        return AW_NAV_BLOCKED;
    }

    if (hereIdx == sNav.goalMapIdx && px == sNav.goalX && py == sNav.goalY)
    {
        sNav.state = NAV_STATE_DONE;
        return AW_NAV_ARRIVED;
    }

    if (sNav.dist[hereIdx] == NAV_UNREACHABLE)
    {
        // A warp dropped us somewhere with no route home. Re-solve from here
        // rather than giving up: the graph is directed and the way back may
        // simply be a different chain.
        BuildInterRoute(sNav.goalMapIdx);
        if (sNav.dist[hereIdx] == NAV_UNREACHABLE)
        {
            sNav.dbgHere = hereIdx; sNav.dbgDist = sNav.dist[hereIdx]; sNav.dbgWhich = 2;
            sNav.blockReason = AW_BLOCK_UNREACHABLE;
            sNav.state = NAV_STATE_FAILED;
            return AW_NAV_BLOCKED;
        }
    }

    mapChanged = (group != sNav.lastGroup || num != sNav.lastNum);
    if (mapChanged)
    {
        // A new map earns a fresh escape budget -- and only a new map does.
        // Resetting on a changed TILE is what made two earlier watchdogs
        // useless: an oscillating agent changes tile every step.
        //
        // 0xFF is "no previous map recorded", which AW_Nav_Reset writes on
        // every re-plan -- and the runner re-plans on every BLOCKED verdict.
        // Treating that as a real map change reset the escape budget each
        // time, pinning escapeTries at 1 so the limit that triggers a
        // re-route was never reached and the agent ground the same impossible
        // edge forever. Only a genuine map-to-map transition counts.
        if (sNav.lastGroup != 0xFF)
        {
            // The edge we were pursuing is the one that just delivered us.
            // Remember it before the budget resets, so if this map turns out
            // to be a dead pocket we can blame the entrance, not just the exit.
            sNav.inboundEdge = sNav.chosenEdge;
            sNav.escapeMode = FALSE;
            sNav.escapeTries = 0;
            sNav.escapeFromDir = DIR_NONE;
            // badEdges deliberately persists across maps: a floor proven to
            // be a dead pocket must stay blamed after we leave it, or the
            // router sends us straight back in through the same staircase.
        }
    }

    prevTgtX = sNav.tgtX;
    prevTgtY = sNav.tgtY;
    if (!ChooseIntraTarget(hereIdx, px, py))
    {
        sNav.blockReason = AW_BLOCK_NO_EDGE;
        sNav.state = NAV_STATE_FAILED;
        return AW_NAV_BLOCKED;
    }
    targetMoved = (sNav.tgtX != prevTgtX || sNav.tgtY != prevTgtY);
    // NOTE (stall 9): keying this on mapChanged alone was tried, on the theory
    // that a door oscillation alternates the target and so kept resetting its
    // own limit. Under the deterministic repro it changed NOTHING -- the Elm's
    // Lab oscillation is byte-identical with and without it -- so this counter
    // is not the one being exercised there, and the change was reverted rather
    // than left in as untested code with a real regression risk (a door that
    // legitimately needs more than NAV_REENTER_LIMIT attempts in one map).
    // The oscillation comes from somewhere else; look at ChooseIntraTarget.
    if (targetMoved || mapChanged)
        sNav.reenterTries = 0;      // a new hop deserves a fresh set of attempts

    // Progress watchdog. If we have been issuing presses and the player has not
    // moved, the field is not doing what the plan assumes -- an NPC wandered
    // into the way, a tile changed, or the plan was wrong. Re-plan rather than
    // grinding into a wall, and give up honestly after a few attempts.
    if (px == sNav.lastX && py == sNav.lastY && !mapChanged)
    {
        if (++sNav.stall > NAV_STALL_REPLAN)
        {
            sNav.stall = 0;
            if (++sNav.replans > NAV_REPLAN_LIMIT)
            {
                sNav.blockReason = sNav.sawReason != AW_BLOCK_NONE
                                 ? sNav.sawReason : AW_BLOCK_UNREACHABLE;
                sNav.state = NAV_STATE_FAILED;
                return AW_NAV_BLOCKED;
            }
            StartIntraSearch(px, py);
            return AW_NAV_WAIT;
        }
    }
    else
    {
        sNav.stall = 0;
        sNav.replans = 0;
        sNav.lastX = px;
        sNav.lastY = py;
    }
    sNav.lastGroup = group;
    sNav.lastNum = num;

    // (Re)start the window search when the map changed, the hop target moved,
    // or we walked out of the window we planned in.
    if (mapChanged || targetMoved || sNav.state == NAV_STATE_IDLE
     || sNav.state == NAV_STATE_DONE || !InWindow(px, py))
        StartIntraSearch(px, py);

    if (sNav.state == NAV_STATE_SEARCHING)
    {
        if (!StepIntraSearch())
            return AW_NAV_WAIT;         // still thinking; press nothing
        sNav.state = NAV_STATE_FOLLOWING;
    }

    {
        u8 d = DirGet(CellOf(px, py));

        if (d == 0)
        {
            // The search finished and never reached us. Recentring can only
            // help if the target was outside the window to begin with.
            if (sNav.tgtIsEdgeSeed || !InWindow(sNav.tgtX, sNav.tgtY))
            {
                sNav.blockReason = AW_BLOCK_WINDOW;
                StartIntraSearch(px, py);
                return AW_NAV_WAIT;
            }
            // Before declaring the goal impossible, ask whether the only
            // thing in the way was a person. This is the stall-8 escape: the
            // strict search cannot reach a warp an NPC is standing on, but the
            // warp is perfectly usable once they step aside.
            if (NavPermissiveRetry(px, py))
                return AW_NAV_WAIT;

            // Every escape hop has been spent and the hop target is still
            // unreachable. At this point the map has proven the router's
            // chosen edge unwalkable, so stop trying to reach it: blacklist
            // the edge and let ChooseIntraTarget pick another, accepting a
            // longer route if it must. Without this the agent grinds the same
            // impossible edge forever -- observed on Route 31, 58 escape hops
            // against a walled west column while the usable gate door sat one
            // tile away.
            if (sNav.escapeTries >= NAV_ESCAPE_LIMIT
             && sNav.chosenEdge != NAV_NO_EDGE && sNav.badEdges < NAV_BAD_EDGES)
            {
                MarkEdgeBad(sNav.chosenEdge);
                MarkEdgeBad(sNav.inboundEdge);
                // ...and the edge that put us here. Without this the router
                // walks back out, re-picks the same entrance, and returns to
                // the identical unreachable pocket.
                sNav.chosenEdge = NAV_NO_EDGE;
                sNav.escapeMode = FALSE;
                sNav.escapeTries = 0;
                sNav.escapeFromDir = DIR_NONE;
                sNav.failedSearchX = -1;
                sNav.failedSearchY = -1;
                sNav.tgtConnDir = DIR_NONE;   // force a fresh target choice
                sNav.state = NAV_STATE_IDLE;  // re-enter planning next tick
                return AW_NAV_WAIT;
            }

            // Escape hops exist to try a DIFFERENT window. When the whole
            // map already fits inside one, there is no other window to try
            // and a completed failed search is conclusive -- so spending the
            // budget just burns frames until the runner's no-progress
            // watchdog fires first, which is exactly what kept the blacklist
            // from ever triggering on Sprout Tower 2F (25x25 inside a 48
            // window, and the 3F stairs outside the reachable pocket).
            {
                const struct MapLayout *lay = gMapHeader.mapLayout;
                if (lay && lay->width <= NAV_WIN && lay->height <= NAV_WIN
                 && sNav.chosenEdge != NAV_NO_EDGE
                 && sNav.badEdges < NAV_BAD_EDGES)
                {
                    MarkEdgeBad(sNav.chosenEdge);
                    MarkEdgeBad(sNav.inboundEdge);
                    sNav.chosenEdge = NAV_NO_EDGE;
                    sNav.tgtConnDir = DIR_NONE;
                    sNav.state = NAV_STATE_IDLE;
                    return AW_NAV_WAIT;
                }
            }

            // The goal-facing edge is unreachable from this window. Walk to
            // whichever edge IS reachable, recentre there, and search again.
            if (!sNav.escapeMode && sNav.escapeTries < NAV_ESCAPE_LIMIT)
            {
                sNav.escapeTries++;
                if (sNav.escapeFired < 255) sNav.escapeFired++;
                sNav.escapeMode = TRUE;
                sNav.failedSearchX = -1;      // a fresh window may succeed
                sNav.failedSearchY = -1;
                StartIntraSearch(px, py);
                return AW_NAV_WAIT;
            }

            sNav.blockReason = sNav.sawReason != AW_BLOCK_NONE
                             ? sNav.sawReason : AW_BLOCK_UNREACHABLE;
            sNav.state = NAV_STATE_FAILED;
            return AW_NAV_BLOCKED;
        }

        if (d == NAV_DIR_GOAL)
        {
            if (sNav.tgtIsEdgeSeed)
            {
                // Reached the window edge: recentre and keep going. If the
                // recentre is from the very position that already failed,
                // repeating it cannot produce a different answer.
                if (px == sNav.failedSearchX && py == sNav.failedSearchY)
                {
                    sNav.blockReason = AW_BLOCK_WINDOW;
                    sNav.state = NAV_STATE_FAILED;
                    return AW_NAV_BLOCKED;
                }
                if (sNav.escapeMode)
                {
                    // Arrived at a reachable boundary. Remember which side we
                    // left by so the next escape cannot immediately come back,
                    // then resume ordinary goal-facing seeding from the new
                    // window -- the whole point of the hop is that the goal may
                    // now be reachable.
                    if (px <= sNav.winX)                      sNav.escapeFromDir = DIR_EAST;
                    else if (px >= sNav.winX + NAV_WIN - 1)   sNav.escapeFromDir = DIR_WEST;
                    else if (py <= sNav.winY)                 sNav.escapeFromDir = DIR_SOUTH;
                    else                                      sNav.escapeFromDir = DIR_NORTH;
                    if (sNav.escapeArrived < 255) sNav.escapeArrived++;
                    sNav.escapeMode = FALSE;
                }
                sNav.failedSearchX = px;
                sNav.failedSearchY = py;
                StartIntraSearch(px, py);
                return AW_NAV_WAIT;
            }

            // Standing ON the hop target. Getting here is the normal case
            // after a map load, because the player spawns on the staircase.
            //
            // This was the hang, and the first fix was also wrong. Warps come
            // in two kinds and they want opposite things:
            //
            //   * Doors, ladders and escalators fire from TryStartWarpEventScript
            //     when the player STEPS ON to the tile (field_control_avatar.c:
            //     IsWarpMetatileBehavior). Standing still never triggers them,
            //     so the agent must step off and walk back on.
            //   * Arrow warps and directional stairs fire from TryArrowWarp
            //     (field_control_avatar.c:972) only while the player is standing
            //     on the tile and HOLDING the matching direction. Stepping off
            //     one of these is precisely wrong -- which is why the agent
            //     oscillated beside the staircase instead of descending.
            //
            // The behaviour byte says which kind this is, so ask it rather than
            // guessing or brute-forcing directions.
            {
                u8 b = MapGridGetMetatileBehaviorAt(px, py);

                // A door we are standing BESIDE, not on. Gen 3 building doors
                // are impassable tiles with a warp attached: the player never
                // occupies one, they stand on the approach tile and press into
                // it. ChooseIntraTarget aimed us here precisely for this.
                if (sNav.doorDir == DIR_NORTH)
                    return AW_NAV_UP;
                if (sNav.doorDir == DIR_SOUTH)
                    return AW_NAV_DOWN;
                if (sNav.doorDir == DIR_WEST)
                    return AW_NAV_LEFT;
                if (sNav.doorDir == DIR_EAST)
                    return AW_NAV_RIGHT;

                if (MetatileBehavior_IsNorthArrowWarp(b))
                    return AW_NAV_UP;
                if (MetatileBehavior_IsSouthArrowWarp(b))
                    return AW_NAV_DOWN;
                if (MetatileBehavior_IsWestArrowWarp(b))
                    return AW_NAV_LEFT;
                if (MetatileBehavior_IsEastArrowWarp(b))
                    return AW_NAV_RIGHT;

                // Directional stairs encode the facing in the behaviour name.
                if (MetatileBehavior_IsDirectionalUpRightStairWarp(b))
                    return AW_NAV_RIGHT;
                if (MetatileBehavior_IsDirectionalUpLeftStairWarp(b))
                    return AW_NAV_LEFT;
                if (MetatileBehavior_IsDirectionalDownRightStairWarp(b))
                    return AW_NAV_RIGHT;
                if (MetatileBehavior_IsDirectionalDownLeftStairWarp(b))
                    return AW_NAV_LEFT;

                // A step-on warp. Step off; the next tick routes back on and
                // the arrival triggers it. Attempts are counted per target, not
                // per frame -- counting per frame and resetting on movement
                // meant the oscillation itself kept resetting the counter and
                // the limit could never trip.
                if (++sNav.reenterTries > NAV_REENTER_LIMIT)
                {
                    sNav.blockReason = AW_BLOCK_NO_EDGE;
                    sNav.state = NAV_STATE_FAILED;
                    return AW_NAV_BLOCKED;
                }
                {
                    u8 t;

                    for (t = DIR_SOUTH; t <= DIR_EAST; t++)
                    {
                        s16 ox = px + sDx[t], oy = py + sDy[t];

                        if (InWindow(ox, oy) && CanStep(px, py, ox, oy, t))
                        {
                            sNav.reenterDir = t;
                            return DirToResult(t);
                        }
                    }
                }
            }
        }

        // Wait rather than bump. A permissive route may step through a tile
        // somebody is standing on; walking into them does not move the player,
        // so issuing the key would burn the progress watchdog on a tile that
        // will very likely be free in a moment. Hold still, re-probe each tick,
        // and only give up once they have proved they are not moving.
        {
            s16 nx = px + sDx[d], ny = py + sDy[d];

            if (ProbeCollision(px, py, nx, ny, d) == COLLISION_OBJECT_EVENT)
            {
                if (++sNav.npcWait <= NAV_NPC_WAIT_LIMIT)
                    return AW_NAV_WAIT;

                // Immovable. Fall through and press: if they are a wall in
                // practice the stall watchdog should see it and replan, which
                // is more honest than waiting forever.
            }
            else
            {
                sNav.npcWait = 0;
            }
        }

        return DirToResult(d);
    }
}

// A navigator that can think forever is a hang the caller cannot distinguish
// from a crash. Every WAIT path -- including any added later -- passes through
// here, so the bound holds by construction rather than by remembering to check
// it at each return.
u8 AW_Nav_Tick(void)
{
    u8 r;

    // Time spent waiting for a cutscene is not the navigator thinking, and
    // must not count against its budget: dialogue can legitimately run for
    // thousands of frames.
    if (ArePlayerFieldControlsLocked() || ScriptContext_IsEnabled())
    {
        sNav.waitTicks = 0;
        return NavTickInner();
    }

    r = NavTickInner();
    if (r != AW_NAV_WAIT)
    {
        sNav.waitTicks = 0;
        return r;
    }

    if (++sNav.waitTicks > NAV_MAX_WAIT)
    {
        sNav.waitTicks = 0;
        sNav.blockReason = sNav.sawReason != AW_BLOCK_NONE
                         ? sNav.sawReason : AW_BLOCK_UNREACHABLE;
        sNav.state = NAV_STATE_FAILED;
        return AW_NAV_BLOCKED;
    }
    return AW_NAV_WAIT;
}
