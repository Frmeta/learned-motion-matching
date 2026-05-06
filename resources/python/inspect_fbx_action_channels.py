"""Summarize animation channels inside an imported FBX."""

from __future__ import annotations

import argparse
import collections
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

    action_counts = collections.Counter()
    bone_counts = collections.Counter()
    first_curves = []

    for action in bpy.data.actions:
        print(f"Action: {action.name}")
        for fcurve in action.fcurves:
            path_str = fcurve.data_path
            if path_str.startswith('pose.bones['):
                bone_name = path_str.split('"')[1]
                bone_counts[bone_name] += 1
                if ".location" in path_str:
                    action_counts["bone_location"] += 1
                elif ".rotation_quaternion" in path_str:
                    action_counts["bone_rotation_quaternion"] += 1
                elif ".rotation_euler" in path_str:
                    action_counts["bone_rotation_euler"] += 1
                elif ".scale" in path_str:
                    action_counts["bone_scale"] += 1
            elif path_str.startswith("location"):
                action_counts["object_location"] += 1
            elif path_str.startswith("rotation"):
                action_counts["object_rotation"] += 1
            elif path_str.startswith("scale"):
                action_counts["object_scale"] += 1
            elif path_str.startswith("lens"):
                action_counts["camera_lens"] += 1
            elif path_str.startswith("dof."):
                action_counts["camera_dof"] += 1

            if len(first_curves) < 20:
                first_curves.append(path_str)

        print(f"  frame_range={action.frame_range} fcurves={len(action.fcurves)}")

    print("Channel summary:")
    for key, value in sorted(action_counts.items()):
        print(f"  {key}: {value}")

    print("Most animated bones:")
    for bone_name, count in bone_counts.most_common(20):
        print(f"  {bone_name}: {count}")

    print("Sample channels:")
    for item in first_curves:
        print(f"  {item}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else sys.argv[1:]))
