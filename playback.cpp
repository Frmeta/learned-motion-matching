#include "playback.h"

// Copy a part of a feature vector from the 
// matching database into the query feature vector
void query_copy_denormalized_feature(
    slice1d<float> query, 
    int& offset, 
    const int size, 
    const slice1d<float> features,
    const slice1d<float> features_offset,
    const slice1d<float> features_scale)
{
    for (int i = 0; i < size; i++)
    {
        query(offset + i) = features(offset + i) * features_scale(offset + i) + features_offset(offset + i);
    }
    
    offset += size;
}

// Copy from a specific source offset in a feature vector into the current query offset.
void query_copy_denormalized_feature_from_source_offset(
    slice1d<float> query,
    int& dst_offset,
    const int size,
    const int src_offset,
    const slice1d<float> features,
    const slice1d<float> features_offset,
    const slice1d<float> features_scale)
{
    for (int i = 0; i < size; i++)
    {
        query(dst_offset + i) = features(src_offset + i) * features_scale(src_offset + i) + features_offset(src_offset + i);
    }

    dst_offset += size;
}

// Compute the query feature vector for the current 
// trajectory controlled by the gamepad.
void query_compute_trajectory_position_feature(
    slice1d<float> query, 
    int& offset, 
    const vec3 root_position, 
    const quat root_rotation, 
    const slice1d<vec3> trajectory_positions)
{
    vec3 traj0 = quat_inv_mul_vec3(root_rotation, trajectory_positions(1) - root_position);
    vec3 traj1 = quat_inv_mul_vec3(root_rotation, trajectory_positions(2) - root_position);
    vec3 traj2 = quat_inv_mul_vec3(root_rotation, trajectory_positions(3) - root_position);
    
    query(offset + 0) = traj0.x;
    query(offset + 1) = traj0.y;
    query(offset + 2) = traj0.z;
    query(offset + 3) = traj1.x;
    query(offset + 4) = traj1.y;
    query(offset + 5) = traj1.z;
    query(offset + 6) = traj2.x;
    query(offset + 7) = traj2.y;
    query(offset + 8) = traj2.z;
    
    offset += 9;
}

// Same but for the trajectory direction
void query_compute_trajectory_direction_feature(
    slice1d<float> query, 
    int& offset, 
    const vec3 root_position,
    const quat root_rotation, 
    const slice1d<vec3> trajectory_positions,
    const slice1d<quat> trajectory_rotations)
{
    vec3 traj0 = quat_inv_mul_vec3(root_rotation, quat_mul_vec3(trajectory_rotations(1), vec3(0, 0, 1)));
    vec3 traj1 = quat_inv_mul_vec3(root_rotation, quat_mul_vec3(trajectory_rotations(2), vec3(0, 0, 1)));
    vec3 traj2 = quat_inv_mul_vec3(root_rotation, quat_mul_vec3(trajectory_rotations(3), vec3(0, 0, 1)));

    // Inject signed vertical intent from trajectory slope.
    // Positive Y means moving up, negative Y means moving down.
    const vec3 d0 = trajectory_positions(1) - root_position;
    const vec3 d1 = trajectory_positions(2) - root_position;
    const vec3 d2 = trajectory_positions(3) - root_position;

    const float h0 = length(vec3(d0.x, 0.0f, d0.z));
    const float h1 = length(vec3(d1.x, 0.0f, d1.z));
    const float h2 = length(vec3(d2.x, 0.0f, d2.z));

    const float eps = 1e-4f;
    traj0.y = d0.y / maxf(h0, eps);
    traj1.y = d1.y / maxf(h1, eps);
    traj2.y = d2.y / maxf(h2, eps);

    traj0 = normalize(traj0);
    traj1 = normalize(traj1);
    traj2 = normalize(traj2);
    
    query(offset + 0) = traj0.x;
    query(offset + 1) = traj0.y;
    query(offset + 2) = traj0.z;
    query(offset + 3) = traj1.x;
    query(offset + 4) = traj1.y;
    query(offset + 5) = traj1.z;
    query(offset + 6) = traj2.x;
    query(offset + 7) = traj2.y;
    query(offset + 8) = traj2.z;
    
    offset += 9;
}

// Add terrain height features to query
void query_compute_terrain_height_feature(
    slice1d<float> query,
    int& offset,
    const slice1d<vec2> future_terrain_heights)
{
    // Store all left toe heights first (4 time samples)
    for (int i = 0; i < 4; i++)
    {
        query(offset + i) = future_terrain_heights(i).x; // Left toe heights at t0, t1, t2, t3
    }
    
    // Then all right toe heights (4 time samples)
    for (int i = 0; i < 4; i++)
    {
        query(offset + 4 + i) = future_terrain_heights(i).y; // Right toe heights at t0, t1, t2, t3
    }
    
    offset += 8;
}

vec3 adjust_character_position(
    const vec3 character_position,
    const vec3 simulation_position,
    const float halflife,
    const float dt)
{
    // Find the difference in positioning
    vec3 difference_position = simulation_position - character_position;
    
    // Damp that difference using the given halflife and dt
    vec3 adjustment_position = damp_adjustment_exact(
        difference_position,
        halflife,
        dt);
    
    // Add the damped difference to move the character toward the sim
    return adjustment_position + character_position;
}

quat adjust_character_rotation(
    const quat character_rotation,
    const quat simulation_rotation,
    const float halflife,
    const float dt)
{
    // Find the difference in rotation (from character to simulation).
    // Here `quat_abs` forces the quaternion to take the shortest 
    // path and normalization is required as sometimes taking 
    // the difference between two very similar rotations can 
    // introduce numerical instability
    quat difference_rotation = quat_abs(quat_normalize(
        quat_mul_inv(simulation_rotation, character_rotation)));
    
    // Damp that difference using the given halflife and dt
    quat adjustment_rotation = damp_adjustment_exact(
        difference_rotation,
        halflife,
        dt);
    
    // Apply the damped adjustment to the character
    return quat_mul(adjustment_rotation, character_rotation);
}

vec3 adjust_character_position_by_velocity(
    const vec3 character_position,
    const vec3 character_velocity,
    const vec3 simulation_position,
    const float max_adjustment_ratio,
    const float halflife,
    const float dt)
{
    // Find and damp the desired adjustment
    vec3 adjustment_position = damp_adjustment_exact(
        simulation_position - character_position,
        halflife,
        dt);
    
    // If the length of the adjustment is greater than the character velocity 
    // multiplied by the ratio then we need to clamp it to that length
    float max_length = max_adjustment_ratio * length(character_velocity) * dt;
    
    if (length(adjustment_position) > max_length)
    {
        adjustment_position = max_length * normalize(adjustment_position);
    }
    
    // Apply the adjustment
    return adjustment_position + character_position;
}

quat adjust_character_rotation_by_velocity(
    const quat character_rotation,
    const vec3 character_angular_velocity,
    const quat simulation_rotation,
    const float max_adjustment_ratio,
    const float halflife,
    const float dt)
{
    // Find and damp the desired rotational adjustment
    quat adjustment_rotation = damp_adjustment_exact(
        quat_abs(quat_normalize(quat_mul_inv(
            simulation_rotation, character_rotation))),
        halflife,
        dt);
    
    // If the length of the adjustment is greater than the angular velocity 
    // multiplied by the ratio then we need to clamp this adjustment
    float max_length = max_adjustment_ratio *
        length(character_angular_velocity) * dt;
    
    if (length(quat_to_scaled_angle_axis(adjustment_rotation)) > max_length)
    {
        // To clamp can convert to scaled angle axis, rescale, and convert back
        adjustment_rotation = quat_from_scaled_angle_axis(max_length * 
            normalize(quat_to_scaled_angle_axis(adjustment_rotation)));
    }
    
    // Apply the adjustment
    return quat_mul(adjustment_rotation, character_rotation);
}

vec3 clamp_character_position(
    const vec3 character_position,
    const vec3 simulation_position,
    const float max_distance)
{
    // If the character deviates too far from the simulation 
    // position we need to clamp it to within the max distance
    if (length(character_position - simulation_position) > max_distance)
    {
        return max_distance * 
            normalize(character_position - simulation_position) + 
            simulation_position;
    }
    else
    {
        return character_position;
    }
}
  
quat clamp_character_rotation(
    const quat character_rotation,
    const quat simulation_rotation,
    const float max_angle)
{
    // If the angle between the character rotation and simulation 
    // rotation exceeds the threshold we need to clamp it back
    if (quat_angle_between(character_rotation, simulation_rotation) > max_angle)
    {
        // First, find the rotational difference between the two
        quat diff = quat_abs(quat_mul_inv(
            character_rotation, simulation_rotation));
        
        // We can then decompose it into angle and axis
        float diff_angle; vec3 diff_axis;
        quat_to_angle_axis(diff, diff_angle, diff_axis);
        
        // We then clamp the angle to within our bounds
        diff_angle = clampf(diff_angle, -max_angle, max_angle);
        
        // And apply back the clamped rotation
        return quat_mul(
          quat_from_angle_axis(diff_angle, diff_axis), simulation_rotation);
    }
    else
    {
        return character_rotation;
    }
}
