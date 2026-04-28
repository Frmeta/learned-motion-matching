"""Export a rigged Blender mesh to the Motion Matching character.bin format.

Run this inside Blender, for example:

    blender --background --python export_character.py -- \
        --input path/to/model.fbx \
        --output resources/bin/custom_character.bin

The output file matches character_load() exactly:
    positions, normals, texcoords, triangles, bone_weights, bone_indices,
    bone_rest_positions, bone_rest_rotations

The exporter expects the same 23-bone character hierarchy used by the
controller/database code. If the rig already contains those bone names,
they are used directly. Bone_Entity can be synthesized when the source rig
does not have an explicit root bone.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
from dataclasses import dataclass
from typing import Dict, Iterable, List, Sequence, Tuple

try:
    import bpy  # type: ignore
    from mathutils import Matrix, Vector  # type: ignore
except Exception as exc:  # pragma: no cover - Blender-only runtime guard.
    raise SystemExit(
        "This script must be run inside Blender so bpy is available."
    ) from exc


EXPECTED_BONES: List[str] = [
    "Bone_Entity",
    "Bone_Hips",
    "Bone_LeftUpLeg",
    "Bone_LeftLeg",
    "Bone_LeftFoot",
    "Bone_LeftToe",
    "Bone_RightUpLeg",
    "Bone_RightLeg",
    "Bone_RightFoot",
    "Bone_RightToe",
    "Bone_Spine",
    "Bone_Spine1",
    "Bone_Spine2",
    "Bone_Neck",
    "Bone_Head",
    "Bone_LeftShoulder",
    "Bone_LeftArm",
    "Bone_LeftForeArm",
    "Bone_LeftHand",
    "Bone_RightShoulder",
    "Bone_RightArm",
    "Bone_RightForeArm",
    "Bone_RightHand",
]

# Canonical parent layout for the fixed 23-bone skeleton.
CANONICAL_PARENTS: List[int] = [
    -1,  # Bone_Entity
    0,   # Bone_Hips
    1,   # Bone_LeftUpLeg
    2,   # Bone_LeftLeg
    3,   # Bone_LeftFoot
    4,   # Bone_LeftToe
    1,   # Bone_RightUpLeg
    6,   # Bone_RightLeg
    7,   # Bone_RightFoot
    8,   # Bone_RightToe
    1,   # Bone_Spine
    10,  # Bone_Spine1
    11,  # Bone_Spine2
    12,  # Bone_Neck
    13,  # Bone_Head
    12,  # Bone_LeftShoulder
    15,  # Bone_LeftArm
    16,  # Bone_LeftForeArm
    17,  # Bone_LeftHand
    12,  # Bone_RightShoulder
    19,  # Bone_RightArm
    20,  # Bone_RightForeArm
    21,  # Bone_RightHand
]

SKIN_INFLUENCE_COUNT = 4


@dataclass
class MeshVertex:
    position: Tuple[float, float, float]
    normal: Tuple[float, float, float]
    texcoord: Tuple[float, float]
    weights: Tuple[float, float, float, float]
    indices: Tuple[int, int, int, int]


def _canonical_bone_aliases(expected_name: str) -> List[str]:
    stem = expected_name.replace("Bone_", "")
    aliases = [
        expected_name,
        stem,
        stem.lower(),
        stem.upper(),
        stem.capitalize(),
        f"mixamorig:{stem}",
        f"mixamo:{stem}",
    ]

    if expected_name == "Bone_Entity":
        aliases.extend([
            "Root",
            "root",
            "Armature",
            "ArmatureRoot",
        ])

    # Preserve order while removing duplicates.
    unique_aliases: List[str] = []
    seen = set()
    for alias in aliases:
        if alias not in seen:
            seen.add(alias)
            unique_aliases.append(alias)
    return unique_aliases


def _normalized_bone_name(name: str) -> str:
    normalized = name
    for separator in (":", "|", "/"):
        if separator in normalized:
            normalized = normalized.split(separator)[-1]
    return normalized.lower()


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Export a rigged mesh to character.bin")
    parser.add_argument("--input", default="", help="FBX/Blender file to import before export")
    parser.add_argument("--output", required=True, help="Output .bin file path")
    parser.add_argument("--armature", default="", help="Optional armature object name")
    parser.add_argument("--mesh", default="", help="Optional mesh object name")
    return parser.parse_args(list(argv))


def _clear_scene() -> None:
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)


def _import_input_model(input_path: str) -> None:
    if not input_path:
        return

    if not os.path.exists(input_path):
        raise FileNotFoundError(f"Input model not found: {input_path}")

    ext = os.path.splitext(input_path)[1].lower()
    _clear_scene()

    if ext == ".fbx":
        bpy.ops.import_scene.fbx(filepath=input_path)
    else:
        raise ValueError(f"Unsupported input format: {ext or '<none>'}. Use .fbx.")


def _find_object_by_name(name: str, object_type: str):
    if not name:
        return None

    obj = bpy.data.objects.get(name)
    if obj is None:
        raise ValueError(f"Object not found: {name}")
    if obj.type != object_type:
        raise ValueError(f"Object '{name}' is type {obj.type}, expected {object_type}")
    return obj


def _find_armature_object(explicit_name: str = ""):
    if explicit_name:
        return _find_object_by_name(explicit_name, "ARMATURE")

    for obj in bpy.data.objects:
        if obj.type == "ARMATURE":
            return obj
    raise ValueError("No armature object found in the scene")


def _find_mesh_object(explicit_name: str = "", armature_obj=None):
    if explicit_name:
        return _find_object_by_name(explicit_name, "MESH")

    candidates = []
    for obj in bpy.data.objects:
        if obj.type != "MESH":
            continue
        score = 0
        for modifier in obj.modifiers:
            if modifier.type == "ARMATURE" and modifier.object == armature_obj:
                score += 10
        if obj.parent == armature_obj:
            score += 5
        candidates.append((score, obj))

    if not candidates:
        raise ValueError("No mesh object found in the scene")

    candidates.sort(key=lambda item: (-item[0], item[1].name))
    return candidates[0][1]


def _normalize_vector3(value: Vector) -> Tuple[float, float, float]:
    return (float(value.x), float(value.y), float(value.z))


def _normalize_vector2(value: Vector) -> Tuple[float, float]:
    return (float(value.x), float(value.y))


def _resolve_bone_map(armature_obj) -> Dict[str, object]:
    bones = armature_obj.data.bones
    resolved: Dict[str, object] = {}
    normalized_lookup = { _normalized_bone_name(bone.name): bone for bone in bones }

    for expected_name in EXPECTED_BONES:
        source_bone = None
        for candidate in _canonical_bone_aliases(expected_name):
            source_bone = bones.get(candidate)
            if source_bone is not None:
                break

            source_bone = normalized_lookup.get(_normalized_bone_name(candidate))
            if source_bone is not None:
                break

        if source_bone is None:
            if expected_name == "Bone_Entity":
                resolved[expected_name] = None
                continue
            raise ValueError(
                f"Missing required bone '{expected_name}'. The source rig must match the project skeleton."
            )

        resolved[expected_name] = source_bone

    return resolved


def _canonical_bone_parent_index(bone_index: int) -> int:
    return CANONICAL_PARENTS[bone_index]


def _get_canonical_local_matrix(armature_obj, bone_map: Dict[str, object], bone_index: int) -> Matrix:
    bone_name = EXPECTED_BONES[bone_index]
    source_bone = bone_map[bone_name]

    if source_bone is None:
        return Matrix.Identity(4)

    parent_index = _canonical_bone_parent_index(bone_index)
    if parent_index < 0:
        return source_bone.matrix_local.copy()

    parent_name = EXPECTED_BONES[parent_index]
    parent_bone = bone_map.get(parent_name)

    if parent_bone is None:
        return source_bone.matrix_local.copy()

    return parent_bone.matrix_local.inverted() @ source_bone.matrix_local


def _extract_vertex_weights(mesh, bone_name_to_index: Dict[str, int]) -> Tuple[List[Tuple[int, int, int, int]], List[Tuple[float, float, float, float]]]:
    indices_per_vertex: List[Tuple[int, int, int, int]] = []
    weights_per_vertex: List[Tuple[float, float, float, float]] = []

    for vertex in mesh.vertices:
        pairs: List[Tuple[int, float]] = []
        for group in vertex.groups:
            group_name = mesh.vertex_groups[group.group].name
            bone_index = bone_name_to_index.get(group_name)
            if bone_index is None:
                continue
            if group.weight <= 0.0:
                continue
            pairs.append((bone_index, float(group.weight)))

        pairs.sort(key=lambda item: (-item[1], item[0]))
        pairs = pairs[:SKIN_INFLUENCE_COUNT]

        if not pairs:
            pairs = [(0, 1.0)]

        total_weight = sum(weight for _, weight in pairs)
        if total_weight <= 0.0:
            pairs = [(0, 1.0)]
            total_weight = 1.0

        normalized_pairs = [(bone_index, weight / total_weight) for bone_index, weight in pairs]
        while len(normalized_pairs) < SKIN_INFLUENCE_COUNT:
            normalized_pairs.append((0, 0.0))

        indices_per_vertex.append(tuple(int(bone_index) for bone_index, _ in normalized_pairs))
        weights_per_vertex.append(tuple(float(weight) for _, weight in normalized_pairs))

    return indices_per_vertex, weights_per_vertex


def _extract_mesh_vertices(mesh_obj, armature_obj, bone_map: Dict[str, object]) -> Tuple[List[MeshVertex], List[int]]:
    depsgraph = bpy.context.evaluated_depsgraph_get()
    evaluated_obj = mesh_obj.evaluated_get(depsgraph)
    evaluated_mesh = evaluated_obj.to_mesh(preserve_all_data_layers=True, depsgraph=depsgraph)

    try:
        evaluated_mesh.calc_loop_triangles()
        evaluated_mesh.calc_normals_split()

        mesh_to_armature = armature_obj.matrix_world.inverted() @ mesh_obj.matrix_world
        normal_matrix = mesh_to_armature.to_3x3().inverted().transposed()

        uv_layer = evaluated_mesh.uv_layers.active
        bone_name_to_index = {name: index for index, name in enumerate(EXPECTED_BONES)}
        vertex_indices, vertex_weights = _extract_vertex_weights(mesh_obj.data, bone_name_to_index)

        if len(vertex_indices) != len(evaluated_mesh.vertices):
            raise ValueError(
                f"Vertex count mismatch after evaluation: {len(evaluated_mesh.vertices)} evaluated vs {len(vertex_indices)} source vertices"
            )

        vertices: List[MeshVertex] = []
        triangles: List[int] = []

        for loop_triangle in evaluated_mesh.loop_triangles:
            tri_indices: List[int] = []
            for corner, loop_index in enumerate(loop_triangle.loops):
                loop = evaluated_mesh.loops[loop_index]
                source_vertex_index = loop.vertex_index
                source_vertex = evaluated_mesh.vertices[source_vertex_index]

                position = mesh_to_armature @ source_vertex.co
                normal = normal_matrix @ loop.normal
                if normal.length_squared > 0.0:
                    normal.normalize()
                else:
                    normal = Vector((0.0, 1.0, 0.0))

                if uv_layer is not None:
                    texcoord = uv_layer.data[loop_index].uv.copy()
                else:
                    texcoord = Vector((0.0, 0.0))

                weights = vertex_weights[source_vertex_index]
                indices = vertex_indices[source_vertex_index]

                vertices.append(
                    MeshVertex(
                        position=_normalize_vector3(position),
                        normal=_normalize_vector3(normal),
                        texcoord=_normalize_vector2(texcoord),
                        weights=weights,
                        indices=indices,
                    )
                )
                tri_indices.append(len(vertices) - 1)

            if len(tri_indices) == 3:
                triangles.extend(tri_indices)

        return vertices, triangles
    finally:
        evaluated_obj.to_mesh_clear()


def _extract_bone_data(bone_map: Dict[str, object]) -> Tuple[List[Tuple[float, float, float]], List[Tuple[float, float, float, float]]]:
    rest_positions: List[Tuple[float, float, float]] = []
    rest_rotations: List[Tuple[float, float, float, float]] = []

    for bone_index, bone_name in enumerate(EXPECTED_BONES):
        source_bone = bone_map[bone_name]

        if source_bone is None:
            rest_positions.append((0.0, 0.0, 0.0))
            rest_rotations.append((1.0, 0.0, 0.0, 0.0))
            continue

        local_matrix = _get_canonical_local_matrix(None, bone_map, bone_index)
        location, rotation, scale = local_matrix.decompose()

        if max(abs(scale.x - 1.0), abs(scale.y - 1.0), abs(scale.z - 1.0)) > 1e-3:
            print(
                f"Warning: bone '{bone_name}' has rest scale {tuple(scale)}; scale is ignored by character.bin.",
                file=sys.stderr,
            )

        rest_positions.append(_normalize_vector3(location))
        rest_rotations.append((float(rotation.w), float(rotation.x), float(rotation.y), float(rotation.z)))

    return rest_positions, rest_rotations


def _write_array1d(file_handle, values: Sequence, type_format: str) -> None:
    file_handle.write(struct.pack("<i", len(values)))
    for value in values:
        if isinstance(value, tuple):
            file_handle.write(struct.pack("<" + type_format, *value))
        else:
            file_handle.write(struct.pack("<" + type_format, value))


def _write_array2d(file_handle, rows: Sequence[Sequence], type_format: str) -> None:
    row_count = len(rows)
    col_count = len(rows[0]) if row_count else 0
    file_handle.write(struct.pack("<ii", row_count, col_count))
    for row in rows:
        if len(row) != col_count:
            raise ValueError("All rows must have the same length")
        for value in row:
            if isinstance(value, tuple):
                file_handle.write(struct.pack("<" + type_format, *value))
            else:
                file_handle.write(struct.pack("<" + type_format, value))


def _write_character_bin(output_path: str, vertices: List[MeshVertex], triangles: List[int], rest_positions, rest_rotations) -> None:
    if len(vertices) > 65535:
        raise ValueError(
            f"Exported mesh has {len(vertices)} vertices, which exceeds the unsigned short index limit."
        )

    positions = [vertex.position for vertex in vertices]
    normals = [vertex.normal for vertex in vertices]
    texcoords = [vertex.texcoord for vertex in vertices]
    bone_weights = [vertex.weights for vertex in vertices]
    bone_indices = [vertex.indices for vertex in vertices]

    with open(output_path, "wb") as file_handle:
        _write_array1d(file_handle, positions, "fff")
        _write_array1d(file_handle, normals, "fff")
        _write_array1d(file_handle, texcoords, "ff")
        _write_array1d(file_handle, triangles, "H")
        _write_array2d(file_handle, bone_weights, "f")
        _write_array2d(file_handle, bone_indices, "H")
        _write_array1d(file_handle, rest_positions, "fff")
        _write_array1d(file_handle, rest_rotations, "ffff")


def main(argv: Sequence[str]) -> int:
    args = _parse_args(argv)

    _import_input_model(args.input)

    armature_obj = _find_armature_object(args.armature)
    mesh_obj = _find_mesh_object(args.mesh, armature_obj)
    bone_map = _resolve_bone_map(armature_obj)

    vertices, triangles = _extract_mesh_vertices(mesh_obj, armature_obj, bone_map)
    rest_positions, rest_rotations = _extract_bone_data(bone_map)

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    _write_character_bin(args.output, vertices, triangles, rest_positions, rest_rotations)

    print(f"Exported {len(vertices)} vertices, {len(triangles) // 3} triangles, {len(rest_positions)} bones")
    print(f"Wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else sys.argv[1:]))