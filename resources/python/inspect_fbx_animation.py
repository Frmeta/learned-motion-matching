"""Inspect FBX animation and armature data inside Blender."""

from __future__ import annotations

import argparse
import os
import sys

try:
    import bpy  # type: ignore
    from mathutils import Matrix  # type: ignore
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


def print_matrix(label, matrix):
    print(label)
    for row in matrix:
        print("  ", [round(v, 6) for v in row])


def main(argv):
    args = parse_args(argv)
    path = os.path.abspath(args.input)
    if not os.path.exists(path):
        raise FileNotFoundError(path)

    import_fbx(path)

    print(f"FBX: {path}")
    print("Objects:")
    for obj in bpy.data.objects:
        print(f"  {obj.name} | type={obj.type} | scale={tuple(round(v, 6) for v in obj.scale)} | location={tuple(round(v, 6) for v in obj.location)}")

    armatures = [obj for obj in bpy.data.objects if obj.type == "ARMATURE"]
    meshes = [obj for obj in bpy.data.objects if obj.type == "MESH"]

    print(f"Armatures: {len(armatures)}")
    for armature in armatures:
        print(f"Armature: {armature.name} | scale={tuple(round(v, 6) for v in armature.scale)} | matrix_world=")
        print_matrix("", armature.matrix_world)
        print("Bones:")
        for bone in armature.data.bones:
            parent = bone.parent.name if bone.parent else None
            print(
                f"  {bone.name} | parent={parent} | head={tuple(round(v, 6) for v in bone.head_local)} | tail={tuple(round(v, 6) for v in bone.tail_local)}"
            )

    print(f"Meshes: {len(meshes)}")
    for mesh in meshes:
        print(f"Mesh: {mesh.name} | verts={len(mesh.data.vertices)} | polys={len(mesh.data.polygons)} | vgroups={len(mesh.vertex_groups)}")
        if mesh.animation_data and mesh.animation_data.action:
            action = mesh.animation_data.action
            print(f"  Mesh action: {action.name} | frame_range={action.frame_range}")

    print(f"Actions: {len(bpy.data.actions)}")
    for action in bpy.data.actions:
        print(f"Action: {action.name} | frame_range={action.frame_range} | fcurves={len(action.fcurves)}")
        for fcurve in action.fcurves[:12]:
            print(f"  {fcurve.data_path}[{fcurve.array_index}] | keys={len(fcurve.keyframe_points)} | range=({fcurve.range()[0]:.3f}, {fcurve.range()[1]:.3f})")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else sys.argv[1:]))
