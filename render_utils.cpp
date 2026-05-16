#include "render_utils.h"

static inline Vector3 to_Vector3(vec3 v)
{
    return (Vector3){ v.x, v.y, v.z };
}

void deform_character_mesh(
  Mesh& mesh, 
  const character& c,
  const slice1d<vec3> bone_anim_positions,
  const slice1d<quat> bone_anim_rotations,
  const slice1d<int> bone_parents)
{
    linear_blend_skinning_positions(
        slice1d<vec3>(mesh.vertexCount, (vec3*)mesh.vertices),
        c.positions,
        c.bone_weights,
        c.bone_indices,
        c.bone_rest_positions,
        c.bone_rest_rotations,
        bone_anim_positions,
        bone_anim_rotations);
    
    linear_blend_skinning_normals(
        slice1d<vec3>(mesh.vertexCount, (vec3*)mesh.normals),
        c.normals,
        c.bone_weights,
        c.bone_indices,
        c.bone_rest_rotations,
        bone_anim_rotations);
    
    UpdateMeshBuffer(mesh, 0, mesh.vertices, mesh.vertexCount * 3 * sizeof(float), 0);
    UpdateMeshBuffer(mesh, 2, mesh.normals, mesh.vertexCount * 3 * sizeof(float), 0);
}

Mesh make_character_mesh(character& c)
{
    Mesh mesh = { 0 };
    
    mesh.vertexCount = c.positions.size;
    mesh.triangleCount = c.triangles.size / 3;
    mesh.vertices = (float*)MemAlloc(c.positions.size * 3 * sizeof(float));
    mesh.texcoords = (float*)MemAlloc(c.texcoords.size * 2 * sizeof(float));
    mesh.normals = (float*)MemAlloc(c.normals.size * 3 * sizeof(float));
    mesh.indices = (unsigned short*)MemAlloc(c.triangles.size * sizeof(unsigned short));
    
    memcpy(mesh.vertices, c.positions.data, c.positions.size * 3 * sizeof(float));
    memcpy(mesh.texcoords, c.texcoords.data, c.texcoords.size * 2 * sizeof(float));
    memcpy(mesh.normals, c.normals.data, c.normals.size * 3 * sizeof(float));
    memcpy(mesh.indices, c.triangles.data, c.triangles.size * sizeof(unsigned short));
    
    UploadMesh(&mesh, true);
    
    return mesh;
}

void draw_axis(const vec3 pos, const quat rot, const float scale)
{
    vec3 axis0 = pos + quat_mul_vec3(rot, scale * vec3(1.0f, 0.0f, 0.0f));
    vec3 axis1 = pos + quat_mul_vec3(rot, scale * vec3(0.0f, 1.0f, 0.0f));
    vec3 axis2 = pos + quat_mul_vec3(rot, scale * vec3(0.0f, 0.0f, 1.0f));
    
    DrawLine3D(to_Vector3(pos), to_Vector3(axis0), RED);
    DrawLine3D(to_Vector3(pos), to_Vector3(axis1), GREEN);
    DrawLine3D(to_Vector3(pos), to_Vector3(axis2), BLUE);
}

void draw_features(const feature_draw_data& f, const vec3 pos, const quat rot)
{
    const slice1d<float> features = f.features;

    vec3 lfoot_pos = quat_mul_vec3(rot, vec3(features( 0), features( 1), features( 2))) + pos;
    vec3 rfoot_pos = quat_mul_vec3(rot, vec3(features( 3), features( 4), features( 5))) + pos;
    vec3 lfoot_vel = quat_mul_vec3(rot, vec3(features( 6), features( 7), features( 8)));
    vec3 rfoot_vel = quat_mul_vec3(rot, vec3(features( 9), features(10), features(11)));
    //vec3 hip_vel   = quat_mul_vec3(rot, vec3(features(12), features(13), features(14)));
    vec3 traj0_pos = quat_mul_vec3(rot, vec3(features(15), features(16), features(17))) + pos;
    vec3 traj1_pos = quat_mul_vec3(rot, vec3(features(18), features(19), features(20))) + pos;
    vec3 traj2_pos = quat_mul_vec3(rot, vec3(features(21), features(22), features(23))) + pos;
    vec3 traj0_dir = quat_mul_vec3(rot, vec3(features(24), features(25), features(26)));
    vec3 traj1_dir = quat_mul_vec3(rot, vec3(features(27), features(28), features(29)));
    vec3 traj2_dir = quat_mul_vec3(rot, vec3(features(30), features(31), features(32)));
    
    // Draw matched feature: current foot pos (still weird)
    DrawSphereWires(to_Vector3(lfoot_pos), 0.02f, 4, 10, DARKBROWN);
    DrawSphereWires(to_Vector3(rfoot_pos), 0.02f, 4, 10, DARKBROWN);

    // Draw matched feature: trajectory pos
    DrawSphereWires(to_Vector3(traj0_pos), 0.05f, 4, 10, ORANGE);
    DrawSphereWires(to_Vector3(traj1_pos), 0.05f, 4, 10, ORANGE);
    DrawSphereWires(to_Vector3(traj2_pos), 0.05f, 4, 10, ORANGE);
    
    // Draw matched feature: foot velocity
    DrawLine3D(to_Vector3(lfoot_pos), to_Vector3(lfoot_pos + 0.1f * lfoot_vel), RED);
    DrawLine3D(to_Vector3(rfoot_pos), to_Vector3(rfoot_pos + 0.1f * rfoot_vel), RED);
    
    // Draw matched feature: trajectory direction
    DrawLine3D(to_Vector3(traj0_pos), to_Vector3(traj0_pos + 0.25f * traj0_dir), GOLD);
    DrawLine3D(to_Vector3(traj1_pos), to_Vector3(traj1_pos + 0.25f * traj1_dir), GOLD);
    DrawLine3D(to_Vector3(traj2_pos), to_Vector3(traj2_pos + 0.25f * traj2_dir), GOLD);
    
    // Draw terrain height features (4 time samples x 2 toes)
    Color terrain_colors[4] = {
        ColorAlpha(BLUE, 1.0f),    // Current frame - full opacity
        ColorAlpha(BLUE, 0.6f),    // +30 frames
        ColorAlpha(BLUE, 0.4f),    // +45 frames
        ColorAlpha(BLUE, 0.8f),   // +15 frames
    };
    
    for (int i = 0; i < 4; i++)
    {
        float left_terrain_height = f.future_terrain_heights(i).x;
        float right_terrain_height = f.future_terrain_heights(i).y;
        
        vec3 left_toe_xz;
        vec3 right_toe_xz;
        
        if (i == 0)
        {
            left_toe_xz = vec3(f.bone_positions(f.contact_bones(0)).x, 0, f.bone_positions(f.contact_bones(0)).z);
            right_toe_xz = vec3(f.bone_positions(f.contact_bones(1)).x, 0, f.bone_positions(f.contact_bones(1)).z);
        }
        else
        {
            int future_idx = i - 1;
            left_toe_xz = vec3(f.future_toe_position(future_idx * 2 + 0).x, 0, f.future_toe_position(future_idx * 2 + 0).y);
            right_toe_xz = vec3(f.future_toe_position(future_idx * 2 + 1).x, 0, f.future_toe_position(future_idx * 2 + 1).y);
            left_toe_xz = quat_mul_vec3(rot, left_toe_xz) + pos;
            right_toe_xz = quat_mul_vec3(rot, right_toe_xz) + pos;
        }

        vec3 left_terrain_pos = vec3(left_toe_xz.x, f.hip_pos.y + left_terrain_height + 0.01f, left_toe_xz.z);
        vec3 right_terrain_pos = vec3(right_toe_xz.x, f.hip_pos.y + right_terrain_height + 0.01f, right_toe_xz.z);
        
        // Draw feature: foot pos (0, +15, +30, +40 frames)
        DrawSphereWires(to_Vector3(left_terrain_pos), 0.03f, 4, 6, terrain_colors[i]);
        DrawSphereWires(to_Vector3(right_terrain_pos), 0.03f, 4, 6, terrain_colors[i]);
        
        vec3 left_hip_level = vec3(left_toe_xz.x, f.hip_pos.y, left_toe_xz.z);
        vec3 right_hip_level = vec3(right_toe_xz.x, f.hip_pos.y, right_toe_xz.z);
        
        // Draw feature: terrain height foot to hips
        DrawLine3D(to_Vector3(left_hip_level), to_Vector3(left_terrain_pos), terrain_colors[i]);
        DrawLine3D(to_Vector3(right_hip_level), to_Vector3(right_terrain_pos), terrain_colors[i]);
    }

    // Helper function definitions for drawing history features
    auto sample_runtime_history_idx = [&](int relative_offset)
    {
        if (f.root_history_positions.empty()) return 0;
        int last = (int)f.root_history_positions.size() - 1;
        return clamp(last + relative_offset, 0, last);
    };

    auto draw_history_traj_sphere = [&](int history_offset, Color c)
    {
        if (f.root_history_positions.empty() || f.root_history_rotations.empty()) return;

        int anchor_idx = sample_runtime_history_idx(history_offset);
        int traj_idx = sample_runtime_history_idx(history_offset + 20);

        vec3 root_pos_history = f.root_history_positions[anchor_idx];
        quat root_rot_history = f.root_history_rotations[anchor_idx];
        vec3 htraj_pos = f.root_history_positions[traj_idx];

        // Draw feature: future trajectory
        Color c_faint = ColorAlpha(c, 0.55f);
        DrawSphereWires(to_Vector3(root_pos_history), 0.03f, 4, 6, c_faint);
        DrawLine3D(to_Vector3(root_pos_history), to_Vector3(htraj_pos), c_faint);
        DrawSphereWires(to_Vector3(htraj_pos), 0.07f, 6, 12, c);

        vec3 history_traj_local = quat_inv_mul_vec3(root_rot_history, htraj_pos - root_pos_history);
        vec3 history_traj_dir = quat_inv_mul_vec3(
            root_rot_history,
            quat_mul_vec3(f.root_history_rotations[traj_idx], vec3(0.0f, 0.0f, 1.0f)));

        const float eps = 1e-4f;
        float h = length(vec3(history_traj_local.x, 0.0f, history_traj_local.z));
        history_traj_dir.y = history_traj_local.y / maxf(h, eps);
        history_traj_dir = normalize(history_traj_dir);

        vec3 history_traj_dir_world = quat_mul_vec3(root_rot_history, history_traj_dir);
        DrawLine3D(to_Vector3(htraj_pos), to_Vector3(htraj_pos + 0.25f * history_traj_dir_world), c_faint);
    };

    auto draw_history_bone_position_sphere = [&](const std::vector<vec3>& history_positions, int history_offset, Color c, float radius = 0.05f)
    {
        if (f.root_history_positions.empty() || history_positions.empty()) return;

        int idx = sample_runtime_history_idx(history_offset);
        vec3 root_pos_history = f.root_history_positions[idx];

        // History positions are already in global coordinates
        vec3 feature_world = history_positions[idx];

        Color c_faint = ColorAlpha(c, 0.45f);
        DrawLine3D(to_Vector3(root_pos_history), to_Vector3(feature_world), c_faint);
        // Draw feature: history pos (can be any bone)
        DrawSphereWires(to_Vector3(feature_world), radius, 4, 10, c);
    };

    auto draw_history_velocity_sphere = [&](const std::vector<vec3>& history_positions, const std::vector<vec3>& history_velocities, int history_offset, Color c)
    {
        if (f.root_history_positions.empty() || history_velocities.empty()) return;

        int idx = sample_runtime_history_idx(history_offset);

        // History positions and velocities are already in global coordinates
        vec3 pos_world = history_positions[idx];
        vec3 vel_world = history_velocities[idx];

        Color c_faint = ColorAlpha(c, 0.45f);
        DrawLine3D(to_Vector3(pos_world), to_Vector3(pos_world + 0.1f * vel_world), c_faint);
        // Draw feature: history direction (trajectory direction)
        DrawSphereWires(to_Vector3(pos_world + 0.1f * vel_world), 0.03f, 4, 6, c);
    };

    auto draw_history_terrain_sphere = [&](const std::vector<vec2>& h_terrain, int history_offset, Color c)
    {
        if (f.root_history_positions.empty() || h_terrain.empty()) return;

        int idx = sample_runtime_history_idx(history_offset);

        // History positions are already in global coordinates
        vec3 l_pos_world = f.history_left_foot_positions[idx];
        vec3 r_pos_world = f.history_right_foot_positions[idx];
        vec3 h_pos_world = f.history_hip_positions[idx];
        float l_terrain_h = h_terrain[idx].x;
        float r_terrain_h = h_terrain[idx].y;

        vec3 l_terrain_pos = vec3(l_pos_world.x, h_pos_world.y + l_terrain_h + 0.01f, l_pos_world.z);
        vec3 r_terrain_pos = vec3(r_pos_world.x, h_pos_world.y + r_terrain_h + 0.01f, r_pos_world.z);

        Color c_faint = ColorAlpha(c, 0.45f);
        // Draw feature: history foot position
        DrawSphereWires(to_Vector3(l_terrain_pos), 0.02f, 4, 6, c);
        DrawSphereWires(to_Vector3(r_terrain_pos), 0.02f, 4, 6, c);
        DrawLine3D(to_Vector3(vec3(l_pos_world.x, h_pos_world.y, l_pos_world.z)), to_Vector3(l_terrain_pos), c_faint);
        DrawLine3D(to_Vector3(vec3(r_pos_world.x, h_pos_world.y, r_pos_world.z)), to_Vector3(r_terrain_pos), c_faint);
    };

    // Function call draw history features
    
    // int history_offsets[3] = { -20, -10, -5 };
    int history_offsets[1] = { -20 };

    Color hc = ColorAlpha(GREEN, 0.4f);

    // Draw feature: history tarjectory pos
    draw_history_traj_sphere(-20, hc);
    
    // Draw feature: history foot pos
    draw_history_bone_position_sphere(f.history_left_foot_positions, -20, hc, 0.04f);
    draw_history_bone_position_sphere(f.history_right_foot_positions, -20, hc, 0.06f);
    
    // Draw feature: history foot vel
    draw_history_velocity_sphere(f.history_left_foot_positions, f.history_left_foot_velocities, -20, hc);
    draw_history_velocity_sphere(f.history_right_foot_positions, f.history_right_foot_velocities, -20, hc);
    
    // Draw feature: history hip pos
    draw_history_bone_position_sphere(f.history_hip_positions, -20, hc, 0.03f);
    
    // Draw feature: history hip vel
    draw_history_velocity_sphere(f.history_hip_positions, f.history_hip_velocities, -20, hc);
    
    // Draw feature: history terrain heights
    draw_history_terrain_sphere(f.history_terrain_heights, -15, hc);
    
}

void draw_trajectory(
    const slice1d<vec3> trajectory_positions, 
    const slice1d<quat> trajectory_rotations, 
    const Color color)
{
    for (int i = 1; i < trajectory_positions.size; i++)
    {
        // Draw feature: future trajectory position
        DrawSphereWires(to_Vector3(trajectory_positions(i)), 0.05f, 4, 10, color);
        DrawLine3D(to_Vector3(trajectory_positions(i)), to_Vector3(
            trajectory_positions(i) + 0.6f * quat_mul_vec3(trajectory_rotations(i), vec3(0, 0, 1.0f))), color);
        DrawLine3D(to_Vector3(trajectory_positions(i-1)), to_Vector3(trajectory_positions(i)), color);
    }
}

void draw_stickman(
    const slice1d<vec3> global_bone_positions,
    const slice1d<int> bone_parents,
    const Color color)
{
    // Draw spheres at each joint
    for (int i = 0; i < global_bone_positions.size; i++)
    {
        DrawSphere(to_Vector3(global_bone_positions(i)), 0.02f, color);
    }
    
    // Draw lines connecting bones to their parents
    for (int i = 1; i < bone_parents.size; i++)
    {
        int parent = bone_parents(i);
        if (parent != -1)
        {
            DrawLine3D(
                to_Vector3(global_bone_positions(i)),
                to_Vector3(global_bone_positions(parent)),
                color);
        }
    }
}
