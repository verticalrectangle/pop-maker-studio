#!/usr/bin/env python3
"""Deterministic checks for ARKit → MediaPipe landmark mapping.

Validates topology-derived eye chains in arkit_landmark_map.h against
k_arkit_tris, and that critical MediaPipe indices used by the beauty/makeup
path have a non-zero hard map (or are documented runtime stubs).

No device / ARKit runtime required.
"""
from __future__ import annotations

import os
import re
import sys
from collections import defaultdict, deque

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
MAP_H = os.path.join(ROOT, "src", "generated", "arkit_landmark_map.h")
MESH_H = os.path.join(ROOT, "src", "generated", "arkit_face_mesh.h")
CPP = os.path.join(ROOT, "src", "arkit_face.cpp")


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    raise SystemExit(1)


def parse_int_array(text: str, name: str) -> list[int]:
    m = re.search(
        rf"static const int {re.escape(name)}\s*(?:\[[^\]]*\])?\s*=\s*([^;]+);",
        text,
        re.S,
    )
    if not m:
        fail(f"missing {name}")
    body = m.group(1)
    # scalar or brace list
    nums = [int(x) for x in re.findall(r"-?\d+", body)]
    if not nums:
        fail(f"empty {name}")
    return nums


def parse_tris(text: str) -> list[tuple[int, int, int]]:
    m = re.search(r"k_arkit_tris\[\d+\]\[3\]\s*=\s*\{(.*)\};", text, re.S)
    if not m:
        fail("missing k_arkit_tris")
    nums = [int(x) for x in re.findall(r"\d+", m.group(1))]
    if len(nums) % 3 != 0:
        fail(f"tris not multiple of 3 ({len(nums)})")
    return [(nums[i], nums[i + 1], nums[i + 2]) for i in range(0, len(nums), 3)]


def adjacency(tris: list[tuple[int, int, int]]):
    adj: dict[int, set[int]] = defaultdict(set)
    for a, b, c in tris:
        adj[a] |= {b, c}
        adj[b] |= {a, c}
        adj[c] |= {a, b}
    return adj


def connected(adj, chain: list[int]) -> bool:
    """Every consecutive pair shares an edge, OR chain is a subsequence of a
    path along exclusive-neighbor corridors (allowing skip of degree-2 mids).
    We require each consecutive pair to be within graph distance ≤ 2.
    """
    def dist(a: int, b: int, limit: int = 2) -> int:
        if a == b:
            return 0
        q = deque([(a, 0)])
        seen = {a}
        while q:
            v, d = q.popleft()
            if d == limit:
                continue
            for n in adj[v]:
                if n in seen:
                    continue
                if n == b:
                    return d + 1
                seen.add(n)
                q.append((n, d + 1))
        return 999

    for a, b in zip(chain, chain[1:]):
        if dist(a, b) > 2:
            return False
    return True


def parse_switch_cases(cpp: str) -> dict[int, str]:
    """Return mp_index → return-expression text for arkit_index_for_mp."""
    m = re.search(
        r"static int arkit_index_for_mp\(int mp\)\s*\{(.*?)\n\}",
        cpp,
        re.S,
    )
    if not m:
        fail("arkit_index_for_mp not found")
    body = m.group(1)
    out: dict[int, str] = {}
    for cm in re.finditer(r"case\s+(\d+)\s*:\s*return\s+([^;]+);", body):
        out[int(cm.group(1))] = cm.group(2).strip()
    return out


def main() -> None:
    map_txt = open(MAP_H, encoding="utf-8").read()
    mesh_txt = open(MESH_H, encoding="utf-8").read()
    cpp_txt = open(CPP, encoding="utf-8").read()

    lid_l = parse_int_array(map_txt, "ARKIT_LID_L")
    lid_r = parse_int_array(map_txt, "ARKIT_LID_R")
    low_l = parse_int_array(map_txt, "ARKIT_LOWER_LID_L")
    low_r = parse_int_array(map_txt, "ARKIT_LOWER_LID_R")
    iris_l = parse_int_array(map_txt, "ARKIT_IRIS_L")[0]
    iris_r = parse_int_array(map_txt, "ARKIT_IRIS_R")[0]
    eye_out_l = parse_int_array(map_txt, "ARKIT_EYE_OUT_L")[0]
    eye_out_r = parse_int_array(map_txt, "ARKIT_EYE_OUT_R")[0]

    if len(lid_l) != 7 or len(lid_r) != 7:
        fail(f"upper lid chains must be length 7 (got {len(lid_l)}, {len(lid_r)})")
    if len(low_l) != 9 or len(low_r) != 9:
        fail(f"lower lid chains must be length 9 (got {len(low_l)}, {len(low_r)})")

    # Ends must match corners.
    if lid_l[0] != eye_out_l or lid_r[0] != eye_out_r:
        fail("EYE_OUT_* must equal LID_*[0]")
    if low_l[0] != lid_l[0] or low_l[-1] != lid_l[-1]:
        fail("LOWER_LID_L must share outer/inner with LID_L")
    if low_r[0] != lid_r[0] or low_r[-1] != lid_r[-1]:
        fail("LOWER_LID_R must share outer/inner with LID_R")

    # L/R iris must differ; L/R lids must be disjoint interiors.
    if iris_l == iris_r:
        fail("IRIS_L == IRIS_R")
    if set(lid_l) & set(lid_r):
        fail(f"LID_L and LID_R overlap: {set(lid_l)&set(lid_r)}")
    if set(low_l[1:-1]) & set(low_r[1:-1]):
        fail("lower lid interiors overlap")

    # LID_L must be outer→inner: first != last, and reverse of previous bug
    # (old order started at 1090). Current outer is 1101.
    if lid_l[0] != 1101 or lid_l[-1] != 1090:
        fail(f"LID_L expected outer 1101 → inner 1090, got {lid_l[0]} → {lid_l[-1]}")
    if lid_r[0] != 1069 or lid_r[-1] != 1080:
        fail(f"LID_R expected outer 1069 → inner 1080, got {lid_r[0]} → {lid_r[-1]}")

    tris = parse_tris(mesh_txt)
    if len(tris) != 2304:
        fail(f"expected 2304 tris, got {len(tris)}")
    adj = adjacency(tris)

    for name, chain in (
        ("LID_L", lid_l),
        ("LID_R", lid_r),
        ("LOWER_LID_L", low_l),
        ("LOWER_LID_R", low_r),
    ):
        if not connected(adj, chain):
            fail(f"{name} is not a near-connected chain on the mesh: {chain}")

    # Lower lid interiors should not be a subset of upper lid interiors.
    if set(low_l[1:-1]).issubset(set(lid_l)):
        fail("LOWER_LID_L interior collapsed onto upper lid")
    if set(low_r[1:-1]).issubset(set(lid_r)):
        fail("LOWER_LID_R interior collapsed onto upper lid")

    # Critical MP indices used by face beauty / mesh must hard-map.
    cases = parse_switch_cases(cpp_txt)
    required = {
        # upper lids
        33, 161, 160, 159, 158, 157, 133,
        263, 388, 387, 386, 385, 384, 362,
        # lower lids
        7, 163, 144, 145, 153, 154, 155,
        249, 390, 373, 374, 380, 381, 382,
        # irises / nose / lips / forehead
        468, 473, 1, 10, 0, 13, 14, 17, 61, 291,
    }
    missing = sorted(i for i in required if i not in cases)
    if missing:
        fail(f"arkit_index_for_mp missing cases: {missing}")

    # Lower-lid cases must reference LOWER_LID arrays (not upper).
    # MP indices 7,163,144,145,153,154,155 are image-LEFT (low U) = person's
    # RIGHT, so they map via ARKIT_LOWER_LID_R.
    # MP indices 249,390,373,374,380,381,382 are image-RIGHT (high U) =
    # person's LEFT, so they map via ARKIT_LOWER_LID_L.
    for mp in (7, 163, 144, 145, 153, 154, 155):
        if "LOWER_LID_R" not in cases[mp]:
            fail(f"MP {mp} should map via ARKIT_LOWER_LID_R (image-left=person R), got {cases[mp]}")
    for mp in (249, 390, 373, 374, 380, 381, 382):
        if "LOWER_LID_L" not in cases[mp]:
            fail(f"MP {mp} should map via ARKIT_LOWER_LID_L (image-right=person L), got {cases[mp]}")

    # Contract comment present.
    if "UNMIRRORED" not in map_txt and "unmirrored" not in map_txt:
        fail("landmark map must document unmirrored ARKit preview contract")

    print(
        "OK: ARKit landmark map — "
        f"LID_L {lid_l[0]}→{lid_l[-1]}, LID_R {lid_r[0]}→{lid_r[-1]}, "
        f"lower lids hard-mapped, {len(required)} critical MP indices present."
    )


if __name__ == "__main__":
    main()
