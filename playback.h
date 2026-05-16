#pragma once

#include "vec.h"
#include "quat.h"
#include "array.h"
#include "spring.h"
#include "common.h"

// Copy a part of a feature vector from the 
// matching database into the query feature vector
void query_copy_denormalized_feature(
    slice1d<float> query, 
    int& offset, 
    const int size, 
    const slice1d<float> features,
    const slice1d<float> features_offset,
    const slice1d<float> features_scale);

// Copy from a specific source offset in a feature vector into the current query offset.
void query_copy_denormalized_feature_from_source_offset(
    slice1d<float> query,
    int& dst_offset,
    const int size,
    const int src_offset,
    const slice1d<float> features,
    const slice1d<float> features_offset,
    const slice1d<float> features_scale);

// Compute the query feature vector for the current 
// trajectory controlled by the gamepad.
void query_compute_trajectory_position_feature(
    slice1d<float> query, 
    int& offset, 
    const vec3 root_position, 
    const quat root_rotation, 
    const slice1d<vec3> trajectory_positions);

// Same but for the trajectory direction
void query_compute_trajectory_direction_feature(
    slice1d<float> query, 
    int& offset, 
    const vec3 root_position,
    const quat root_rotation, 
    const slice1d<vec3> trajectory_positions,
    const slice1d<quat> trajectory_rotations);

// Add terrain height features to query
void query_compute_terrain_height_feature(
    slice1d<float> query,
    int& offset,
    const slice1d<vec2> future_terrain_heights);

vec3 adjust_character_position(
    const vec3 character_position,
    const vec3 simulation_position,
    const float halflife,
    const float dt);

quat adjust_character_rotation(
    const quat character_rotation,
    const quat simulation_rotation,
    const float halflife,
    const float dt);

vec3 adjust_character_position_by_velocity(
    const vec3 character_position,
    const vec3 character_velocity,
    const vec3 simulation_position,
    const float max_adjustment_ratio,
    const float halflife,
    const float dt);

quat adjust_character_rotation_by_velocity(
    const quat character_rotation,
    const vec3 character_angular_velocity,
    const quat simulation_rotation,
    const float max_adjustment_ratio,
    const float halflife,
    const float dt);

vec3 clamp_character_position(
    const vec3 character_position,
    const vec3 simulation_position,
    const float max_distance);
  
quat clamp_character_rotation(
    const quat character_rotation,
    const quat simulation_rotation,
    const float max_angle);
