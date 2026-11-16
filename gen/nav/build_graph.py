#!/usr/bin/env python3
"""build_graph.py - derive the Auto Worker world graph from the game's own data.

The repo already contains every map's warps, connections and dimensions. None of
that needs authoring: this reads data/maps/map_groups.json (the authoritative
list of what is actually linked into the ROM), joins it to each map.json, and
emits

    world_graph.json   machine-readable, for analysis and verification
    world_graph.h/.c   const tables for the ROM (CSR adjacency)

Addressing follows the game's own (mapGroup, mapNum) scheme so a route can be
handed straight to the warp/connection code at runtime.
"""
import json, os, sys, re
from collections import defaultdict

REPO = "/Users/eemanmajumder/hns/pokehns-expansion"
MAPS = os.path.join(REPO, "data", "maps")
LAYOUTS = os.path.join(REPO, "data", "layouts", "layouts.json")
OUT = os.path.dirname(os.path.abspath(__file__))

EDGE_WARP, EDGE_CONN = 0, 1
DIRS = {"up": 0, "down": 1, "left": 2, "right": 3,
        "north": 0, "south": 1, "west": 2, "east": 3,
        "dive": 4, "emerge": 5}

def load_groups():
    g = json.load(open(os.path.join(MAPS, "map_groups.json")))
    order = g["group_order"]
    name_to_addr, addr_order = {}, []
    for gi, gname in enumerate(order):
        for mi, mapname in enumerate(g[gname]):
            name_to_addr[mapname] = (gi, mi)
            addr_order.append(mapname)
    return order, name_to_addr, addr_order

def load_layouts():
    d = json.load(open(LAYOUTS))
    return {l["id"]: (l.get("width", 0), l.get("height", 0)) for l in d["layouts"]}

def parse_warp_id(v):
    """dest_warp_id is usually a decimal string but may be a named constant."""
    if isinstance(v, int):
        return v, None
    s = str(v).strip()
    if re.fullmatch(r"-?\d+", s):
        return int(s), None
    return None, s          # unresolved symbolic warp id

def main():
    order, name_to_addr, addr_order = load_groups()
    layouts = load_layouts()

    maps = []                 # index -> dict
    id_to_index = {}
    missing_json = []

    for idx, mapname in enumerate(addr_order):
        path = os.path.join(MAPS, mapname, "map.json")
        if not os.path.exists(path):
            missing_json.append(mapname)
            maps.append(None)
            continue
        d = json.load(open(path))
        gi, mi = name_to_addr[mapname]
        w, h = layouts.get(d.get("layout", ""), (0, 0))
        m = {
            "index": idx, "name": mapname, "id": d.get("id", ""),
            "group": gi, "num": mi,
            "layout": d.get("layout", ""), "width": w, "height": h,
            "map_type": d.get("map_type", ""), "weather": d.get("weather", ""),
            "mapsec": d.get("region_map_section", ""),
            "region": d.get("region", ""), "game_version": d.get("game_version", ""),
            "warps": [], "connections": [],
        }
        for wi, wp in enumerate(d.get("warp_events") or []):
            dw, sym = parse_warp_id(wp.get("dest_warp_id", 0))
            m["warps"].append({
                "i": wi, "x": wp.get("x", 0), "y": wp.get("y", 0),
                "elev": wp.get("elevation", 0),
                "dest_map": wp.get("dest_map", ""), "dest_warp": dw,
                "dest_warp_sym": sym,
            })
        for cn in (d.get("connections") or []):
            m["connections"].append({
                "map": cn.get("map", ""), "dir": cn.get("direction", ""),
                "offset": cn.get("offset", 0),
            })
        maps.append(m)
        if m["id"]:
            id_to_index[m["id"]] = idx

    # ---- build directed edges -------------------------------------------
    edges = defaultdict(list)     # src index -> list of edge dicts
    unresolved = []
    for m in maps:
        if m is None:
            continue
        for wp in m["warps"]:
            dst = id_to_index.get(wp["dest_map"])
            if dst is None:
                unresolved.append((m["name"], wp["i"], wp["dest_map"], "no such map"))
                continue
            dm = maps[dst]
            if wp["dest_warp"] is None:
                unresolved.append((m["name"], wp["i"], wp["dest_map"], f"symbolic warp id {wp['dest_warp_sym']}"))
                continue
            if wp["dest_warp"] >= len(dm["warps"]):
                unresolved.append((m["name"], wp["i"], wp["dest_map"],
                                   f"dest_warp_id {wp['dest_warp']} >= {len(dm['warps'])} warps"))
                continue
            edges[m["index"]].append({
                "kind": EDGE_WARP, "dest": dst,
                "param": wp["i"],          # which warp on the SOURCE map to step on
                "dest_warp": wp["dest_warp"],
            })
        for cn in m["connections"]:
            dst = id_to_index.get(cn["map"])
            if dst is None:
                unresolved.append((m["name"], -1, cn["map"], "connection to unknown map"))
                continue
            edges[m["index"]].append({
                "kind": EDGE_CONN, "dest": dst,
                "param": DIRS.get(cn["dir"], 255), "offset": cn["offset"],
            })

    # ---- one-way detection ----------------------------------------------
    # A warp is "paired" when the destination warp leads back to this map.
    one_way = []
    for m in maps:
        if m is None:
            continue
        for wp in m["warps"]:
            dst = id_to_index.get(wp["dest_map"])
            if dst is None or wp["dest_warp"] is None:
                continue
            dm = maps[dst]
            if wp["dest_warp"] >= len(dm["warps"]):
                continue
            back = dm["warps"][wp["dest_warp"]]
            if id_to_index.get(back["dest_map"]) != m["index"]:
                one_way.append({
                    "from": m["name"], "warp": wp["i"],
                    "to": dm["name"], "dest_warp": wp["dest_warp"],
                    "return_goes_to": back["dest_map"],
                })

    graph = {
        "group_order": order,
        "maps": maps,
        "edges": {str(k): v for k, v in edges.items()},
        "unresolved": unresolved,
        "one_way": one_way,
        "missing_json": missing_json,
    }
    with open(os.path.join(OUT, "world_graph.json"), "w") as f:
        json.dump(graph, f)

    n_maps = sum(1 for m in maps if m)
    n_warps = sum(len(m["warps"]) for m in maps if m)
    n_conn = sum(len(m["connections"]) for m in maps if m)
    n_edges = sum(len(v) for v in edges.values())
    print(f"maps            {len(maps)} slots, {n_maps} with map.json ({len(missing_json)} missing)")
    print(f"warps           {n_warps}")
    print(f"connections     {n_conn}")
    print(f"edges resolved  {n_edges}")
    print(f"unresolved      {len(unresolved)}")
    print(f"one-way warps   {len(one_way)}")
    emit_c(maps, edges, n_edges)

# ---------------------------------------------------------------------------
def emit_c(maps, edges, n_edges):
    """CSR adjacency. Tables are const so they live in ROM, never RAM."""
    n = len(maps)
    max_w = max((m["width"] for m in maps if m), default=0)
    max_h = max((m["height"] for m in maps if m), default=0)
    max_wx = max((w["x"] for m in maps if m for w in m["warps"]), default=0)
    max_wy = max((w["y"] for m in maps if m for w in m["warps"]), default=0)

    # flat warp table, indexed by CSR too
    warp_start, warps_flat = [], []
    for m in maps:
        warp_start.append(len(warps_flat))
        if m:
            for w in m["warps"]:
                warps_flat.append((w["x"], w["y"], w["elev"]))
    warp_start.append(len(warps_flat))

    edge_start, edges_flat = [], []
    for i in range(n):
        edge_start.append(len(edges_flat))
        for e in edges.get(i, []):
            edges_flat.append(e)
    edge_start.append(len(edges_flat))

    # reverse adjacency: predecessors of each node, for backward BFS
    rev = {}
    for s_i, es in edges.items():
        for e in es:
            rev.setdefault(e["dest"], []).append(s_i)
    rstart, rflat = [], []
    for i in range(n):
        rstart.append(len(rflat))
        for pred in rev.get(i, []):
            rflat.append(pred)
    rstart.append(len(rflat))

    h = []
    h.append("#ifndef GUARD_WORLD_GRAPH_H\n#define GUARD_WORLD_GRAPH_H\n")
    h.append("// GENERATED by gen/nav/build_graph.py -- do not edit by hand.\n")
    h.append(f"#define WORLD_MAP_COUNT   {n}")
    h.append(f"#define WORLD_EDGE_COUNT  {len(edges_flat)}")
    h.append(f"#define WORLD_WARP_COUNT  {len(warps_flat)}")
    h.append(f"#define WORLD_REV_COUNT   {len(rflat)}")
    h.append("#define WORLD_EDGE_WARP   0")
    h.append("#define WORLD_EDGE_CONN   1\n")
    h.append("""struct WorldMap
{
    u8  group;
    u8  num;
    u8  width;      // in metatiles, clamped; 0 means >255 (see WorldMapWideList)
    u8  height;
};

struct WorldEdge
{
    u16 dest;       // index into gWorldMaps
    u8  kind;       // WORLD_EDGE_WARP | WORLD_EDGE_CONN
    u8  param;      // warp: source warp id. conn: direction 0=U 1=D 2=L 3=R
};

struct WorldWarp
{
    u8 x;
    u8 y;
    u8 elev;
};

extern const struct WorldMap  gWorldMaps[WORLD_MAP_COUNT];
extern const u16              gWorldEdgeStart[WORLD_MAP_COUNT + 1];
extern const struct WorldEdge gWorldEdges[WORLD_EDGE_COUNT];
extern const u16              gWorldWarpStart[WORLD_MAP_COUNT + 1];
extern const struct WorldWarp gWorldWarps[WORLD_WARP_COUNT];

// Reverse adjacency. Backward BFS from the destination yields a
// distance-to-target field; the agent then simply steps to any neighbour with a
// lower distance. No path array, and the route repairs itself if the agent is
// displaced by a script, a battle or a ledge.
extern const u16 gWorldRevStart[WORLD_MAP_COUNT + 1];
extern const u16 gWorldRev[WORLD_REV_COUNT];

u16 WorldGraph_IndexOf(u8 group, u8 num);
""")
    h.append("#endif // GUARD_WORLD_GRAPH_H")
    open(os.path.join(OUT, "world_graph.h"), "w").write("\n".join(h) + "\n")

    c = ['#include "global.h"', '#include "world_graph.h"', ""]
    c.append("// GENERATED by gen/nav/build_graph.py -- do not edit by hand.")
    c.append(f"// {n} maps, {len(edges_flat)} edges, {len(warps_flat)} warps.")
    c.append("")
    c.append(f"const struct WorldMap gWorldMaps[WORLD_MAP_COUNT] =\n{{")
    for m in maps:
        if m:
            w = m["width"] if m["width"] < 256 else 0
            hh = m["height"] if m["height"] < 256 else 0
            c.append(f"    {{{m['group']},{m['num']},{w},{hh}}}, // {m['index']} {m['name']}")
        else:
            c.append("    {255,255,0,0}, // (no map.json)")
    c.append("};\n")

    c.append("const u16 gWorldEdgeStart[WORLD_MAP_COUNT + 1] =\n{")
    c.append("    " + ",".join(str(v) for v in edge_start))
    c.append("};\n")

    c.append("const struct WorldEdge gWorldEdges[WORLD_EDGE_COUNT] =\n{")
    for e in edges_flat:
        c.append(f"    {{{e['dest']},{e['kind']},{e['param']}}},")
    c.append("};\n")

    c.append("const u16 gWorldWarpStart[WORLD_MAP_COUNT + 1] =\n{")
    c.append("    " + ",".join(str(v) for v in warp_start))
    c.append("};\n")

    c.append("const struct WorldWarp gWorldWarps[WORLD_WARP_COUNT] =\n{")
    for (x, y, el) in warps_flat:
        c.append(f"    {{{min(x,255)},{min(y,255)},{el}}},")
    c.append("};\n")

    c.append("const u16 gWorldRevStart[WORLD_MAP_COUNT + 1] =\n{")
    c.append("    " + ",".join(str(v) for v in rstart))
    c.append("};\n")

    c.append("const u16 gWorldRev[WORLD_REV_COUNT] =\n{")
    c.append("    " + ",".join(str(v) for v in rflat))
    c.append("};\n")

    c.append("""u16 WorldGraph_IndexOf(u8 group, u8 num)
{
    u16 i;
    for (i = 0; i < WORLD_MAP_COUNT; i++)
    {
        if (gWorldMaps[i].group == group && gWorldMaps[i].num == num)
            return i;
    }
    return 0xFFFF;
}
""")
    open(os.path.join(OUT, "world_graph.c"), "w").write("\n".join(c) + "\n")

    b_maps = n * 4
    b_estart = (n + 1) * 2
    b_edges = len(edges_flat) * 4
    b_wstart = (n + 1) * 2
    b_warps = len(warps_flat) * 3
    b_rstart = (n + 1) * 2
    b_rev = len(rflat) * 2
    total = b_maps + b_estart + b_edges + b_wstart + b_warps + b_rstart + b_rev
    print()
    print("C table size (ROM, const):")
    print(f"  gWorldMaps       {n} x 4  = {b_maps:7d} B")
    print(f"  gWorldEdgeStart  {n+1} x 2  = {b_estart:7d} B")
    print(f"  gWorldEdges      {len(edges_flat)} x 4  = {b_edges:7d} B")
    print(f"  gWorldWarpStart  {n+1} x 2  = {b_wstart:7d} B")
    print(f"  gWorldWarps      {len(warps_flat)} x 3  = {b_warps:7d} B")
    print(f"  gWorldRevStart   {n+1} x 2  = {b_rstart:7d} B")
    print(f"  gWorldRev        {len(rflat)} x 2  = {b_rev:7d} B")
    print(f"  TOTAL                      = {total:7d} B  ({total/1024:.1f} KB of ~7.4 MB free)")
    print(f"  max map dims: {max_w} x {max_h}; max warp coord: x={max_wx} y={max_wy}")

if __name__ == "__main__":
    main()
