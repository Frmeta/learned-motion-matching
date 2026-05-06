"""Print value ranges for key FBX animation channels."""

from __future__ import annotations

import argparse
import os
import sys
from math import inf

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

    stats = {}

    for action in bpy.data.actions:
        for fcurve in action.fcurves:
            path_str = fcurve.data_path
            if not path_str.startswith('pose.bones['):
                continue
            bone_name = path_str.split('"')[1]
            if ".location" in path_str:
                key = (bone_name, "location")
            elif ".rotation_quaternion" in path_str:
                key = (bone_name, "rotation_quaternion")
            elif ".scale" in path_str:
                key = (bone_name, "scale")
            else:
                continue

            values = [pt.co[1] for pt in fcurve.keyframe_points]
            if not values:
                continue
            cur = stats.get(key, [inf, -inf])
            cur[0] = min(cur[0], min(values))
            cur[1] = max(cur[1], max(values))
            stats[key] = cur

    print("Channel ranges (min/max across keyframes):")
    for (bone_name, kind), (min_v, max_v) in sorted(stats.items(), key=lambda item: (item[0][0], item[0][1])):
        print(f"  {bone_name:24s} {kind:20s} {min_v: .6f} .. {max_v: .6f}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else sys.argv[1:]))
