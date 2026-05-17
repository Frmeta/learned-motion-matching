#include "input.h"
#include "spring.h"
#include "common.h"
#include <cmath>

static float keyboard_axis(int negative_key, int positive_key)
{
    return (IsKeyDown(positive_key) ? 1.0f : 0.0f) - (IsKeyDown(negative_key) ? 1.0f : 0.0f);
}

static vec3 keyboard_get_stick(int stick)
{
    float keyboardx = 0.0f;
    float keyboardy = 0.0f;

    if (stick == GAMEPAD_STICK_LEFT)
    {
        // WASD drives character movement.
        keyboardx = keyboard_axis(KEY_A, KEY_D);
        keyboardy = keyboard_axis(KEY_W, KEY_S);
    }
    else
    {
        // Arrow keys emulate right stick for camera/facing.
        keyboardx = keyboard_axis(KEY_LEFT, KEY_RIGHT);
        keyboardy = keyboard_axis(KEY_UP, KEY_DOWN);
    }

    if (keyboardx != 0.0f || keyboardy != 0.0f)
    {
        float mag = sqrtf(keyboardx * keyboardx + keyboardy * keyboardy);
        if (mag > 1.0f)
        {
            keyboardx /= mag;
            keyboardy /= mag;
        }
    }

    return vec3(keyboardx, 0.0f, keyboardy);
}

vec3 gamepad_get_stick(int stick, const float deadzone)
{
    float gamepadx = GetGamepadAxisMovement(GAMEPAD_PLAYER, stick == GAMEPAD_STICK_LEFT ? GAMEPAD_AXIS_LEFT_X : GAMEPAD_AXIS_RIGHT_X);
    float gamepady = GetGamepadAxisMovement(GAMEPAD_PLAYER, stick == GAMEPAD_STICK_LEFT ? GAMEPAD_AXIS_LEFT_Y : GAMEPAD_AXIS_RIGHT_Y);
    float gamepadmag = sqrtf(gamepadx*gamepadx + gamepady*gamepady);
    
    if (gamepadmag > deadzone)
    {
        float gamepaddirx = gamepadx / gamepadmag;
        float gamepaddiry = gamepady / gamepadmag;
        float gamepadclippedmag = gamepadmag > 1.0f ? 1.0f : gamepadmag*gamepadmag;
        gamepadx = gamepaddirx * gamepadclippedmag;
        gamepady = gamepaddiry * gamepadclippedmag;
    }
    else
    {
        gamepadx = 0.0f;
        gamepady = 0.0f;
    }

    vec3 keyboard_stick = keyboard_get_stick(stick);
    gamepadx += keyboard_stick.x;
    gamepady += keyboard_stick.z;

    float merged_mag = sqrtf(gamepadx * gamepadx + gamepady * gamepady);
    if (merged_mag > 1.0f)
    {
        gamepadx /= merged_mag;
        gamepady /= merged_mag;
    }
    
    return vec3(gamepadx, 0.0f, gamepady);
}

float orbit_camera_update_azimuth(
    const float azimuth, 
    const vec3 gamepadstick_right,
    const bool desired_strafe,
    const float dt)
{
    vec3 gamepadaxis = desired_strafe ? vec3() : gamepadstick_right;
    return azimuth + 2.0f * dt * -gamepadaxis.x;
}

float orbit_camera_update_altitude(
    const float altitude, 
    const vec3 gamepadstick_right,
    const bool desired_strafe,
    const float dt)
{
    vec3 gamepadaxis = desired_strafe ? vec3() : gamepadstick_right;
    return clampf(altitude + 2.0f * dt * gamepadaxis.z, 0.0, 0.4f * PIf);
}

float orbit_camera_update_distance(
    const float distance, 
    const float dt)
{
    float gamepadzoom = 
        IsGamepadButtonDown(GAMEPAD_PLAYER, GAMEPAD_BUTTON_LEFT_TRIGGER_1)  ? +1.0f :
        IsGamepadButtonDown(GAMEPAD_PLAYER, GAMEPAD_BUTTON_RIGHT_TRIGGER_1) ? -1.0f : 0.0f;

    float keyboard_zoom = keyboard_axis(KEY_E, KEY_Q);
    float zoom_input = clampf(gamepadzoom + keyboard_zoom, -1.0f, 1.0f);
        
    return clampf(distance +  10.0f * dt * zoom_input, 0.1f, 100.0f);
}

void orbit_camera_update(
    Camera3D& cam, 
    float& camera_azimuth,
    float& camera_altitude,
    float& camera_distance,
    const vec3 target,
    const vec3 gamepadstick_right,
    const bool desired_strafe,
    const float dt)
{
    camera_azimuth = orbit_camera_update_azimuth(camera_azimuth, gamepadstick_right, desired_strafe, dt);
    camera_altitude = orbit_camera_update_altitude(camera_altitude, gamepadstick_right, desired_strafe, dt);
    camera_distance = orbit_camera_update_distance(camera_distance, dt);
    
    quat rotation_azimuth = quat_from_angle_axis(camera_azimuth, vec3(0, 1, 0));
    vec3 position = quat_mul_vec3(rotation_azimuth, vec3(0, 0, camera_distance));
    vec3 axis = normalize(cross(position, vec3(0, 1, 0)));
    
    quat rotation_altitude = quat_from_angle_axis(camera_altitude, axis);
    
    vec3 eye = target + quat_mul_vec3(rotation_altitude, position);

    cam.target = (Vector3){ target.x, target.y, target.z };
    cam.position = (Vector3){ eye.x, eye.y, eye.z };
}

bool desired_strafe_update()
{
    return IsGamepadButtonDown(GAMEPAD_PLAYER, GAMEPAD_BUTTON_LEFT_TRIGGER_2) > 0.5f ||
           IsKeyDown(KEY_H);
}

void desired_gait_update(
    float& desired_gait, 
    float& desired_gait_velocity,
    const bool desired_walk,
    const float dt,
    const float gait_change_halflife)
{
    float gait_target = desired_walk ? 1.0f : 0.0f;

    simple_spring_damper_exact(
        desired_gait, 
        desired_gait_velocity,
        gait_target,
        gait_change_halflife,
        dt);
}

vec3 desired_velocity_update(
    const vec3 gamepadstick_left,
    const float camera_azimuth,
    const quat simulation_rotation,
    const float fwrd_speed,
    const float side_speed,
    const float back_speed)
{
    // Find stick position in world space by rotating using camera azimuth
    vec3 global_stick_direction = quat_mul_vec3(
        quat_from_angle_axis(camera_azimuth, vec3(0, 1, 0)), gamepadstick_left);
    
    // Find stick position local to current facing direction
    vec3 local_stick_direction = quat_inv_mul_vec3(
        simulation_rotation, global_stick_direction);
    
    // Scale stick by forward, sideways and backwards speeds
    vec3 local_desired_velocity = local_stick_direction.z > 0.0 ?
        vec3(side_speed, 0.0f, fwrd_speed) * local_stick_direction :
        vec3(side_speed, 0.0f, back_speed) * local_stick_direction;
    
    // Re-orientate into the world space
    return quat_mul_vec3(simulation_rotation, local_desired_velocity);
}

quat desired_rotation_update(
    const quat desired_rotation,
    const vec3 gamepadstick_left,
    const vec3 gamepadstick_right,
    const float camera_azimuth,
    const bool desired_strafe,
    const vec3 desired_velocity)
{
    quat desired_rotation_curr = desired_rotation;
    
    // If strafe is active then desired direction is coming from right
    // stick as long as that stick is being used, otherwise we assume
    // forward facing
    if (desired_strafe)
    {
        vec3 desired_direction = quat_mul_vec3(quat_from_angle_axis(camera_azimuth, vec3(0, 1, 0)), vec3(0, 0, -1));

        if (length(gamepadstick_right) > 0.01f)
        {
            desired_direction = quat_mul_vec3(quat_from_angle_axis(camera_azimuth, vec3(0, 1, 0)), normalize(gamepadstick_right));
        }
        
        return quat_from_angle_axis(atan2f(desired_direction.x, desired_direction.z), vec3(0, 1, 0));            
    }
    
    // If strafe is not active the desired direction comes from the left 
    // stick as long as that stick is being used
    else if (length(gamepadstick_left) > 0.01f)
    {
        
        vec3 desired_direction = normalize(desired_velocity);
        return quat_from_angle_axis(atan2f(desired_direction.x, desired_direction.z), vec3(0, 1, 0));
    }
    
    // Otherwise desired direction remains the same
    else
    {
        return desired_rotation_curr;
    }
}
