#include "trajectory.h"
#include "spring.h"
#include <cmath>

static constexpr float kTerrainFollowMaxVerticalSpeed = 10.0f;
static constexpr float kTerrainFollowMinVerticalSpeed = -10.0f;

// Taken from https://theorangeduck.com/page/spring-roll-call#controllers
void simulation_positions_update(
    vec3& position, 
    vec3& velocity, 
    vec3& acceleration, 
    const vec3 desired_velocity, 
    const float halflife, 
    const float dt,
    const Model& obstacle_model)
{
    float y = halflife_to_damping(halflife) / 2.0f; 
    vec3 j0 = velocity - desired_velocity;
    vec3 j1 = acceleration + j0*y;
    float eydt = fast_negexpf(y*dt);
    
    vec3 position_prev = position;

    position = eydt*(((-j1)/(y*y)) + ((-j0 - j1*dt)/y)) + 
        (j1/(y*y)) + j0/y + desired_velocity * dt + position_prev;
    velocity = eydt*(j0 + j1*dt) + desired_velocity;
    acceleration = eydt*(acceleration - j1*y*dt);
    
    // position = simulation_collide_obstacles(
    //     position_prev, 
    //     position,
    //     obstacle_model);
    
    // Ground collision: if player is phasing through ground, set velocity.y to positive
    float terrain_height = 0.0f;
    if (sample_terrain_height(obstacle_model, position, terrain_height))
    {
        if (position.y < terrain_height)
        {
            velocity.y = maxf(velocity.y, 0.2f);  // Set to positive value (upward)
        }
    }
}

void simulation_rotations_update(
    quat& rotation, 
    vec3& angular_velocity, 
    const quat desired_rotation, 
    const float halflife, 
    const float dt)
{
    simple_spring_damper_exact(
        rotation, 
        angular_velocity, 
        desired_rotation, 
        halflife, dt);
}

// Predict what the desired velocity will be in the 
// future. Here we need to use the future trajectory 
// rotation as well as predicted future camera 
// position to find an accurate desired velocity in 
// the world space
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
  const float dt)
{
    desired_velocities(0) = desired_velocity;
        if (jump_active)
        {
                desired_velocities(0).y = jump_vertical_velocity;
        }

    // Inject vertical trajectory intent from terrain slope so future
    // trajectory Y becomes positive uphill and negative downhill.
    float current_terrain_height = 0.0f;
    const bool has_current_terrain = sample_terrain_height(
        ground_plane_model,
        simulation_position,
        current_terrain_height);
    
    for (int i = 1; i < desired_velocities.size; i++)
    {
        desired_velocities(i) = desired_velocity_update(
            gamepadstick_left,
            orbit_camera_update_azimuth(
                camera_azimuth, gamepadstick_right, desired_strafe, i * dt),
            trajectory_rotations(i),
            fwrd_speed,
            side_speed,
            back_speed);

        if (jump_active && jump_speed_boost > 1.0f)
        {
            desired_velocities(i).x *= jump_speed_boost;
            desired_velocities(i).z *= jump_speed_boost;
        }

        if (jump_active)
        {
            float predicted_vy = jump_vertical_velocity - jump_gravity * (i * dt);
            desired_velocities(i).y = clampf(predicted_vy, kTerrainFollowMinVerticalSpeed, kTerrainFollowMaxVerticalSpeed);
        }
        else if (jump_gait_timer > 0.0f)
        {
            // Fading downward arc during holdover: simulates post-landing settling,
            // scaled by how much holdover time remains (fades smoothly to 0)
            float holdover_blend = jump_gait_timer / jump_gait_hold_time;
            float predicted_vy = -jump_gravity * (i * dt) * holdover_blend;
            desired_velocities(i).y = clampf(predicted_vy, kTerrainFollowMinVerticalSpeed, 0.0f);
        }
        else if (has_current_terrain)
        {
            float future_terrain_height = 0.0f;
            vec3 probe_position = simulation_position +
                vec3(desired_velocities(i).x, 0.0f, desired_velocities(i).z) * (i * dt);

            if (sample_terrain_height(ground_plane_model, probe_position, future_terrain_height))
            {
                float prediction_time = maxf(i * dt, 1e-4f);
                float target_vertical_speed = (future_terrain_height - current_terrain_height) / prediction_time;
                desired_velocities(i).y = clampf(target_vertical_speed, kTerrainFollowMinVerticalSpeed, kTerrainFollowMaxVerticalSpeed);
            }
        }
    }
}

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
    const Model& obstacle_model)
{
    positions(0) = position;
    velocities(0) = velocity;
    accelerations(0) = acceleration;
    
    for (int i = 1; i < positions.size; i++)
    {
        positions(i) = positions(i-1);
        velocities(i) = velocities(i-1);
        accelerations(i) = accelerations(i-1);
        
        simulation_positions_update(
            positions(i), 
            velocities(i), 
            accelerations(i), 
            desired_velocities(i), 
            halflife, 
            dt, 
            obstacle_model);
    }
}

// Predict desired rotations given the estimated future 
// camera rotation and other parameters
void trajectory_desired_rotations_predict(
  slice1d<quat> desired_rotations,
  const slice1d<vec3> desired_velocities,
  const quat desired_rotation,
  const float camera_azimuth,
  const vec3 gamepadstick_left,
  const vec3 gamepadstick_right,
  const bool desired_strafe,
  const float dt)
{
    desired_rotations(0) = desired_rotation;
    
    for (int i = 1; i < desired_rotations.size; i++)
    {
        desired_rotations(i) = desired_rotation_update(
            desired_rotations(i-1),
            gamepadstick_left,
            gamepadstick_right,
            orbit_camera_update_azimuth(
                camera_azimuth, gamepadstick_right, desired_strafe, i * dt),
            desired_strafe,
            desired_velocities(i));
    }
}

void trajectory_rotations_predict(
    slice1d<quat> rotations, 
    slice1d<vec3> angular_velocities, 
    const quat rotation, 
    const vec3 angular_velocity, 
    const slice1d<quat> desired_rotations, 
    const float halflife,
    const float dt)
{
    rotations.set(rotation);
    angular_velocities.set(angular_velocity);
    
    for (int i = 1; i < rotations.size; i++)
    {
        simulation_rotations_update(
            rotations(i), 
            angular_velocities(i), 
            desired_rotations(i), 
            halflife, 
            i * dt);
    }
}
