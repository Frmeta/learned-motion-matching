#pragma once

#include "vec.h"
#include "quat.h"
#include "raylib.h"

enum
{
    GAMEPAD_PLAYER = 0,
};

enum
{
    GAMEPAD_STICK_LEFT,
    GAMEPAD_STICK_RIGHT,
};

vec3 gamepad_get_stick(int stick, const float deadzone = 0.2f);

float orbit_camera_update_azimuth(
    const float azimuth, 
    const vec3 gamepadstick_right,
    const bool desired_strafe,
    const float dt);

float orbit_camera_update_altitude(
    const float altitude, 
    const vec3 gamepadstick_right,
    const bool desired_strafe,
    const float dt);

float orbit_camera_update_distance(
    const float distance, 
    const float dt);

void orbit_camera_update(
    Camera3D& cam, 
    float& camera_azimuth,
    float& camera_altitude,
    float& camera_distance,
    const vec3 target,
    const vec3 gamepadstick_right,
    const bool desired_strafe,
    const float dt);

bool desired_strafe_update();

void desired_gait_update(
    float& desired_gait, 
    float& desired_gait_velocity,
    const bool desired_walk,
    const float dt,
    const float gait_change_halflife = 0.1f);

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
