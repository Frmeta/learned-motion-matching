#pragma once

#include "vec.h"
#include "quat.h"
#include "array.h"
#include "raylib.h"

// Forward declarations for functions kept in controller.cpp
bool sample_terrain_height(
    const Model& ground_plane_model,
    const vec3 position,
    float& out_height);

float orbit_camera_update_azimuth(
    const float azimuth, 
    const vec3 gamepadstick_right,
    const bool desired_strafe,
    const float dt);

vec3 desired_velocity_update(
    const vec3 gamepadstick_left,
    const float camera_azimuth,
    const quat simulation_rotation,
    const float fwrd_speed,
    const float side_speed,
    const float back_speed);

quat desired_rotation_update(
    const quat desired_rotation,
    const vec3 gamepadstick_left,
    const vec3 gamepadstick_right,
    const float camera_azimuth,
    const bool desired_strafe,
    const vec3 desired_velocity);

// Trajectory and Simulation
void simulation_positions_update(
    vec3& position, 
    vec3& velocity, 
    vec3& acceleration, 
    const vec3 desired_velocity, 
    const float halflife, 
    const float dt,
    const Model& obstacle_model);

void simulation_rotations_update(
    quat& rotation, 
    vec3& angular_velocity, 
    const quat desired_rotation, 
    const float halflife, 
    const float dt);

void trajectory_desired_velocities_predict(
  slice1d<vec3> desired_velocities,
  const slice1d<quat> trajectory_rotations,
  const vec3 desired_velocity,
    const vec3 simulation_position,
    const Model& ground_plane_model,
    const bool jump_active,
    const float jump_vertical_velocity,
    const float jump_gravity,
    const float jump_root_height_offset,
  const float camera_azimuth,
  const vec3 gamepadstick_left,
  const vec3 gamepadstick_right,
  const bool desired_strafe,
  const float fwrd_speed,
  const float side_speed,
  const float back_speed,
  const float jump_speed_boost,
  const float jump_gait_timer,
  const float jump_gait_hold_time,
  const float dt);

void trajectory_positions_predict(
    slice1d<vec3> positions, 
    slice1d<vec3> velocities, 
    slice1d<vec3> accelerations, 
    const vec3 position, 
    const vec3 velocity, 
    const vec3 acceleration, 
    const slice1d<vec3> desired_velocities, 
    const float halflife,
    const float dt,
    const Model& obstacle_model);

void trajectory_desired_rotations_predict(
  slice1d<quat> desired_rotations,
  const slice1d<vec3> desired_velocities,
  const quat desired_rotation,
  const float camera_azimuth,
  const vec3 gamepadstick_left,
  const vec3 gamepadstick_right,
  const bool desired_strafe,
  const float dt);

void trajectory_rotations_predict(
    slice1d<quat> rotations, 
    slice1d<vec3> angular_velocities, 
    const quat rotation, 
    const vec3 angular_velocity, 
    const slice1d<quat> desired_rotations, 
    const float halflife,
    const float dt);
