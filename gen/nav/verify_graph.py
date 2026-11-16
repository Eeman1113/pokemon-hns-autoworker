#!/usr/bin/env python3
"""verify_graph.py - prove the world graph before anything trusts it.

Checks, in order of how badly a failure would hurt the router:
  1. every warp destination resolves to a real map and a real warp id
  2. connectivity: the largest component, and what sits outside it
  3. landmark shortest paths, to sanity-check that hop counts are plausible
  4. one-way edges, which are where a naive router strands itself
"""
import json, os
from collections import deque, defaultdict, Counter

OUT = os.path.dirname(os.path.abspath(__file__))
g = json.load(open(os.path.join(OUT, "world_graph.json")))
maps, edges = g["maps"], {int(k): v for k, v in g["edges"].items()}
order = g["group_order"]

byname = {m["name"]: m["index"] for m in maps if m}
# The ROM links three games' maps. Only groups 0..30 are Heart & Soul; the rest
# are vanilla Emerald and FireRed/LeafGreen content that HnS play never enters.
HNS_GROUPS = set(range(0, 31))
is_hns = lambda m: m and m["group"] in HNS_GROUPS

def adj(directed=True):
    a = defaultdict(list)
    for s, es in edges.items():
        for e in es:
            a[s].append(e["dest"])
            if not directed:
                a[e["dest"]].append(s)
    return a

def bfs(start, a, allowed=None):
    seen = {start: 0}
    q = deque([start])
    while q:
        u = q.popleft()
        for v in a.get(u, []):
            if v in seen:
                continue
            if allowed and not allowed(maps[v]):
                continue
            seen[v] = seen[u] + 1
            q.append(v)
    return seen

print("=" * 68)
print("1. REFERENTIAL INTEGRITY")
print("=" * 68)
un = g["unresolved"]
dyn = [x for x in un if x[2] == "MAP_DYNAMIC"]
real = [x for x in un if x[2] != "MAP_DYNAMIC"]
print(f"  warps total            {sum(len(m['warps']) for m in maps if m)}")
print(f"  unresolved -> MAP_DYNAMIC (script-set, expected) {len(dyn)}")
print(f"  unresolved -> REAL DATA BUGS                     {len(real)}")
for x in real:
    print(f"      {x[0]} warp {x[1]} -> {x[2]}: {x[3]}")

print()
print("=" * 68)
print("2. CONNECTIVITY (directed, from the player's start)")
print("=" * 68)
a = adj()
start = byname.get("NewBarkTown_PlayersHouse_2F_hns") or byname.get("NewBarkTown_hns")
print(f"  start node: {maps[start]['name']} (idx {start}, group {maps[start]['group']})")
reach = bfs(start, a)
hns_total = sum(1 for m in maps if is_hns(m))
hns_reached = sum(1 for i in reach if is_hns(maps[i]))
print(f"  reachable overall      {len(reach)} / {len(maps)}")
print(f"  reachable HnS maps     {hns_reached} / {hns_total}")
unreached_hns = [maps[i]['name'] for i in range(len(maps))
                 if is_hns(maps[i]) and i not in reach]
print(f"  UNREACHABLE HnS maps   {len(unreached_hns)}")
grp = Counter(order[maps[byname[n]]['group']] for n in unreached_hns)
for k, v in grp.most_common(8):
    print(f"      {v:4d}  {k}")
print("  sample:", ", ".join(unreached_hns[:6]))

other = [i for i in reach if not is_hns(maps[i])]
print(f"  non-HnS maps reachable from HnS start: {len(other)}")
if other:
    print("      ", ", ".join(maps[i]['name'] for i in other[:6]))

print()
print("=" * 68)
print("3. LANDMARK ROUTES (hops = warps + connections traversed)")
print("=" * 68)
LM = ["NewBarkTown_hns", "CherrygroveCity_hns", "VioletCity_hns",
      "GoldenrodCity_hns", "EcruteakCity_hns", "OlivineCity_hns",
      "BlackthornCity_hns", "PalletTown_hns"]
have = [n for n in LM if n in byname]
missing = [n for n in LM if n not in byname]
if missing:
    print("  (not found, name differs):", ", ".join(missing))
for i in range(len(have) - 1):
    s, t = byname[have[i]], byname[have[i + 1]]
    d = bfs(s, a)
    hop = d.get(t)
    print(f"  {have[i]:24s} -> {have[i+1]:24s} {hop if hop is not None else 'UNREACHABLE'}")

print()
print("=" * 68)
print("4. ONE-WAY EDGES (router must not plan a return through these)")
print("=" * 68)
ow = g["one_way"]
print(f"  one-way warps: {len(ow)}")
c = Counter(x["from"].split("_")[0] for x in ow)
for k, v in c.most_common(8):
    print(f"      {v:4d}  {k}")
print("  sample:")
for x in ow[:5]:
    print(f"      {x['from']} w{x['warp']} -> {x['to']}; return leads to {x['return_goes_to']}")

print()
print("=" * 68)
print("5. DEGREE / SHAPE")
print("=" * 68)
deg = [len(edges.get(i, [])) for i in range(len(maps)) if maps[i]]
print(f"  mean out-degree {sum(deg)/len(deg):.2f}, max {max(deg)}")
iso = sum(1 for d in deg if d == 0)
print(f"  maps with no outgoing edge: {iso}")
ecc = bfs(start, a)
print(f"  eccentricity from start: max {max(ecc.values())} hops")
