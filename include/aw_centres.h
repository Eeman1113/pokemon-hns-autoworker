#ifndef GUARD_AW_CENTRES_H
#define GUARD_AW_CENTRES_H

#include "global.h"

// Pokemon Centres, derived from map data by gen/quest4/build_centres.py.
//
// Why this table exists: the game records a heal destination only once the
// player has actually used a Centre, so gSaveBlock1Ptr->lastHealLocation reads
// the player's own front door at the start of the game. An agent that trusts it
// routes home, finds no nurse and loops. This gives the runner a destination it
// can use from the first frame.
//
// A Centre is identified by DATA, never by name: an object whose graphics is a
// NURSE variant (not CHANSEY, not UNION_ROOM) whose script is non-NULL. Name
// matching fails both ways -- BattleFrontier_Lounge9 carries a nurse sprite
// with script=NULL and heals nobody, while a Centre's 2F trade room carries the
// Centre's name and has no nurse at all.
//
// approachX/Y is where the PLAYER stands, not where the nurse is. Gen 3 puts
// the nurse behind a counter: the tile between is collision=1 with MB_COUNTER
// behaviour and the engine forwards the interaction across it, so the approach
// is usually TWO tiles from the nurse, and the naive adjacent tile is the
// Chansey's square.

struct AwCentre
{
    u16 mapId;
    u8  nurseX, nurseY;       // for cross-checking whatever FindNurse() locates
    u8  approachX, approachY; // where to stand
    u8  facing;               // DIR_* to face from the approach tile
    u8  counterSteps;         // 1 = adjacent, 2 = across a counter
    u8  warpX, warpY;         // entrance door, for the navigator's door handling
};

#define AW_NUM_CENTRES 30
extern const struct AwCentre gAwCentres[AW_NUM_CENTRES];

// The Centre ON this map, or NULL. An exact lookup -- not a search.
const struct AwCentre *AW_FindCentre(u16 mapId);

// Nearest Centre in hops from (group, num) by forward BFS over the warp graph,
// or NULL if none is reachable. This is what the heal routine wants.
const struct AwCentre *AW_FindNearestCentre(u8 group, u8 num);

#endif
