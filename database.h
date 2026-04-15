#pragma once

#include "common.h"
#include "vec.h"
#include "quat.h"
#include "array.h"
#include "character.h"

#include <assert.h>
#include <float.h>
#include <stdio.h>
#include <math.h>

//--------------------------------------

enum
{
    BOUND_SM_SIZE = 16,
    BOUND_LR_SIZE = 64,
};

struct database
{
    array2d<vec3> bone_positions;
    array2d<vec3> bone_velocities;
    array2d<quat> bone_rotations;
    array2d<vec3> bone_angular_velocities;
    array1d<int> bone_parents;
    
    array1d<int> range_starts;
    array1d<int> range_stops;
    
    array2d<float> features;
    array1d<float> features_offset;
    array1d<float> features_scale;
    
    array2d<bool> contact_states;
    
    array2d<float> future_toe_positions;  // 12 floats per frame (6 vec2: L15, R15, L30, R30, L45, R45)
    array2d<bool> crouch_states;          // 1 column per frame: 1 for crouch clip, 0 otherwise
    array2d<bool> idle_states;            // 1 column per frame: 1 for idle clip, 0 otherwise
    array2d<bool> jump_states;            // 1 column per frame: 1 for jump clip, 0 otherwise
    
    array2d<float> bound_sm_min;
    array2d<float> bound_sm_max;
    array2d<float> bound_lr_min;
    array2d<float> bound_lr_max;
    
    int nframes() const { return bone_positions.rows; }
    int nbones() const { return bone_positions.cols; }
    int nranges() const { return range_starts.size; }
    int nfeatures() const { return features.cols; }
    int ncontacts() const { return contact_states.cols; }
};

void database_load(database& db, const char* filename)
{
    FILE* f = fopen(filename, "rb");
    assert(f != NULL);
    
    array2d_read(db.bone_positions, f);
    array2d_read(db.bone_velocities, f);
    array2d_read(db.bone_rotations, f);
    array2d_read(db.bone_angular_velocities, f);
    array1d_read(db.bone_parents, f);
    
    array1d_read(db.range_starts, f);
    array1d_read(db.range_stops, f);
    
    array2d_read(db.contact_states, f);
    
    array2d_read(db.future_toe_positions, f);  // Load precomputed future toe positions

    // Optional field for newer database versions.
    long crouch_pos = ftell(f);
    fseek(f, 0, SEEK_END);
    long file_end = ftell(f);
    fseek(f, crouch_pos, SEEK_SET);

    if (file_end - crouch_pos >= (long)(2 * sizeof(int)))
    {
        array2d_read(db.crouch_states, f);
    }
    else
    {
        db.crouch_states.resize(db.nframes(), 1);
        db.crouch_states.zero();
    }

    // Optional field for newest database versions.
    long idle_pos = ftell(f);
    fseek(f, 0, SEEK_END);
    file_end = ftell(f);
    fseek(f, idle_pos, SEEK_SET);

    if (file_end - idle_pos >= (long)(2 * sizeof(int)))
    {
        array2d_read(db.idle_states, f);
    }
    else
    {
        db.idle_states.resize(db.nframes(), 1);
        db.idle_states.zero();
    }

    // Optional field for newest database versions.
    long jump_pos = ftell(f);
    fseek(f, 0, SEEK_END);
    file_end = ftell(f);
    fseek(f, jump_pos, SEEK_SET);

    if (file_end - jump_pos >= (long)(2 * sizeof(int)))
    {
        array2d_read(db.jump_states, f);
    }
    else
    {
        db.jump_states.resize(db.nframes(), 1);
        db.jump_states.zero();
    }
    
    fclose(f);
}

void compute_crouch_feature(database& db, int& offset)
{
    const float crouch_feature_strength = 6.0f;

    bool has_crouch =
        db.crouch_states.rows == db.nframes() &&
        db.crouch_states.cols >= 1;

    for (int i = 0; i < db.nframes(); i++)
    {
        db.features(i, offset) = has_crouch && db.crouch_states(i, 0) ? crouch_feature_strength : 0.0f;
    }

    // Keep this semantic flag in raw binary space.
    db.features_offset(offset) = 0.0f;
    db.features_scale(offset) = 1.0f;
    offset += 1;
}

void compute_idle_feature(database& db, int& offset)
{
    const float idle_feature_strength = 6.0f;

    bool has_idle =
        db.idle_states.rows == db.nframes() &&
        db.idle_states.cols >= 1;

    for (int i = 0; i < db.nframes(); i++)
    {
        db.features(i, offset) = has_idle && db.idle_states(i, 0) ? idle_feature_strength : 0.0f;
    }

    // Keep this semantic flag in raw binary space.
    db.features_offset(offset) = 0.0f;
    db.features_scale(offset) = 1.0f;
    offset += 1;
}

void compute_jump_feature(database& db, int& offset)
{
    const float jump_feature_strength = 6.0f;

    bool has_jump =
        db.jump_states.rows == db.nframes() &&
        db.jump_states.cols >= 1;

    for (int i = 0; i < db.nframes(); i++)
    {
        db.features(i, offset) = has_jump && db.jump_states(i, 0) ? jump_feature_strength : 0.0f;
    }

    // Keep this semantic flag in raw binary space.
    db.features_offset(offset) = 0.0f;
    db.features_scale(offset) = 1.0f;
    offset += 1;
}

void database_save_matching_features(const database& db, const char* filename, bool is_saved_as_csv = false)
{
    FILE* f = fopen(filename, "wb");
    assert(f != NULL);
    
    array2d_write(db.features, f);
    array1d_write(db.features_offset, f);
    array1d_write(db.features_scale, f);
    
    fclose(f);
    
    // Save as CSV if requested
    if (is_saved_as_csv)
    {
        // Create CSV filename by replacing .bin with .csv
        char csv_filename[512];
        strcpy(csv_filename, filename);
        char* dot = strrchr(csv_filename, '.');
        if (dot != NULL)
        {
            strcpy(dot, ".csv");
        }
        else
        {
            strcat(csv_filename, ".csv");
        }
        
        // Write features to CSV
        FILE* csv_f = fopen(csv_filename, "w");
        assert(csv_f != NULL);
        
        // Write header with offsets and scales
        fprintf(csv_f, "# Features Offset:\n");
        for (int j = 0; j < db.features_offset.size; j++)
        {
            fprintf(csv_f, "%f", db.features_offset(j));
            if (j < db.features_offset.size - 1) fprintf(csv_f, ",");
        }
        fprintf(csv_f, "\n");
        
        fprintf(csv_f, "# Features Scale:\n");
        for (int j = 0; j < db.features_scale.size; j++)
        {
            fprintf(csv_f, "%f", db.features_scale(j));
            if (j < db.features_scale.size - 1) fprintf(csv_f, ",");
        }
        fprintf(csv_f, "\n");
        
        // Write features data
        fprintf(csv_f, "# Features Data (rows=%d, cols=%d):\n", db.features.rows, db.features.cols);
        for (int i = 0; i < db.features.rows; i++)
        {
            for (int j = 0; j < db.features.cols; j++)
            {
                fprintf(csv_f, "%f", db.features(i, j));
                if (j < db.features.cols - 1) fprintf(csv_f, ",");
            }
            fprintf(csv_f, "\n");
        }
        
        fclose(csv_f);
    }
}

void database_load_matching_features(database& db, const char* filename)
{
    FILE* f = fopen(filename, "rb");
    assert(f != NULL);

    array2d_read(db.features, f);
    array1d_read(db.features_offset, f);
    array1d_read(db.features_scale, f);

    fclose(f);
}

// When we add an offset to a frame in the database there is a chance
// it will go out of the relevant range so here we can clamp it to 
// the last frame of that range.
int database_index_clamp(database& db, int frame, int offset)
{
    for (int i = 0; i < db.nranges(); i++)
    {
        if (frame >= db.range_starts(i) && frame < db.range_stops(i))
        {
            return clamp(frame + offset, db.range_starts(i), db.range_stops(i) - 1);
        }
    }
    
    assert(false);
    return -1;
}

//--------------------------------------

void normalize_feature(
    slice2d<float> features,
    slice1d<float> features_offset, // mean
    slice1d<float> features_scale,
    const int offset, 
    const int size, 
    const float weight = 1.0f)
{
    // First compute what is essentially the mean 
    // value for each feature dimension
    for (int j = 0; j < size; j++)
    {
        features_offset(offset + j) = 0.0f;    
    }
    
    for (int i = 0; i < features.rows; i++)
    {
        for (int j = 0; j < size; j++)
        {
            features_offset(offset + j) += features(i, offset + j) / features.rows;
        }
    }
    
    // Now compute the variance of each feature dimension
    array1d<float> vars(size);
    vars.zero();
    
    for (int i = 0; i < features.rows; i++)
    {
        for (int j = 0; j < size; j++)
        {
            vars(j) += squaref(features(i, offset + j) - features_offset(offset + j)) / features.rows;
        }
    }
    
    // We compute the overall std of the feature as the average
    // std across all dimensions
    float std = 0.0f;
    for (int j = 0; j < size; j++)
    {
        std += sqrtf(vars(j)) / size;
    }
    
    // Features with no variation can have zero std which is
    // almost always a bug.
    assert(std > 0.0);
    
    // The scale of a feature is just the std divided by the weight
    for (int j = 0; j < size; j++)
    {
        features_scale(offset + j) = std / weight;
    }
    
    // Using the offset and scale we can then normalize the features
    for (int i = 0; i < features.rows; i++)
    {
        for (int j = 0; j < size; j++)
        {
            features(i, offset + j) = (features(i, offset + j) - features_offset(offset + j)) / features_scale(offset + j);
        }
    }
}

void denormalize_features(
    slice1d<float> features,
    const slice1d<float> features_offset,
    const slice1d<float> features_scale)
{
    for (int i = 0; i < features.size; i++)
    {
        features(i) = (features(i) * features_scale(i)) + features_offset(i);
    }  
}

//--------------------------------------

// Here I am using a simple recursive version of forward kinematics
void forward_kinematics(
    vec3& bone_position,
    quat& bone_rotation,
    const slice1d<vec3> bone_positions,
    const slice1d<quat> bone_rotations,
    const slice1d<int> bone_parents,
    const int bone)
{
    if (bone_parents(bone) != -1)
    {
        vec3 parent_position;
        quat parent_rotation;
        
        forward_kinematics(
            parent_position,
            parent_rotation,
            bone_positions,
            bone_rotations,
            bone_parents,
            bone_parents(bone));
        
        bone_position = quat_mul_vec3(parent_rotation, bone_positions(bone)) + parent_position;
        bone_rotation = quat_mul(parent_rotation, bone_rotations(bone));
    }
    else
    {
        bone_position = bone_positions(bone);
        bone_rotation = bone_rotations(bone); 
    }
}

// Forward kinematics but also compute the velocities
void forward_kinematics_velocity(
    vec3& bone_position,
    vec3& bone_velocity,
    quat& bone_rotation,
    vec3& bone_angular_velocity,
    const slice1d<vec3> bone_positions,
    const slice1d<vec3> bone_velocities,
    const slice1d<quat> bone_rotations,
    const slice1d<vec3> bone_angular_velocities,
    const slice1d<int> bone_parents,
    const int bone)
{
    //
    if (bone_parents(bone) != -1)
    {
        vec3 parent_position;
        vec3 parent_velocity;
        quat parent_rotation;
        vec3 parent_angular_velocity;
        
        forward_kinematics_velocity(
            parent_position,
            parent_velocity,
            parent_rotation,
            parent_angular_velocity,
            bone_positions,
            bone_velocities,
            bone_rotations,
            bone_angular_velocities,
            bone_parents,
            bone_parents(bone));
        
        bone_position = quat_mul_vec3(parent_rotation, bone_positions(bone)) + parent_position;
        bone_velocity = 
            parent_velocity + 
            quat_mul_vec3(parent_rotation, bone_velocities(bone)) + 
            cross(parent_angular_velocity, quat_mul_vec3(parent_rotation, bone_positions(bone)));
        bone_rotation = quat_mul(parent_rotation, bone_rotations(bone));
        bone_angular_velocity = quat_mul_vec3(parent_rotation, bone_angular_velocities(bone)) + parent_angular_velocity;
    }
    else
    {
        bone_position = bone_positions(bone);
        bone_velocity = bone_velocities(bone);
        bone_rotation = bone_rotations(bone);
        bone_angular_velocity = bone_angular_velocities(bone); 
    }
}

// Compute forward kinematics for all joints
void forward_kinematics_full(
    slice1d<vec3> global_bone_positions,
    slice1d<quat> global_bone_rotations,
    const slice1d<vec3> local_bone_positions,
    const slice1d<quat> local_bone_rotations,
    const slice1d<int> bone_parents)
{
    for (int i = 0; i < bone_parents.size; i++)
    {
        // Assumes bones are always sorted from root onwards
        assert(bone_parents(i) < i);
        
        if (bone_parents(i) == -1)
        {
            global_bone_positions(i) = local_bone_positions(i);
            global_bone_rotations(i) = local_bone_rotations(i);
        }
        else
        {
            vec3 parent_position = global_bone_positions(bone_parents(i));
            quat parent_rotation = global_bone_rotations(bone_parents(i));
            global_bone_positions(i) = quat_mul_vec3(parent_rotation, local_bone_positions(i)) + parent_position;
            global_bone_rotations(i) = quat_mul(parent_rotation, local_bone_rotations(i));
        }
    }
}

// Compute forward kinematics of just some joints using a
// mask to indicate which joints are already computed
void forward_kinematics_partial(
    slice1d<vec3> global_bone_positions,
    slice1d<quat> global_bone_rotations,
    slice1d<bool> global_bone_computed,
    const slice1d<vec3> local_bone_positions,
    const slice1d<quat> local_bone_rotations,
    const slice1d<int> bone_parents,
    int bone)
{
    if (bone_parents(bone) == -1)
    {
        global_bone_positions(bone) = local_bone_positions(bone);
        global_bone_rotations(bone) = local_bone_rotations(bone);
        global_bone_computed(bone) = true;
        return;
    }
    
    if (!global_bone_computed(bone_parents(bone)))
    {
        forward_kinematics_partial(
            global_bone_positions,
            global_bone_rotations,
            global_bone_computed,
            local_bone_positions,
            local_bone_rotations,
            bone_parents,
            bone_parents(bone));
    }
    
    vec3 parent_position = global_bone_positions(bone_parents(bone));
    quat parent_rotation = global_bone_rotations(bone_parents(bone));
    global_bone_positions(bone) = quat_mul_vec3(parent_rotation, local_bone_positions(bone)) + parent_position;
    global_bone_rotations(bone) = quat_mul(parent_rotation, local_bone_rotations(bone));
    global_bone_computed(bone) = true;
}

// Same but including velocity
void forward_kinematics_velocity_partial(
    slice1d<vec3> global_bone_positions,
    slice1d<vec3> global_bone_velocities,
    slice1d<quat> global_bone_rotations,
    slice1d<vec3> global_bone_angular_velocities,
    slice1d<bool> global_bone_computed,
    const slice1d<vec3> local_bone_positions,
    const slice1d<vec3> local_bone_velocities,
    const slice1d<quat> local_bone_rotations,
    const slice1d<vec3> local_bone_angular_velocities,
    const slice1d<int> bone_parents,
    int bone)
{
    if (bone_parents(bone) == -1)
    {
        global_bone_positions(bone) = local_bone_positions(bone);
        global_bone_velocities(bone) = local_bone_velocities(bone);
        global_bone_rotations(bone) = local_bone_rotations(bone);
        global_bone_angular_velocities(bone) = local_bone_angular_velocities(bone);
        global_bone_computed(bone) = true;
        return;
    }
    
    if (!global_bone_computed(bone_parents(bone)))
    {
        forward_kinematics_velocity_partial(
            global_bone_positions,
            global_bone_velocities,
            global_bone_rotations,
            global_bone_angular_velocities,
            global_bone_computed,
            local_bone_positions,
            local_bone_velocities,
            local_bone_rotations,
            local_bone_angular_velocities,
            bone_parents,
            bone_parents(bone));
    }
    
    vec3 parent_position = global_bone_positions(bone_parents(bone));
    vec3 parent_velocity = global_bone_velocities(bone_parents(bone));
    quat parent_rotation = global_bone_rotations(bone_parents(bone));
    vec3 parent_angular_velocity = global_bone_angular_velocities(bone_parents(bone));
    
    global_bone_positions(bone) = quat_mul_vec3(parent_rotation, local_bone_positions(bone)) + parent_position;
    global_bone_velocities(bone) = 
        parent_velocity + 
        quat_mul_vec3(parent_rotation, local_bone_velocities(bone)) + 
        cross(parent_angular_velocity, quat_mul_vec3(parent_rotation, local_bone_positions(bone)));
    global_bone_rotations(bone) = quat_mul(parent_rotation, local_bone_rotations(bone));
    global_bone_angular_velocities(bone) = quat_mul_vec3(parent_rotation, local_bone_angular_velocities(bone)) + parent_angular_velocity;
    global_bone_computed(bone) = true;
}

//--------------------------------------

// Compute a feature for the position of a bone relative to the simulation/root bone
void compute_bone_position_feature(database& db, int& offset, int bone, float weight = 1.0f)
{
    for (int i = 0; i < db.nframes(); i++)
    {
        vec3 bone_position;
        quat bone_rotation;
        
        forward_kinematics(
            bone_position,
            bone_rotation,
            db.bone_positions(i),
            db.bone_rotations(i),
            db.bone_parents,
            bone);
        
        bone_position = quat_mul_vec3(quat_inv(db.bone_rotations(i, 0)), bone_position - db.bone_positions(i, 0));
        
        db.features(i, offset + 0) = bone_position.x;
        db.features(i, offset + 1) = bone_position.y;
        db.features(i, offset + 2) = bone_position.z;
    }
    
    normalize_feature(db.features, db.features_offset, db.features_scale, offset, 3, weight);
    
    offset += 3;
}

// Similar but for a bone's velocity
void compute_bone_velocity_feature(database& db, int& offset, int bone, float weight = 1.0f)
{
    for (int i = 0; i < db.nframes(); i++)
    {
        vec3 bone_position;
        vec3 bone_velocity;
        quat bone_rotation;
        vec3 bone_angular_velocity;
        
        forward_kinematics_velocity(
            bone_position,
            bone_velocity,
            bone_rotation,
            bone_angular_velocity,
            db.bone_positions(i),
            db.bone_velocities(i),
            db.bone_rotations(i),
            db.bone_angular_velocities(i),
            db.bone_parents,
            bone);
        
        bone_velocity = quat_mul_vec3(quat_inv(db.bone_rotations(i, 0)), bone_velocity);
        
        db.features(i, offset + 0) = bone_velocity.x;
        db.features(i, offset + 1) = bone_velocity.y;
        db.features(i, offset + 2) = bone_velocity.z;
    }
    
    normalize_feature(db.features, db.features_offset, db.features_scale, offset, 3, weight);
    
    offset += 3;
}

// Compute the trajectory at 20, 40, and 60 frames in the future
void compute_trajectory_position_feature(database& db, int& offset, float weight = 1.0f)
{
    for (int i = 0; i < db.nframes(); i++)
    {
        int t0 = database_index_clamp(db, i, 20);
        int t1 = database_index_clamp(db, i, 40);
        int t2 = database_index_clamp(db, i, 60);
        
        vec3 trajectory_pos0 = quat_mul_vec3(quat_inv(db.bone_rotations(i, 0)), db.bone_positions(t0, 0) - db.bone_positions(i, 0));
        vec3 trajectory_pos1 = quat_mul_vec3(quat_inv(db.bone_rotations(i, 0)), db.bone_positions(t1, 0) - db.bone_positions(i, 0));
        vec3 trajectory_pos2 = quat_mul_vec3(quat_inv(db.bone_rotations(i, 0)), db.bone_positions(t2, 0) - db.bone_positions(i, 0));
        
        db.features(i, offset + 0) = trajectory_pos0.x;
        db.features(i, offset + 1) = trajectory_pos0.y;
        db.features(i, offset + 2) = trajectory_pos0.z;
        db.features(i, offset + 3) = trajectory_pos1.x;
        db.features(i, offset + 4) = trajectory_pos1.y;
        db.features(i, offset + 5) = trajectory_pos1.z;
        db.features(i, offset + 6) = trajectory_pos2.x;
        db.features(i, offset + 7) = trajectory_pos2.y;
        db.features(i, offset + 8) = trajectory_pos2.z;
    }
    
    normalize_feature(db.features, db.features_offset, db.features_scale, offset, 9, weight);
    
    offset += 9;
}

// Same for direction
void compute_trajectory_direction_feature(database& db, int& offset, float weight = 1.0f)
{
    for (int i = 0; i < db.nframes(); i++)
    {
        int t0 = database_index_clamp(db, i, 20);
        int t1 = database_index_clamp(db, i, 40);
        int t2 = database_index_clamp(db, i, 60);
        
        vec3 trajectory_dir0 = quat_mul_vec3(quat_inv(db.bone_rotations(i, 0)), quat_mul_vec3(db.bone_rotations(t0, 0), vec3(0, 0, 1)));
        vec3 trajectory_dir1 = quat_mul_vec3(quat_inv(db.bone_rotations(i, 0)), quat_mul_vec3(db.bone_rotations(t1, 0), vec3(0, 0, 1)));
        vec3 trajectory_dir2 = quat_mul_vec3(quat_inv(db.bone_rotations(i, 0)), quat_mul_vec3(db.bone_rotations(t2, 0), vec3(0, 0, 1)));

        // Inject signed vertical intent from trajectory slope so direction Y
        // becomes positive uphill and negative downhill.
        vec3 trajectory_pos0 = quat_mul_vec3(quat_inv(db.bone_rotations(i, 0)), db.bone_positions(t0, 0) - db.bone_positions(i, 0));
        vec3 trajectory_pos1 = quat_mul_vec3(quat_inv(db.bone_rotations(i, 0)), db.bone_positions(t1, 0) - db.bone_positions(i, 0));
        vec3 trajectory_pos2 = quat_mul_vec3(quat_inv(db.bone_rotations(i, 0)), db.bone_positions(t2, 0) - db.bone_positions(i, 0));

        const float eps = 1e-4f;
        float h0 = length(vec3(trajectory_pos0.x, 0.0f, trajectory_pos0.z));
        float h1 = length(vec3(trajectory_pos1.x, 0.0f, trajectory_pos1.z));
        float h2 = length(vec3(trajectory_pos2.x, 0.0f, trajectory_pos2.z));

        trajectory_dir0.y = trajectory_pos0.y / maxf(h0, eps);
        trajectory_dir1.y = trajectory_pos1.y / maxf(h1, eps);
        trajectory_dir2.y = trajectory_pos2.y / maxf(h2, eps);

        trajectory_dir0 = normalize(trajectory_dir0);
        trajectory_dir1 = normalize(trajectory_dir1);
        trajectory_dir2 = normalize(trajectory_dir2);
        
        db.features(i, offset + 0) = trajectory_dir0.x;
        db.features(i, offset + 1) = trajectory_dir0.y;
        db.features(i, offset + 2) = trajectory_dir0.z;
        db.features(i, offset + 3) = trajectory_dir1.x;
        db.features(i, offset + 4) = trajectory_dir1.y;
        db.features(i, offset + 5) = trajectory_dir1.z;
        db.features(i, offset + 6) = trajectory_dir2.x;
        db.features(i, offset + 7) = trajectory_dir2.y;
        db.features(i, offset + 8) = trajectory_dir2.z;
    }

    normalize_feature(db.features, db.features_offset, db.features_scale, offset, 9, weight);

    offset += 9;
}

// Helper function to build terrain height map from foot contact points
// Extracts all foot contact points and builds a nearest neighbor lookup structure
void build_terrain_height_map(
    const database& db,
    array1d<vec3>& terrain_points,  // (x, y, height) for each contact point
    array1d<int>& contact_bone_indices, // bone index for each contact point
    array1d<int>& contact_frame_indices) // frame index for each contact point
{
    // Collect all foot contact points
    int left_toe = Bone_LeftToe;
    int right_toe = Bone_RightToe;
    
    int contact_count = 0;
    
    // First pass: count total contact points
    for (int i = 0; i < db.nframes(); i++)
    {
        if (db.contact_states(i, 0)) contact_count++; // Left foot
        if (db.contact_states(i, 1)) contact_count++; // Right foot
    }
    
    // Allocate storage
    terrain_points.resize(contact_count);
    contact_bone_indices.resize(contact_count);
    contact_frame_indices.resize(contact_count);
    
    // Second pass: extract contact points
    int contact_idx = 0;
    for (int frame = 0; frame < db.nframes(); frame++)
    {
        // Get hip position for reference
        vec3 hip_position;
        quat hip_rotation;
        forward_kinematics(
            hip_position,
            hip_rotation,
            db.bone_positions(frame),
            db.bone_rotations(frame),
            db.bone_parents,
            Bone_Hips);
        
        // Extract left foot contact points
        if (db.contact_states(frame, 0))
        {
            vec3 foot_position;
            quat foot_rotation;
            forward_kinematics(
                foot_position,
                foot_rotation,
                db.bone_positions(frame),
                db.bone_rotations(frame),
                db.bone_parents,
                left_toe);
            
            terrain_points(contact_idx) = vec3(foot_position.x, foot_position.z, foot_position.y);
            contact_bone_indices(contact_idx) = left_toe;
            contact_frame_indices(contact_idx) = frame;
            contact_idx++;
        }
        
        // Extract right foot contact points
        if (db.contact_states(frame, 1))
        {
            vec3 foot_position;
            quat foot_rotation;
            forward_kinematics(
                foot_position,
                foot_rotation,
                db.bone_positions(frame),
                db.bone_rotations(frame),
                db.bone_parents,
                right_toe);
            
            terrain_points(contact_idx) = vec3(foot_position.x, foot_position.z, foot_position.y);
            contact_bone_indices(contact_idx) = right_toe;
            contact_frame_indices(contact_idx) = frame;
            contact_idx++;
        }
    }
}

// Query terrain height at a given (x, y) position using nearest neighbor
// Returns the height of the nearest foot contact point within the specified range
float query_terrain_height(
    const array1d<vec3>& terrain_points,
    const array1d<int>& contact_frame_indices,
    int range_start,
    int range_stop,
    float query_x,
    float query_y)
{
    if (terrain_points.size == 0)
        return 0.0f;
    
    float min_distance_sq = FLT_MAX;
    float height = 0.0f;
    
    for (int i = 0; i < terrain_points.size; i++)
    {
        // Only search contact points within the current animation range
        if (contact_frame_indices(i) < range_start || contact_frame_indices(i) >= range_stop)
            continue;
        
        float dx = terrain_points(i).x - query_x;
        float dy = terrain_points(i).y - query_y;
        float distance_sq = dx * dx + dy * dy;
        
        if (distance_sq < min_distance_sq)
        {
            min_distance_sq = distance_sq;
            height = terrain_points(i).z;
        }
    }
    
    return height;
}

void compute_terrain_height_feature(database& db, int& offset, int bone, float weight = 1.0f)
{
    // Build terrain height map from all foot contact points
    array1d<vec3> terrain_points;
    array1d<int> contact_bone_indices;
    array1d<int> contact_frame_indices;
    build_terrain_height_map(db, terrain_points, contact_bone_indices, contact_frame_indices);
    
    // Sample terrain height at future frames
    for (int i = 0; i < db.nframes(); i++)
    {
        // Find which animation range the current frame belongs to
        int range_start = 0;
        int range_stop = db.nframes();
        for (int r = 0; r < db.nranges(); r++)
        {
            if (i >= db.range_starts(r) && i < db.range_stops(r))
            {
                range_start = db.range_starts(r);
                range_stop = db.range_stops(r);
                break;
            }
        }
        
        // Get hip position and rotation for current frame
        vec3 hip_position;
        quat hip_rotation;
        forward_kinematics(
            hip_position,
            hip_rotation,
            db.bone_positions(i),
            db.bone_rotations(i),
            db.bone_parents,
            Bone_Hips);
        
        // Sample at 4 future time points
        int t0 = database_index_clamp(db, i, 0);
        int t1 = database_index_clamp(db, i, 15);
        int t2 = database_index_clamp(db, i, 30);
        int t3 = database_index_clamp(db, i, 45);
        
        // Get toe positions at future frames
        vec3 future_toe_pos0;
        vec3 future_toe_pos1;
        vec3 future_toe_pos2;
        vec3 future_toe_pos3;

        quat temp_rot;

        // FK to get global position of toe
        forward_kinematics(future_toe_pos0, temp_rot, 
            db.bone_positions(t0), db.bone_rotations(t0), 
            db.bone_parents, bone);
        
        forward_kinematics(future_toe_pos1, temp_rot, 
            db.bone_positions(t1), db.bone_rotations(t1), 
            db.bone_parents, bone);
        
        forward_kinematics(future_toe_pos2, temp_rot, 
            db.bone_positions(t2), db.bone_rotations(t2), 
            db.bone_parents, bone);
        
        forward_kinematics(future_toe_pos3, temp_rot, 
            db.bone_positions(t3), db.bone_rotations(t3), 
            db.bone_parents, bone);
        
        // Query terrain height from xy of both toes within current animation range
        float height0 = query_terrain_height(terrain_points, contact_frame_indices, range_start, range_stop, future_toe_pos0.x, future_toe_pos0.z);
        float height1 = query_terrain_height(terrain_points, contact_frame_indices, range_start, range_stop, future_toe_pos1.x, future_toe_pos1.z);
        float height2 = query_terrain_height(terrain_points, contact_frame_indices, range_start, range_stop, future_toe_pos2.x, future_toe_pos2.z);
        float height3 = query_terrain_height(terrain_points, contact_frame_indices, range_start, range_stop, future_toe_pos3.x, future_toe_pos3.z);
        
        // Store height relative to current hip height
        db.features(i, offset + 0) = height0 - hip_position.y;
        db.features(i, offset + 1) = height1 - hip_position.y;
        db.features(i, offset + 2) = height2 - hip_position.y;
        db.features(i, offset + 3) = height3 - hip_position.y;
    }

    normalize_feature(db.features, db.features_offset, db.features_scale, offset, 4, weight);

    offset += 4;
}


// Build the Motion Matching search acceleration structure. Here we
// just use axis aligned bounding boxes regularly spaced at BOUND_SM_SIZE
// and BOUND_LR_SIZE frames
void database_build_bounds(database& db)
{
    int nbound_sm = ((db.nframes() + BOUND_SM_SIZE - 1) / BOUND_SM_SIZE);
    int nbound_lr = ((db.nframes() + BOUND_LR_SIZE - 1) / BOUND_LR_SIZE);
    
    db.bound_sm_min.resize(nbound_sm, db.nfeatures()); 
    db.bound_sm_max.resize(nbound_sm, db.nfeatures()); 
    db.bound_lr_min.resize(nbound_lr, db.nfeatures()); 
    db.bound_lr_max.resize(nbound_lr, db.nfeatures()); 
    
    db.bound_sm_min.set(+FLT_MAX);
    db.bound_sm_max.set(-FLT_MAX);
    db.bound_lr_min.set(+FLT_MAX);
    db.bound_lr_max.set(-FLT_MAX);
    
    for (int i = 0; i < db.nframes(); i++)
    {
        int i_sm = i / BOUND_SM_SIZE;
        int i_lr = i / BOUND_LR_SIZE;
        
        for (int j = 0; j < db.nfeatures(); j++)
        {
            db.bound_sm_min(i_sm, j) = minf(db.bound_sm_min(i_sm, j), db.features(i, j));
            db.bound_sm_max(i_sm, j) = maxf(db.bound_sm_max(i_sm, j), db.features(i, j));
            db.bound_lr_min(i_lr, j) = minf(db.bound_lr_min(i_lr, j), db.features(i, j));
            db.bound_lr_max(i_lr, j) = maxf(db.bound_lr_max(i_lr, j), db.features(i, j));
        }
    }
}

// Build all motion matching features and acceleration structure
void database_build_matching_features(
    database& db,
    const float feature_weight_foot_position,
    const float feature_weight_foot_velocity,
    const float feature_weight_hip_velocity,
    const float feature_weight_trajectory_positions,
    const float feature_weight_trajectory_directions,
    const float feature_weight_terrain_heights)
{
    int nfeatures = 
        3 + // Left Foot Position
        3 + // Right Foot Position 
        3 + // Left Foot Velocity
        3 + // Right Foot Velocity
        3 + // Hip Velocity
        9 + // Trajectory Positions 3D
        9 + // Trajectory Directions 3D
        8 + // Terrain Heights
        1 + // Idle Flag
        1 + // Crouch Flag
        1; // Jump Flag
        
    db.features.resize(db.nframes(), nfeatures);
    db.features_offset.resize(nfeatures);
    db.features_scale.resize(nfeatures);
    
    int offset = 0;
    compute_bone_position_feature(db, offset, Bone_LeftFoot, feature_weight_foot_position);
    compute_bone_position_feature(db, offset, Bone_RightFoot, feature_weight_foot_position);
    compute_bone_velocity_feature(db, offset, Bone_LeftFoot, feature_weight_foot_velocity);
    compute_bone_velocity_feature(db, offset, Bone_RightFoot, feature_weight_foot_velocity);
    compute_bone_velocity_feature(db, offset, Bone_Hips, feature_weight_hip_velocity);
    compute_trajectory_position_feature(db, offset, feature_weight_trajectory_positions);
    compute_trajectory_direction_feature(db, offset, feature_weight_trajectory_directions);
    compute_terrain_height_feature(db, offset, Bone_LeftToe, feature_weight_terrain_heights);
    compute_terrain_height_feature(db, offset, Bone_RightToe, feature_weight_terrain_heights);
    compute_idle_feature(db, offset);
    compute_crouch_feature(db, offset);
    compute_jump_feature(db, offset);
    
    assert(offset == nfeatures);
    
    database_build_bounds(db);
}

// Motion Matching search function essentially consists
// of comparing every feature vector in the database, 
// against the query feature vector, first checking the 
// query distance to the axis aligned bounding boxes used 
// for the acceleration structure.
void motion_matching_search(
    int& __restrict__ best_index, 
    float& __restrict__ best_cost, 
    const slice1d<int> range_starts,
    const slice1d<int> range_stops,
    const slice2d<float> features,
    const slice1d<float> features_offset,
    const slice1d<float> features_scale,
    const slice2d<float> bound_sm_min,
    const slice2d<float> bound_sm_max,
    const slice2d<float> bound_lr_min,
    const slice2d<float> bound_lr_max,
    const slice1d<float> query_normalized,
    const float transition_cost,
    const int ignore_range_end,
    const int ignore_surrounding)
{
    int nfeatures = query_normalized.size;
    int nranges = range_starts.size;
    
    int curr_index = best_index;
    
    // Find cost for current frame
    if (best_index != -1)
    {
        best_cost = 0.0;
        for (int i = 0; i < nfeatures; i++)
        {
            best_cost += squaref(query_normalized(i) - features(best_index, i));
        }
    }
    
    float curr_cost = 0.0f;
    
    // Search rest of database
    for (int r = 0; r < nranges; r++)
    {
        // Exclude end of ranges from search    
        int i = range_starts(r);
        int range_end = range_stops(r) - ignore_range_end;
        
        while (i < range_end)
        {
            // Find index of current and next large box
            int i_lr = i / BOUND_LR_SIZE;
            int i_lr_next = (i_lr + 1) * BOUND_LR_SIZE;
            
            // Find distance to box
            curr_cost = transition_cost;
            for (int j = 0; j < nfeatures; j++)
            {
                curr_cost += squaref(query_normalized(j) - clampf(query_normalized(j), 
                    bound_lr_min(i_lr, j), bound_lr_max(i_lr, j)));
                
                if (curr_cost >= best_cost)
                {
                    break;
                }
            }
            
            // If distance is greater than current best jump to next box
            if (curr_cost >= best_cost)
            {
                i = i_lr_next;
                continue;
            }
            
            // Check against small box
            while (i < i_lr_next && i < range_end)
            {   
                // Find index of current and next small box
                int i_sm = i / BOUND_SM_SIZE;
                int i_sm_next = (i_sm + 1) * BOUND_SM_SIZE;
                
                // Find distance to box
                curr_cost = transition_cost;
                for (int j = 0; j < nfeatures; j++)
                {
                    curr_cost += squaref(query_normalized(j) - clampf(query_normalized(j), 
                        bound_sm_min(i_sm, j), bound_sm_max(i_sm, j)));
                    
                    if (curr_cost >= best_cost)
                    {
                        break;
                    }
                }
                
                // If distance is greater than current best jump to next box
                if (curr_cost >= best_cost)
                {
                    i = i_sm_next;
                    continue;
                }
                
                // Search inside small box
                while (i < i_sm_next && i < range_end)
                {
                    // Skip surrounding frames
                    if (curr_index != - 1 && abs(i - curr_index) < ignore_surrounding)
                    {
                        i++;
                        continue;
                    }
                    
                    // Check against each frame inside small box
                    curr_cost = transition_cost;
                    for (int j = 0; j < nfeatures; j++)
                    {
                        curr_cost += squaref(query_normalized(j) - features(i, j));
                        if (curr_cost >= best_cost)
                        {
                            break;
                        }
                    }
                    
                    // If cost is lower than current best then update best
                    if (curr_cost < best_cost)
                    {
                        best_index = i;
                        best_cost = curr_cost;
                    }
                    
                    i++;
                }
            }
        }
    }
}

// Search database
void database_search(
    int& best_index, 
    float& best_cost, 
    const database& db, 
    const slice1d<float> query,
    const float transition_cost = 0.0f,
    const int ignore_range_end = 20,
    const int ignore_surrounding = 20)
{
    // Normalize Query
    array1d<float> query_normalized(db.nfeatures());
    for (int i = 0; i < db.nfeatures(); i++)
    {
        query_normalized(i) = (query(i) - db.features_offset(i)) / db.features_scale(i);
    }
    
    // Search
    motion_matching_search(
        best_index, 
        best_cost, 
        db.range_starts,
        db.range_stops,
        db.features,
        db.features_offset,
        db.features_scale,
        db.bound_sm_min,
        db.bound_sm_max,
        db.bound_lr_min,
        db.bound_lr_max,
        query_normalized,
        transition_cost,
        ignore_range_end,
        ignore_surrounding);
}
