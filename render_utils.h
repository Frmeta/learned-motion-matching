#pragma once

#include "raylib.h"
#include "vec.h"
#include "quat.h"
#include "array.h"
#include "character.h"
#include <vector>

struct feature_draw_data
{
    array1d<float> features;            // denormalized feature vector
    vec3           root_pos;            // global_bone_positions(0)
    quat           root_rot;            // global_bone_rotations(0)
    vec3           hip_pos;             // global_bone_positions(Bone_Hips)
    array1d<vec3>  bone_positions;      // full global_bone_positions copy
    array1d<int>   contact_bones;       // indices of bones used for contact-plane visuals

    array1d<vec2>  future_toe_position;    // 6 vec2 (3 future frames x 2 toes)
    array1d<vec2>  future_terrain_heights; // 4 vec2

    // Rolling history buffers as-of this frame (typically <=30 elements each)
    std::vector<vec3> root_history_positions;
    std::vector<quat> root_history_rotations;
    std::vector<vec3> history_left_foot_positions;
    std::vector<vec3> history_right_foot_positions;
    std::vector<vec3> history_left_foot_velocities;
    std::vector<vec3> history_right_foot_velocities;
    std::vector<vec3> history_hip_positions;
    std::vector<vec3> history_hip_velocities;
    std::vector<vec2> history_terrain_heights;
};

void deform_character_mesh(
  Mesh& mesh, 
  const character& c,
  const slice1d<vec3> bone_anim_positions,
  const slice1d<quat> bone_anim_rotations,
  const slice1d<int> bone_parents);

Mesh make_character_mesh(character& c);

void draw_axis(const vec3 pos, const quat rot, const float scale = 1.0f);

void draw_features(const feature_draw_data& f, const vec3 pos, const quat rot);

void draw_trajectory(
    const slice1d<vec3> trajectory_positions, 
    const slice1d<quat> trajectory_rotations, 
    const Color color);

void draw_stickman(
    const slice1d<vec3> global_bone_positions,
    const slice1d<int> bone_parents,
    const Color color);
