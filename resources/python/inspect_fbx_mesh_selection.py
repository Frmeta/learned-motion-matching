"""Print how the exporter would choose a mesh object from the FBX."""

from __future__ import annotations

import argparse
import os
import sys

try:
    import bpy  # type: ignore
except Exception as exc:
    raise SystemExit("This script must be run inside Blender.") from exc


def parse_args(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    return parser.parse_args(argv)


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)


def import_fbx(path: str):
    clear_scene()
    bpy.ops.import_scene.fbx(filepath=path)


def main(argv):
    args = parse_args(argv)
    path = os.path.abspath(args.input)
    import_fbx(path)

    armature = next((obj for obj in bpy.data.objects if obj.type == "ARMATURE"), None)
    if armature is None:
        print("No armature found")
        return 1

    candidates = []
    for obj in bpy.data.objects:
        if obj.type != "MESH":
            continue
        score = 0
        arm_mods = 0
        for modifier in obj.modifiers:
            if modifier.type == "ARMATURE" and modifier.object == armature:
                score += 10
                arm_mods += 1
        if obj.parent == armature:
            score += 5
        candidates.append((score, obj.name, len(obj.data.vertices), len(obj.data.polygons), len(obj.vertex_groups), arm_mods))

    candidates.sort(key=lambda item: (-item[0], item[1]))

    print(f"Armature: {armature.name}")
    print("Mesh candidates (sorted the way exporter would pick them):")
    for score, name, verts, polys, vgroups, arm_mods in candidates:
        print(f"  score={score:2d} | {name:20s} | verts={verts:4d} | polys={polys:4d} | vgroups={vgroups:2d} | armature_modifiers={arm_mods}")

    if candidates:
        chosen = candidates[0]
        print(f"Chosen mesh: {chosen[1]}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else sys.argv[1:]))
