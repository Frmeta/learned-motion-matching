#define RAYGUI_IMPLEMENTATION
#include "raygui.h"
#if defined(PLATFORM_WEB)
#include <emscripten/emscripten.h>
#include <emscripten/heap.h>
#endif

#include "raylib.h"
#include "raymath.h"
#include "common.h"
#include "vec.h"
#include "quat.h"
#include "spring.h"
#include "array.h"
#include "character.h"
#include "database.h"
#include "nnet.h"
#include "lmm.h"

#include <initializer_list>
#include <functional>
#include <iostream> // TODO: Remove this when not used
#include <cstring>
#include <ctime>
#include <string>
#include <algorithm>
#include <cmath>
#include <vector>
#include <chrono>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef NOGDI
#define NOGDI
#endif
#ifndef NOUSER
#define NOUSER
#endif
#include <windows.h>
#include <psapi.h>
#endif

static constexpr bool debug = false;

#if defined(_WIN32)
struct runtime_metrics
{
    float cpu_percent = 0.0f;
    float gpu_percent = -1.0f; // Not available via raylib/OpenGL without platform-specific APIs.
    float process_memory_mb = 0.0f;
    float system_memory_percent = 0.0f;

    unsigned long long last_proc_time_100ns = 0;
    unsigned long long last_wall_time_100ns = 0;
    unsigned int cpu_count = 1;
};

static unsigned long long filetime_to_u64(const FILETIME& ft)
{
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

static void runtime_metrics_init(runtime_metrics& m)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    m.cpu_count = si.dwNumberOfProcessors > 0 ? si.dwNumberOfProcessors : 1;

    FILETIME create_time, exit_time, kernel_time, user_time;
    if (GetProcessTimes(GetCurrentProcess(), &create_time, &exit_time, &kernel_time, &user_time))
    {
        m.last_proc_time_100ns = filetime_to_u64(kernel_time) + filetime_to_u64(user_time);
    }

    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    m.last_wall_time_100ns = filetime_to_u64(now);
}

static void runtime_metrics_update(runtime_metrics& m)
{
    FILETIME create_time, exit_time, kernel_time, user_time;
    FILETIME now;

    if (GetProcessTimes(GetCurrentProcess(), &create_time, &exit_time, &kernel_time, &user_time))
    {
        unsigned long long proc_now = filetime_to_u64(kernel_time) + filetime_to_u64(user_time);
        GetSystemTimeAsFileTime(&now);
        unsigned long long wall_now = filetime_to_u64(now);

        unsigned long long proc_delta = proc_now - m.last_proc_time_100ns;
        unsigned long long wall_delta = wall_now - m.last_wall_time_100ns;

        if (wall_delta > 0)
        {
            double cpu = (100.0 * (double)proc_delta) / ((double)wall_delta * (double)m.cpu_count);
            if (cpu < 0.0) cpu = 0.0;
            if (cpu > 100.0) cpu = 100.0;
            m.cpu_percent = (float)cpu;
        }

        m.last_proc_time_100ns = proc_now;
        m.last_wall_time_100ns = wall_now;
    }

    PROCESS_MEMORY_COUNTERS pmc;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
    {
        m.process_memory_mb = (float)pmc.WorkingSetSize / (1024.0f * 1024.0f);
    }

    MEMORYSTATUSEX mem_status;
    mem_status.dwLength = sizeof(mem_status);
    if (GlobalMemoryStatusEx(&mem_status))
    {
        m.system_memory_percent = (float)mem_status.dwMemoryLoad;
    }
}

static float get_process_memory_mb()
{
    PROCESS_MEMORY_COUNTERS pmc;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
    {
        return (float)pmc.WorkingSetSize / (1024.0f * 1024.0f);
    }
    return -1.0f;
}
#endif

#if defined(PLATFORM_WEB)
EM_JS(double, web_get_js_heap_used_mb, (), {
    if (typeof performance !== 'undefined' && performance.memory) {
        return performance.memory.usedJSHeapSize / (1024.0 * 1024.0);
    }
    return -1.0;
});

EM_JS(double, web_get_js_heap_total_mb, (), {
    if (typeof performance !== 'undefined' && performance.memory) {
        return performance.memory.jsHeapSizeLimit / (1024.0 * 1024.0);
    }
    return -1.0;
});

struct runtime_metrics
{
    float cpu_percent = 0.0f;
    float gpu_percent = -1.0f; // Not exposed reliably in browser without specialized extensions.
    float process_memory_mb = 0.0f;
    float system_memory_percent = -1.0f;
};

static void runtime_metrics_init(runtime_metrics& m)
{
    m.cpu_percent = 0.0f;
    m.gpu_percent = -1.0f;
    m.process_memory_mb = 0.0f;
    m.system_memory_percent = -1.0f;
}

static void runtime_metrics_update(runtime_metrics& m, const float frame_time_ms)
{
    const float target_frame_ms = 1000.0f / 60.0f;
    m.cpu_percent = clampf((frame_time_ms / target_frame_ms) * 100.0f, 0.0f, 100.0f);
    m.gpu_percent = -1.0f;
    m.process_memory_mb = (float)emscripten_get_heap_size() / (1024.0f * 1024.0f);

    const double js_used_mb = web_get_js_heap_used_mb();
    const double js_total_mb = web_get_js_heap_total_mb();
    if (js_used_mb >= 0.0 && js_total_mb > 0.0)
    {
        m.system_memory_percent = (float)((js_used_mb / js_total_mb) * 100.0);
    }
    else
    {
        m.system_memory_percent = -1.0f;
    }
}
#endif

// Rebuild features when they do not exist yet, or when database.bin is newer.
static bool should_rebuild_features(const char* database_path, const char* features_path)
{
    struct stat db_info;
    if (stat(database_path, &db_info) != 0)
    {
        // If database is missing/unreadable, keep previous behavior and attempt build.
        return true;
    }

    struct stat features_info;
    if (stat(features_path, &features_info) != 0)
    {
        return true;
    }

    return db_info.st_mtime > features_info.st_mtime;
}

static int matching_feature_count_expected()
{
    return
        3 + // Left Foot Position
        3 + // Right Foot Position
        3 + // Left Foot Velocity
        3 + // Right Foot Velocity
        3 + // Hip Velocity
        9 + // Trajectory Positions
        9 + // Trajectory Directions
        8 + // Terrain Heights (left+right, 4 samples each)

        // Flag
        1 + // Idle Flag
        1 + // Crouch Flag
        1 + // Jump Flag
        1 + // Cartwheel Flag

        // History
        3 + // History Left Foot Position (-20)
        3 + // History Right Foot Position (-20)
        3 + // History Left Foot Velocity (-20)
        3 + // History Right Foot Velocity (-20)
        3 + // History Hip Velocity (-20)
        3 + // History Trajectory Position (-20)
        3 + // History Trajectory Direction (-20)
        2;  // History Terrain Heights (-15)
}

struct joystick_record_sample
{
    int frame = 0;
    float time_seconds = 0.0f;
    vec3 left_stick;
    vec3 right_stick;
    vec3 player_position;
};

struct range_metadata_entry
{
    int range_index = -1;
    int db_start = 0;
    int db_stop = 0;
    char bvh_name[256] = "";
    int source_start = 0;
    int source_stop = 0;
    bool is_mirrored = false;
};

static bool save_joystick_recording_csv(
    const char* filename,
    const std::vector<joystick_record_sample>& samples)
{
    FILE* f = fopen(filename, "w");
    if (f == NULL)
    {
        return false;
    }

    fprintf(f, "frame,time_seconds,left_x,left_z,right_x,right_z,player_x,player_y,player_z\n");

    for (size_t i = 0; i < samples.size(); i++)
    {
        const joystick_record_sample& s = samples[i];
        fprintf(
            f,
            "%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
            s.frame,
            s.time_seconds,
            s.left_stick.x,
            s.left_stick.z,
            s.right_stick.x,
            s.right_stick.z,
            s.player_position.x,
            s.player_position.y,
            s.player_position.z);
    }

    fclose(f);
    return true;
}

static bool load_joystick_recording_csv(
    const char* filename,
    std::vector<joystick_record_sample>& samples)
{
    FILE* f = fopen(filename, "r");
    if (f == NULL)
    {
        return false;
    }

    samples.clear();

    char line[512];
    bool first_line = true;

    while (fgets(line, sizeof(line), f) != NULL)
    {
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '\0')
        {
            continue;
        }

        if (first_line)
        {
            first_line = false;
            if (strstr(line, "frame,time_seconds") != NULL)
            {
                continue;
            }
        }

        joystick_record_sample s;
        float left_x, left_z, right_x, right_z;
        float player_x, player_y, player_z;

        int parsed = sscanf(
            line,
            "%d,%f,%f,%f,%f,%f,%f,%f,%f",
            &s.frame,
            &s.time_seconds,
            &left_x,
            &left_z,
            &right_x,
            &right_z,
            &player_x,
            &player_y,
            &player_z);

        if (parsed == 9)
        {
            s.left_stick = vec3(left_x, 0.0f, left_z);
            s.right_stick = vec3(right_x, 0.0f, right_z);
            s.player_position = vec3(player_x, player_y, player_z);
            samples.push_back(s);
        }
    }

    fclose(f);
    return !samples.empty();
}

static bool load_range_metadata_csv(
    const char* filename,
    std::vector<range_metadata_entry>& entries)
{
    FILE* f = fopen(filename, "r");
    if (f == NULL)
    {
        return false;
    }

    entries.clear();

    char line[1024];
    bool first_line = true;

    while (fgets(line, sizeof(line), f) != NULL)
    {
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '\0')
        {
            continue;
        }

        if (first_line)
        {
            first_line = false;
            if (strstr(line, "range_index,db_start") != NULL)
            {
                continue;
            }
        }

        range_metadata_entry entry;
        int is_mirrored_int = 0;

        int parsed = sscanf(
            line,
            "%d,%d,%d,%255[^,],%d,%d,%d",
            &entry.range_index,
            &entry.db_start,
            &entry.db_stop,
            entry.bvh_name,
            &entry.source_start,
            &entry.source_stop,
            &is_mirrored_int);

        if (parsed == 7)
        {
            entry.is_mirrored = is_mirrored_int != 0;
            entries.push_back(entry);
        }
    }

    fclose(f);
    return !entries.empty();
}

static std::string joystick_recording_timestamp_string()
{
    std::time_t now = std::time(NULL);
    std::tm local_now = {};

#if defined(_WIN32)
    localtime_s(&local_now, &now);
#else
    local_now = *std::localtime(&now);
#endif

    char timestamp[32];
    std::strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &local_now);
    return std::string(timestamp);
}

static std::string joystick_recording_make_output_path(const char* folder)
{
    std::string base_folder = (folder != NULL && folder[0] != '\0') ? folder : ".";

    while (!base_folder.empty() &&
           (base_folder.back() == '/' || base_folder.back() == '\\'))
    {
        base_folder.pop_back();
    }

    if (base_folder.empty())
    {
        base_folder = ".";
    }

    return base_folder + "/joystick_recording_" + joystick_recording_timestamp_string() + ".csv";
}

static void joystick_recording_refresh_csv_files(
    const char* folder,
    std::vector<std::string>& files)
{
    files.clear();

#if defined(_WIN32)
    char search_pattern[768];
    snprintf(search_pattern, sizeof(search_pattern), "%s/*.csv", folder);

    WIN32_FIND_DATAA find_data;
    HANDLE find_handle = FindFirstFileA(search_pattern, &find_data);
    if (find_handle == INVALID_HANDLE_VALUE)
    {
        return;
    }

    do
    {
        if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            files.push_back(find_data.cFileName);
        }
    }
    while (FindNextFileA(find_handle, &find_data));

    FindClose(find_handle);
#endif

    std::sort(files.begin(), files.end());
}

static void joystick_recording_build_dropdown_text(
    const std::vector<std::string>& files,
    char* output,
    size_t output_size)
{
    if (output_size == 0)
    {
        return;
    }

    output[0] = '\0';

    if (files.empty())
    {
        snprintf(output, output_size, "<no csv files>");
        return;
    }

    for (size_t i = 0; i < files.size(); i++)
    {
        if (i > 0)
        {
            strncat(output, ";", output_size - strlen(output) - 1);
        }
        strncat(output, files[i].c_str(), output_size - strlen(output) - 1);
    }
}

//--------------------------------------

static inline Vector3 to_Vector3(vec3 v)
{
    return (Vector3){ v.x, v.y, v.z };
}

static inline vec3 from_Vector3(Vector3 v)
{
    return vec3(v.x, v.y, v.z);
}

static constexpr float kTerrainFollowMaxVerticalSpeed = 10.0f;
static constexpr float kTerrainFollowMinVerticalSpeed = -10.0f;

//--------------------------------------

// Perform linear blend skinning and copy 
// result into mesh data. Update and upload 
// deformed vertex positions and normals to GPU
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

//--------------------------------------

// Basic functionality to get gamepad input including deadzone and 
// squaring of the stick location to increase sensitivity. To make 
// all the other code that uses this easier, we assume stick is 
// oriented on floor (i.e. y-axis is zero)

enum
{
    GAMEPAD_PLAYER = 0,
};

enum
{
    GAMEPAD_STICK_LEFT,
    GAMEPAD_STICK_RIGHT,
};

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

vec3 gamepad_get_stick(int stick, const float deadzone = 0.2f)
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

//--------------------------------------

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

// Updates the camera using the orbit cam controls
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

//--------------------------------------

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
    const float gait_change_halflife = 0.1f)
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

//--------------------------------------

// Moving the root is a little bit difficult when we have the
// inertializer set up in the way we do. Essentially we need
// to also make sure to adjust all of the locations where 
// we are transforming the data to and from as well as the 
// offsets being blended out
void inertialize_root_adjust(
    vec3& offset_position,
    vec3& transition_src_position,
    quat& transition_src_rotation,
    vec3& transition_dst_position,
    quat& transition_dst_rotation,
    vec3& position,
    quat& rotation,
    const vec3 input_position,
    const quat input_rotation)
{
    // Find the position difference and add it to the state and transition location
    vec3 position_difference = input_position - position;
    position = position_difference + position;
    transition_dst_position = position_difference + transition_dst_position;
    
    // Find the point at which we want to now transition from in the src data
    transition_src_position = transition_src_position + quat_mul_vec3(transition_src_rotation,
        quat_inv_mul_vec3(transition_dst_rotation, position - offset_position - transition_dst_position));
    transition_dst_position = position;
    offset_position = vec3();
    
    // Find the rotation difference. We need to normalize here or some error can accumulate 
    // over time during adjustment.
    quat rotation_difference = quat_normalize(quat_mul_inv(input_rotation, rotation));
    
    // Apply the rotation difference to the current rotation and transition location
    rotation = quat_mul(rotation_difference, rotation);
    transition_dst_rotation = quat_mul(rotation_difference, transition_dst_rotation);
}

void inertialize_pose_reset(
    slice1d<vec3> bone_offset_positions,
    slice1d<vec3> bone_offset_velocities,
    slice1d<quat> bone_offset_rotations,
    slice1d<vec3> bone_offset_angular_velocities,
    vec3& transition_src_position,
    quat& transition_src_rotation,
    vec3& transition_dst_position,
    quat& transition_dst_rotation,
    const vec3 root_position,
    const quat root_rotation)
{
    bone_offset_positions.zero();
    bone_offset_velocities.zero();
    bone_offset_rotations.set(quat());
    bone_offset_angular_velocities.zero();
    
    transition_src_position = root_position;
    transition_src_rotation = root_rotation;
    transition_dst_position = vec3();
    transition_dst_rotation = quat();
}

// This function transitions the inertializer for 
// the full character. It takes as input the current 
// offsets, as well as the root transition locations,
// current root state, and the full pose information 
// for the pose being transitioned from (src) as well 
// as the pose being transitioned to (dst) in their
// own animation spaces.
void inertialize_pose_transition(
    slice1d<vec3> bone_offset_positions,
    slice1d<vec3> bone_offset_velocities,
    slice1d<quat> bone_offset_rotations,
    slice1d<vec3> bone_offset_angular_velocities,
    vec3& transition_src_position,
    quat& transition_src_rotation,
    vec3& transition_dst_position,
    quat& transition_dst_rotation,
    const vec3 root_position,
    const vec3 root_velocity,
    const quat root_rotation,
    const vec3 root_angular_velocity,
    const slice1d<vec3> bone_src_positions,
    const slice1d<vec3> bone_src_velocities,
    const slice1d<quat> bone_src_rotations,
    const slice1d<vec3> bone_src_angular_velocities,
    const slice1d<vec3> bone_dst_positions,
    const slice1d<vec3> bone_dst_velocities,
    const slice1d<quat> bone_dst_rotations,
    const slice1d<vec3> bone_dst_angular_velocities)
{
    // First we record the root position and rotation
    // in the animation data for the source and destination
    // animation
    transition_dst_position = root_position;
    transition_dst_rotation = root_rotation;
    transition_src_position = bone_dst_positions(0);
    transition_src_rotation = bone_dst_rotations(0);
    
    // We then find the velocities so we can transition the 
    // root inertiaizers
    vec3 world_space_dst_velocity = quat_mul_vec3(transition_dst_rotation, 
        quat_inv_mul_vec3(transition_src_rotation, bone_dst_velocities(0)));
    
    vec3 world_space_dst_angular_velocity = quat_mul_vec3(transition_dst_rotation, 
        quat_inv_mul_vec3(transition_src_rotation, bone_dst_angular_velocities(0)));
    
    // Transition inertializers recording the offsets for 
    // the root joint
    inertialize_transition(
        bone_offset_positions(0),
        bone_offset_velocities(0),
        root_position,
        root_velocity,
        root_position,
        world_space_dst_velocity);
        
    inertialize_transition(
        bone_offset_rotations(0),
        bone_offset_angular_velocities(0),
        root_rotation,
        root_angular_velocity,
        root_rotation,
        world_space_dst_angular_velocity);
    
    // Transition all the inertializers for each other bone
    for (int i = 1; i < bone_offset_positions.size; i++)
    {
        inertialize_transition(
            bone_offset_positions(i),
            bone_offset_velocities(i),
            bone_src_positions(i),
            bone_src_velocities(i),
            bone_dst_positions(i),
            bone_dst_velocities(i));
            
        inertialize_transition(
            bone_offset_rotations(i),
            bone_offset_angular_velocities(i),
            bone_src_rotations(i),
            bone_src_angular_velocities(i),
            bone_dst_rotations(i),
            bone_dst_angular_velocities(i));
    }
}

// This function updates the inertializer states. Here 
// it outputs the smoothed animation (input plus offset) 
// as well as updating the offsets themselves. It takes 
// as input the current playing animation as well as the 
// root transition locations, a halflife, and a dt
void inertialize_pose_update(
    slice1d<vec3> bone_positions,
    slice1d<vec3> bone_velocities,
    slice1d<quat> bone_rotations,
    slice1d<vec3> bone_angular_velocities,
    slice1d<vec3> bone_offset_positions,
    slice1d<vec3> bone_offset_velocities,
    slice1d<quat> bone_offset_rotations,
    slice1d<vec3> bone_offset_angular_velocities,
    const slice1d<vec3> bone_input_positions,
    const slice1d<vec3> bone_input_velocities,
    const slice1d<quat> bone_input_rotations,
    const slice1d<vec3> bone_input_angular_velocities,
    const vec3 transition_src_position,
    const quat transition_src_rotation,
    const vec3 transition_dst_position,
    const quat transition_dst_rotation,
    const float halflife,
    const float dt)
{
    // First we find the next root position, velocity, rotation
    // and rotational velocity in the world space by transforming 
    // the input animation from it's animation space into the 
    // space of the currently playing animation.
    vec3 world_space_position = quat_mul_vec3(transition_dst_rotation, 
        quat_inv_mul_vec3(transition_src_rotation, 
            bone_input_positions(0) - transition_src_position)) + transition_dst_position;
    
    vec3 world_space_velocity = quat_mul_vec3(transition_dst_rotation, 
        quat_inv_mul_vec3(transition_src_rotation, bone_input_velocities(0)));
    
    // Normalize here because quat inv mul can sometimes produce 
    // unstable returns when the two rotations are very close.
    quat world_space_rotation = quat_normalize(quat_mul(transition_dst_rotation, 
        quat_inv_mul(transition_src_rotation, bone_input_rotations(0))));
    
    vec3 world_space_angular_velocity = quat_mul_vec3(transition_dst_rotation, 
        quat_inv_mul_vec3(transition_src_rotation, bone_input_angular_velocities(0)));
    
    // Then we update these two inertializers with these new world space inputs
    inertialize_update(
        bone_positions(0),
        bone_velocities(0),
        bone_offset_positions(0),
        bone_offset_velocities(0),
        world_space_position,
        world_space_velocity,
        halflife,
        dt);
        
    inertialize_update(
        bone_rotations(0),
        bone_angular_velocities(0),
        bone_offset_rotations(0),
        bone_offset_angular_velocities(0),
        world_space_rotation,
        world_space_angular_velocity,
        halflife,
        dt);        
    
    // Then we update the inertializers for the rest of the bones
    for (int i = 1; i < bone_positions.size; i++)
    {
        inertialize_update(
            bone_positions(i),
            bone_velocities(i),
            bone_offset_positions(i),
            bone_offset_velocities(i),
            bone_input_positions(i),
            bone_input_velocities(i),
            halflife,
            dt);
            
        inertialize_update(
            bone_rotations(i),
            bone_angular_velocities(i),
            bone_offset_rotations(i),
            bone_offset_angular_velocities(i),
            bone_input_rotations(i),
            bone_input_angular_velocities(i),
            halflife,
            dt);
    }
}

//--------------------------------------

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
        // std::cout << future_terrain_heights(i).x << std::endl;
    }
    
    // Then all right toe heights (4 time samples)
    for (int i = 0; i < 4; i++)
    {
        query(offset + 4 + i) = future_terrain_heights(i).y; // Right toe heights at t0, t1, t2, t3
    }
    
    offset += 8;
}

//--------------------------------------

static bool sample_terrain_height(
    const Model& ground_plane_model,
    const vec3 position,
    float& out_height)
{
    bool hit = false;
    float highest = 0.0f;
    Ray ray = { to_Vector3(position + vec3(0.0f, 20.0f, 0.0f)), {0.0f, -1.0f, 0.0f} };

    for (int i = 0; i < ground_plane_model.meshCount; i++)
    {
        RayCollision collision = GetRayCollisionMesh(ray, ground_plane_model.meshes[i], ground_plane_model.transform);
        if (collision.hit && (!hit || collision.point.y > highest))
        {
            highest = collision.point.y;
            hit = true;
        }
    }

    if (hit)
    {
        out_height = highest;
    }

    return hit;
}

static void clamp_position_min_terrain_y(
    vec3& position,
    const Model& ground_plane_model,
    const float terrain_height_offset)
{
    float terrain_height = 0.0f;
    if (sample_terrain_height(ground_plane_model, position, terrain_height))
    {
        position.y = maxf(position.y, terrain_height + terrain_height_offset);
    }
}

//--------------------------------------

// Closest-point-on-triangle from Real-Time Collision Detection (Christer Ericson).
static vec3 closest_point_on_triangle(const vec3 p, const vec3 a, const vec3 b, const vec3 c)
{
    vec3 ab = b - a;
    vec3 ac = c - a;
    vec3 ap = p - a;

    float d1 = dot(ab, ap);
    float d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;

    vec3 bp = p - b;
    float d3 = dot(ab, bp);
    float d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;

    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
    {
        float v = d1 / (d1 - d3);
        return a + v * ab;
    }

    vec3 cp = p - c;
    float d5 = dot(ab, cp);
    float d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;

    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
    {
        float w = d2 / (d2 - d6);
        return a + w * ac;
    }

    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
    {
        vec3 bc = c - b;
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return b + w * bc;
    }

    float denom = 1.0f / (va + vb + vc);
    float v = vb * denom;
    float w = vc * denom;
    return a + v * ab + w * ac;
}

static bool nearest_point_on_model(
    const Model& obstacle_model,
    const vec3 point,
    vec3& out_nearest,
    vec3& out_normal)
{
    bool found = false;
    float best_dist_sq = FLT_MAX;

    for (int mesh_idx = 0; mesh_idx < obstacle_model.meshCount; mesh_idx++)
    {
        const Mesh& mesh = obstacle_model.meshes[mesh_idx];
        const float* vertices = mesh.vertices;
        const unsigned short* indices = mesh.indices;

        if (vertices == nullptr)
        {
            continue;
        }

        int tri_count = mesh.triangleCount;
        for (int tri = 0; tri < tri_count; tri++)
        {
            int i0 = 0;
            int i1 = 0;
            int i2 = 0;

            if (indices)
            {
                i0 = indices[tri * 3 + 0];
                i1 = indices[tri * 3 + 1];
                i2 = indices[tri * 3 + 2];
            }
            else
            {
                i0 = tri * 3 + 0;
                i1 = tri * 3 + 1;
                i2 = tri * 3 + 2;
            }

            Vector3 a_v = { vertices[i0 * 3 + 0], vertices[i0 * 3 + 1], vertices[i0 * 3 + 2] };
            Vector3 b_v = { vertices[i1 * 3 + 0], vertices[i1 * 3 + 1], vertices[i1 * 3 + 2] };
            Vector3 c_v = { vertices[i2 * 3 + 0], vertices[i2 * 3 + 1], vertices[i2 * 3 + 2] };

            vec3 a = from_Vector3(Vector3Transform(a_v, obstacle_model.transform));
            vec3 b = from_Vector3(Vector3Transform(b_v, obstacle_model.transform));
            vec3 c = from_Vector3(Vector3Transform(c_v, obstacle_model.transform));

            vec3 nearest = closest_point_on_triangle(point, a, b, c);
            vec3 delta = point - nearest;
            float dist_sq = dot(delta, delta);

            if (dist_sq < best_dist_sq)
            {
                best_dist_sq = dist_sq;
                out_nearest = nearest;

                vec3 tri_normal = cross(b - a, c - a);
                if (dot(tri_normal, tri_normal) > 1e-10f)
                {
                    out_normal = normalize(tri_normal);
                }
                else
                {
                    out_normal = vec3(0.0f, 1.0f, 0.0f);
                }

                found = true;
            }
        }
    }

    return found;
}

// Collide against the obstacle model by finding nearest point on mesh surface and pushing out.
vec3 simulation_collide_obstacles(
    const vec3 prev_pos,
    const vec3 next_pos,
    const Model& obstacle_model,
    const float radius = 0.8f)
{
    vec3 dx = next_pos - prev_pos;
    vec3 proj_pos = prev_pos;
    
    // Substep because I'm too lazy to implement CCD
    int substeps = 1 + (int)(length(dx) * 5.0f);
    
    for (int j = 0; j < substeps; j++)
    {
        proj_pos = proj_pos + dx / substeps;

        vec3 nearest;
        vec3 nearest_normal;
        if (nearest_point_on_model(obstacle_model, proj_pos, nearest, nearest_normal))
        {
            vec3 delta = proj_pos - nearest;
            float dist_sq = dot(delta, delta);
            if (dist_sq < radius * radius)
            {
                if (dist_sq > 1e-10f)
                {
                    proj_pos = nearest + radius * normalize(delta);
                }
                else
                {
                    proj_pos = nearest + radius * nearest_normal;
                }
            }
        }
    } 
    
    return proj_pos;
}

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

        if (jump_active)
        {
            float predicted_vy = jump_vertical_velocity - jump_gravity * (i * dt);
            desired_velocities(i).y = clampf(predicted_vy, kTerrainFollowMinVerticalSpeed, kTerrainFollowMaxVerticalSpeed);
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

//--------------------------------------

void contact_reset(
    bool& contact_state,
    bool& contact_lock,
    vec3& contact_position,
    vec3& contact_velocity,
    vec3& contact_point,
    vec3& contact_target,
    vec3& contact_offset_position,
    vec3& contact_offset_velocity,
    const vec3 input_contact_position,
    const vec3 input_contact_velocity,
    const bool input_contact_state)
{
    contact_state = false;
    contact_lock = false;
    contact_position = input_contact_position;
    contact_velocity = input_contact_velocity;
    contact_point = input_contact_position;
    contact_target = input_contact_position;
    contact_offset_position = vec3();
    contact_offset_velocity = vec3();
}

void contact_update(
    bool& contact_state,
    bool& contact_lock,
    vec3& contact_position,
    vec3& contact_velocity,
    vec3& contact_point,
    vec3& contact_target,
    vec3& contact_offset_position,
    vec3& contact_offset_velocity,
    const vec3 input_contact_position,
    const bool input_contact_state,
    const float unlock_radius,
    const float foot_height,
    const float halflife,
    const float dt,
    const float eps=1e-8)
{
    // First compute the input contact position velocity via finite difference
    vec3 input_contact_velocity = 
        (input_contact_position - contact_target) / (dt + eps);    
    contact_target = input_contact_position;
    
    // Update the inertializer to tick forward in time
    inertialize_update(
        contact_position,
        contact_velocity,
        contact_offset_position,
        contact_offset_velocity,
        // If locked we feed the contact point and zero velocity, 
        // otherwise we feed the input from the animation
        contact_lock ? contact_point : input_contact_position,
        contact_lock ?        vec3() : input_contact_velocity,
        halflife,
        dt);
    
    // If the contact point is too far from the current input position 
    // then we need to unlock the contact
    bool unlock_contact = contact_lock && (
        length(contact_point - input_contact_position) > unlock_radius);
    
    // If the contact was previously inactive but is now active we 
    // need to transition to the locked contact state
    if (!contact_state && input_contact_state)
    {
        // Contact point is given by the current position of 
        // the foot projected onto the ground plus foot height
        contact_lock = true;
        contact_point = contact_position;
        contact_point.y = foot_height;
        
        inertialize_transition(
            contact_offset_position,
            contact_offset_velocity,
            input_contact_position,
            input_contact_velocity,
            contact_point,
            vec3());
    }
    
    // Otherwise if we need to unlock or we were previously in 
    // contact but are no longer we transition to just taking 
    // the input position as-is
    else if ((contact_lock && contact_state && !input_contact_state) 
         || unlock_contact)
    {
        contact_lock = false;
        
        inertialize_transition(
            contact_offset_position,
            contact_offset_velocity,
            contact_point,
            vec3(),
            input_contact_position,
            input_contact_velocity);
    }
    
    // Update contact state
    contact_state = input_contact_state;
}

//--------------------------------------

// Rotate a joint to look toward some 
// given target position
void ik_look_at(
    quat& bone_rotation,
    const quat global_parent_rotation,
    const quat global_rotation,
    const vec3 global_position,
    const vec3 child_position,
    const vec3 target_position,
    const float eps = 1e-5f)
{
    vec3 curr_dir = normalize(child_position - global_position);
    vec3 targ_dir = normalize(target_position - global_position);

    if (fabs(1.0f - dot(curr_dir, targ_dir) > eps))
    {
        bone_rotation = quat_inv_mul(global_parent_rotation, 
            quat_mul(quat_between(curr_dir, targ_dir), global_rotation));
    }
}

// Basic two-joint IK in the style of https://theorangeduck.com/page/simple-two-joint
// Here I add a basic "forward vector" which acts like a kind of pole-vetor
// to control the bending direction
void ik_two_bone(
    quat& bone_root_lr, 
    quat& bone_mid_lr,
    const vec3 bone_root, 
    const vec3 bone_mid, 
    const vec3 bone_end, 
    const vec3 target, 
    const vec3 fwd,
    const quat bone_root_gr, 
    const quat bone_mid_gr,
    const quat bone_par_gr,
    const float max_length_buffer) {
    
    float max_extension = 
        length(bone_root - bone_mid) + 
        length(bone_mid - bone_end) - 
        max_length_buffer;
    
    vec3 target_clamp = target;
    if (length(target - bone_root) > max_extension)
    {
        target_clamp = bone_root + max_extension * normalize(target - bone_root);
    }
    
    vec3 axis_dwn = normalize(bone_end - bone_root);
    vec3 axis_rot = normalize(cross(axis_dwn, fwd));

    vec3 a = bone_root;
    vec3 b = bone_mid;
    vec3 c = bone_end;
    vec3 t = target_clamp;
    
    float lab = length(b - a);
    float lcb = length(b - c);
    float lat = length(t - a);

    float ac_ab_0 = acosf(clampf(dot(normalize(c - a), normalize(b - a)), -1.0f, 1.0f));
    float ba_bc_0 = acosf(clampf(dot(normalize(a - b), normalize(c - b)), -1.0f, 1.0f));

    float ac_ab_1 = acosf(clampf((lab * lab + lat * lat - lcb * lcb) / (2.0f * lab * lat), -1.0f, 1.0f));
    float ba_bc_1 = acosf(clampf((lab * lab + lcb * lcb - lat * lat) / (2.0f * lab * lcb), -1.0f, 1.0f));

    quat r0 = quat_from_angle_axis(ac_ab_1 - ac_ab_0, axis_rot);
    quat r1 = quat_from_angle_axis(ba_bc_1 - ba_bc_0, axis_rot);

    vec3 c_a = normalize(bone_end - bone_root);
    vec3 t_a = normalize(target_clamp - bone_root);

    quat r2 = quat_from_angle_axis(
        acosf(clampf(dot(c_a, t_a), -1.0f, 1.0f)),
        normalize(cross(c_a, t_a)));
    
    bone_root_lr = quat_inv_mul(bone_par_gr, quat_mul(r2, quat_mul(r0, bone_root_gr)));
    bone_mid_lr = quat_inv_mul(bone_root_gr, quat_mul(r1, bone_mid_gr));
}

//--------------------------------------

void draw_axis(const vec3 pos, const quat rot, const float scale = 1.0f)
{
    vec3 axis0 = pos + quat_mul_vec3(rot, scale * vec3(1.0f, 0.0f, 0.0f));
    vec3 axis1 = pos + quat_mul_vec3(rot, scale * vec3(0.0f, 1.0f, 0.0f));
    vec3 axis2 = pos + quat_mul_vec3(rot, scale * vec3(0.0f, 0.0f, 1.0f));
    
    DrawLine3D(to_Vector3(pos), to_Vector3(axis0), RED);
    DrawLine3D(to_Vector3(pos), to_Vector3(axis1), GREEN);
    DrawLine3D(to_Vector3(pos), to_Vector3(axis2), BLUE);
}

void draw_features(
    const slice1d<float> features, 
    const vec3 pos, 
    const quat rot, 
    const Color color,
    const slice1d<vec2> future_toe_position,
    const slice1d<vec2> future_terrain_heights,
    const vec3 hip_global_position,
    const slice1d<vec3> global_bone_positions,
    const slice1d<int> contact_bones,
    const std::vector<vec3>& root_history_positions,
    const std::vector<quat>& root_history_rotations,
    const std::vector<vec3>& history_left_foot_positions,
    const std::vector<vec3>& history_right_foot_positions,
    const std::vector<vec3>& history_left_foot_velocities,
    const std::vector<vec3>& history_right_foot_velocities,
    const std::vector<vec3>& history_hip_positions,
    const std::vector<vec3>& history_hip_velocities,
    const std::vector<vec2>& history_terrain_heights)
{
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
    
    DrawSphereWires(to_Vector3(lfoot_pos), 0.05f, 4, 10, color);
    DrawSphereWires(to_Vector3(rfoot_pos), 0.05f, 4, 10, color);
    DrawSphereWires(to_Vector3(traj0_pos), 0.05f, 4, 10, color);
    DrawSphereWires(to_Vector3(traj1_pos), 0.05f, 4, 10, color);
    DrawSphereWires(to_Vector3(traj2_pos), 0.05f, 4, 10, color);
    
    DrawLine3D(to_Vector3(lfoot_pos), to_Vector3(lfoot_pos + 0.1f * lfoot_vel), color);
    DrawLine3D(to_Vector3(rfoot_pos), to_Vector3(rfoot_pos + 0.1f * rfoot_vel), color);
    
    DrawLine3D(to_Vector3(traj0_pos), to_Vector3(traj0_pos + 0.25f * traj0_dir), color);
    DrawLine3D(to_Vector3(traj1_pos), to_Vector3(traj1_pos + 0.25f * traj1_dir), color);
    DrawLine3D(to_Vector3(traj2_pos), to_Vector3(traj2_pos + 0.25f * traj2_dir), color);
    
    // Draw terrain height features (8 values: 4 time samples x 2 toes)
    // Features 33-36: left toe terrain heights at t0, t1, t2, t3
    // Features 37-40: right toe terrain heights at t0, t1, t2, t3
    Color terrain_colors[4] = {
        ColorAlpha(color, 1.0f),    // Current frame - full opacity
        ColorAlpha(color, 0.8f),   // +15 frames
        ColorAlpha(color, 0.6f),    // +30 frames
        ColorAlpha(color, 0.4f)    // +45 frames
    };
    
    for (int i = 0; i < 4; i++)
    {
        // Use actual terrain heights computed from raycasting
        float left_terrain_height = future_terrain_heights(i).x;
        float right_terrain_height = future_terrain_heights(i).y;
        
        vec3 left_toe_xz;
        vec3 right_toe_xz;
        
        if (i == 0)
        {
            // Current frame: use actual global toe positions
            left_toe_xz = vec3(global_bone_positions(contact_bones(0)).x, 0, global_bone_positions(contact_bones(0)).z);
            right_toe_xz = vec3(global_bone_positions(contact_bones(1)).x, 0, global_bone_positions(contact_bones(1)).z);
        }
        else
        {
            // Future frames: transform future_toe_position from local to world space
            // future_toe_position stores positions in character-local space
            int future_idx = i - 1;
            
            // Convert 2D local positions to 3D
            left_toe_xz = vec3(future_toe_position(future_idx * 2 + 0).x, 0, future_toe_position(future_idx * 2 + 0).y);
            right_toe_xz = vec3(future_toe_position(future_idx * 2 + 1).x, 0, future_toe_position(future_idx * 2 + 1).y);

            // Relative to root
            left_toe_xz = quat_mul_vec3(rot, left_toe_xz) + pos;
            right_toe_xz = quat_mul_vec3(rot, right_toe_xz) + pos;
            
        }

        
        
        // Position spheres at toe XZ position with Y = terrain_height + hip_height
        vec3 left_terrain_pos = vec3(left_toe_xz.x, hip_global_position.y + left_terrain_height + 0.01f, left_toe_xz.z);
        vec3 right_terrain_pos = vec3(right_toe_xz.x, hip_global_position.y + right_terrain_height + 0.01f, right_toe_xz.z);
        
        // Draw small spheres at terrain height positions
        DrawSphereWires(to_Vector3(left_terrain_pos), 0.03f, 4, 6, terrain_colors[i]);
        DrawSphereWires(to_Vector3(right_terrain_pos), 0.03f, 4, 6, terrain_colors[i]);
        
        // Draw vertical lines from toe XZ position at hip height down to terrain
        vec3 left_hip_level = vec3(left_toe_xz.x, hip_global_position.y, left_toe_xz.z);
        vec3 right_hip_level = vec3(right_toe_xz.x, hip_global_position.y, right_toe_xz.z);
        
        DrawLine3D(to_Vector3(left_hip_level), to_Vector3(left_terrain_pos), terrain_colors[i]);
        DrawLine3D(to_Vector3(right_hip_level), to_Vector3(right_terrain_pos), terrain_colors[i]);
    }

    auto sample_runtime_history_idx = [&](int relative_offset)
    {
        if (root_history_positions.empty()) return 0;
        int last = (int)root_history_positions.size() - 1;
        return clamp(last + relative_offset, 0, last);
    };

    auto draw_history_traj_sphere = [&](int history_offset, Color c)
    {
        if (root_history_positions.empty() || root_history_rotations.empty()) return;

        int anchor_idx = sample_runtime_history_idx(history_offset);
        int traj_idx = sample_runtime_history_idx(history_offset + 20);

        vec3 root_pos_history = root_history_positions[anchor_idx];
        quat root_rot_history = root_history_rotations[anchor_idx];
        vec3 htraj_pos = root_history_positions[traj_idx];

        Color c_faint = ColorAlpha(c, 0.55f);
        DrawSphereWires(to_Vector3(root_pos_history), 0.03f, 4, 6, c_faint);
        DrawLine3D(to_Vector3(root_pos_history), to_Vector3(htraj_pos), c_faint);
        DrawSphereWires(to_Vector3(htraj_pos), 0.07f, 6, 12, c);

        vec3 history_traj_local = quat_inv_mul_vec3(root_rot_history, htraj_pos - root_pos_history);
        vec3 history_traj_dir = quat_inv_mul_vec3(
            root_rot_history,
            quat_mul_vec3(root_history_rotations[traj_idx], vec3(0.0f, 0.0f, 1.0f)));

        const float eps = 1e-4f;
        float h = length(vec3(history_traj_local.x, 0.0f, history_traj_local.z));
        history_traj_dir.y = history_traj_local.y / maxf(h, eps);
        history_traj_dir = normalize(history_traj_dir);

        vec3 history_traj_dir_world = quat_mul_vec3(root_rot_history, history_traj_dir);
        DrawLine3D(to_Vector3(htraj_pos), to_Vector3(htraj_pos + 0.25f * history_traj_dir_world), c_faint);
    };

    auto draw_history_bone_position_sphere = [&](const std::vector<vec3>& history_positions, int history_offset, Color c, float radius = 0.05f)
    {
        if (root_history_positions.empty() || root_history_rotations.empty() || history_positions.empty()) return;

        int idx = sample_runtime_history_idx(history_offset);
        vec3 root_pos_history = root_history_positions[idx];
        quat root_rot_history = root_history_rotations[idx];

        vec3 local_feature = history_positions[idx];
        vec3 feature_world = quat_mul_vec3(root_rot_history, local_feature) + root_pos_history;

        Color c_faint = ColorAlpha(c, 0.45f);
        DrawLine3D(to_Vector3(root_pos_history), to_Vector3(feature_world), c_faint);
        DrawSphereWires(to_Vector3(feature_world), radius, 4, 10, c);
    };

    auto draw_history_velocity_sphere = [&](const std::vector<vec3>& history_positions, const std::vector<vec3>& history_velocities, int history_offset, Color c)
    {
        if (root_history_positions.empty() || root_history_rotations.empty() || history_velocities.empty()) return;

        int idx = sample_runtime_history_idx(history_offset);
        vec3 root_pos_history = root_history_positions[idx];
        quat root_rot_history = root_history_rotations[idx];

        vec3 feature_pos_local = vec3();
        if (!history_positions.empty())
        {
            feature_pos_local = history_positions[idx];
        }

        vec3 feature_vel_local = history_velocities[idx];

        vec3 feature_pos_world = quat_mul_vec3(root_rot_history, feature_pos_local) + root_pos_history;
        vec3 feature_vel_world = quat_mul_vec3(root_rot_history, feature_vel_local);
        vec3 feature_vel_end = feature_pos_world + 0.1f * feature_vel_world;

        Color c_faint = ColorAlpha(c, 0.45f);
        DrawLine3D(to_Vector3(feature_pos_world), to_Vector3(feature_vel_end), c_faint);
        DrawSphereWires(to_Vector3(feature_vel_end), 0.045f, 4, 10, c);
    };

    auto draw_history_terrain_spheres = [&](int history_offset, Color c)
    {
        if (root_history_positions.empty() || root_history_rotations.empty()) return;
        if (history_terrain_heights.empty() || history_left_foot_positions.empty() || history_right_foot_positions.empty()) return;

        int idx = sample_runtime_history_idx(history_offset);
        vec3 root_pos_history = root_history_positions[idx];
        quat root_rot_history = root_history_rotations[idx];

        vec3 left_toe_world = quat_mul_vec3(root_rot_history, history_left_foot_positions[idx]) + root_pos_history;
        vec3 right_toe_world = quat_mul_vec3(root_rot_history, history_right_foot_positions[idx]) + root_pos_history;

        float left_terrain_height = history_terrain_heights[idx].x;
        float right_terrain_height = history_terrain_heights[idx].y;

        float hip_y = root_pos_history.y;
        if (!history_hip_positions.empty())
        {
            hip_y = history_hip_positions[idx].y;
        }

        vec3 left_terrain_pos = vec3(left_toe_world.x, hip_y + left_terrain_height + 0.01f, left_toe_world.z);
        vec3 right_terrain_pos = vec3(right_toe_world.x, hip_y + right_terrain_height + 0.01f, right_toe_world.z);

        Color c_faint = ColorAlpha(c, 0.45f);
        DrawLine3D(to_Vector3(vec3(left_toe_world.x, hip_y, left_toe_world.z)), to_Vector3(left_terrain_pos), c_faint);
        DrawLine3D(to_Vector3(vec3(right_toe_world.x, hip_y, right_toe_world.z)), to_Vector3(right_terrain_pos), c_faint);
        DrawSphereWires(to_Vector3(left_terrain_pos), 0.03f, 4, 6, c);
        DrawSphereWires(to_Vector3(right_terrain_pos), 0.03f, 4, 6, c);
    };

    draw_history_bone_position_sphere(history_left_foot_positions, -20, SKYBLUE);
    draw_history_bone_position_sphere(history_right_foot_positions, -20, SKYBLUE);
    draw_history_velocity_sphere(history_left_foot_positions, history_left_foot_velocities, -20, VIOLET);
    draw_history_velocity_sphere(history_right_foot_positions, history_right_foot_velocities, -20, MAGENTA);
    draw_history_velocity_sphere(history_hip_positions, history_hip_velocities, -20, LIME);
    draw_history_traj_sphere(-20, YELLOW);
    draw_history_terrain_spheres(-15, GREEN);
}

void draw_trajectory(
    const slice1d<vec3> trajectory_positions, 
    const slice1d<quat> trajectory_rotations, 
    const Color color)
{
    for (int i = 1; i < trajectory_positions.size; i++)
    {
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

//--------------------------------------

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

//--------------------------------------

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

//--------------------------------------

void update_callback(void* args)
{
    ((std::function<void()>*)args)->operator()();
}

int main(int argc, char** argv)
{
    try
    {
        bool start_with_lmm_enabled = false;
        enum app_mode
        {
            APP_MODE_WINDOW,
            APP_MODE_ANALYZE_BOTH,
            APP_MODE_ANALYZE_MM,
            APP_MODE_ANALYZE_LMM
        };
        app_mode mode = APP_MODE_WINDOW;
        char analyze_input_path[512] = "./resources/input-recording";
        bool analyze_input_is_file = false;
        bool force_rebuild_features = false;
        for (int argi = 1; argi < argc; argi++)
        {
            if (strcmp(argv[argi], "--learned") == 0)
            {
                start_with_lmm_enabled = true;
            }
            else if (strcmp(argv[argi], "--rebuild-features") == 0)
            {
                force_rebuild_features = true;
            }
            else if (strcmp(argv[argi], "--window") == 0)
            {
                mode = APP_MODE_WINDOW;
            }
            else if (strcmp(argv[argi], "--analyze") == 0 || strcmp(argv[argi], "--analyze-both") == 0)
            {
                mode = APP_MODE_ANALYZE_BOTH;
            }
            else if (strcmp(argv[argi], "--analyze-mm") == 0)
            {
                mode = APP_MODE_ANALYZE_MM;
            }
            else if (strcmp(argv[argi], "--analyze-lmm") == 0)
            {
                mode = APP_MODE_ANALYZE_LMM;
            }
            else if (strncmp(argv[argi], "--mode=", 7) == 0)
            {
                const char* mode_name = argv[argi] + 7;
                if (strcmp(mode_name, "window") == 0)
                {
                    mode = APP_MODE_WINDOW;
                }
                else if (strcmp(mode_name, "analyze-both") == 0)
                {
                    mode = APP_MODE_ANALYZE_BOTH;
                }
                else if (strcmp(mode_name, "analyze-mm") == 0)
                {
                    mode = APP_MODE_ANALYZE_MM;
                }
                else if (strcmp(mode_name, "analyze-lmm") == 0)
                {
                    mode = APP_MODE_ANALYZE_LMM;
                }
                else
                {
                    printf("Warning: Unknown mode '%s'\n", mode_name);
                }
            }
            else if (strncmp(argv[argi], "--input=", 8) == 0)
            {
                snprintf(analyze_input_path, sizeof(analyze_input_path), "%s", argv[argi] + 8);
                analyze_input_is_file = true;
            }
            else if (strncmp(argv[argi], "--csv=", 6) == 0)
            {
                snprintf(analyze_input_path, sizeof(analyze_input_path), "%s", argv[argi] + 6);
                analyze_input_is_file = true;
            }
            else if (argv[argi][0] != '-')
            {
                snprintf(analyze_input_path, sizeof(analyze_input_path), "%s", argv[argi]);
                analyze_input_is_file = true;
            }
            else if (strcmp(argv[argi], "-h") == 0 || strcmp(argv[argi], "--help") == 0)
            {
                printf("Usage: %s [--learned] [--rebuild-features] [--window | --analyze-both | --analyze-mm | --analyze-lmm] [--input=<csv>]\n", argv[0]);
                printf("       %s --mode=<window|analyze-both|analyze-mm|analyze-lmm> --input=<csv>\n", argv[0]);
                return 0;
            }
            else
            {
                printf("Warning: Unknown argument '%s'\n", argv[argi]);
            }
        }

        // Init Window
        
        const int screen_width = 1720;
        const int screen_height = 920;
        
        unsigned int window_flags = FLAG_VSYNC_HINT | FLAG_MSAA_4X_HINT;
        if (mode != APP_MODE_WINDOW)
        {
            window_flags |= FLAG_WINDOW_HIDDEN;
        }
        SetConfigFlags(window_flags);
        InitWindow(screen_width, screen_height, "raylib [data vs code driven displacement]");
        SetTargetFPS(60);
    
    // Camera

    Camera3D camera = { 0 };
    camera.position = (Vector3){ 0.0f, 10.0f, 10.0f };
    camera.target = (Vector3){ 0.0f, 0.0f, 0.0f };
    camera.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    float camera_azimuth = 0.0f;
    float camera_altitude = 0.4f;
    float camera_distance = 4.0f;
    
    // Ground Plane
    
    // Try to load .glb model first, fallback to procedural plane
    const char* ground_glb_path = "resources/glb/10-Playground6.glb";
    Model ground_plane_model;
    Shader ground_plane_shader = { 0 };
    bool using_glb_ground = false;
    
    if (FileExists(ground_glb_path))
    {
        ground_plane_model = LoadModel(ground_glb_path);
        using_glb_ground = true;

        // Generate a procedural checkerboard image
        Image img = GenImageChecked(256, 256, 32, 32, LIGHTGRAY, WHITE);
        Texture2D texture = LoadTextureFromImage(img);
        UnloadImage(img); // Unload CPU image

        // Assign to model material
        for (int i = 0; i < ground_plane_model.materialCount; i++){
            ground_plane_model.materials[i].maps[MATERIAL_MAP_DIFFUSE].texture = texture;
        }
    }
    else
    {
        // Fallback to procedural ground plane with checkerboard shader
        ground_plane_shader = LoadShader("./resources/shaders/checkerboard.vs", "./resources/shaders/checkerboard.fs");
        Mesh ground_plane_mesh = GenMeshPlane(20.0f, 20.0f, 10, 10);
        ground_plane_model = LoadModelFromMesh(ground_plane_mesh);
        ground_plane_model.materials[0].shader = ground_plane_shader;
    }
    
    // Character
    
    character character_data;
    character_load(character_data, "./resources/bin/character.bin");
    
    Shader character_shader = LoadShader("./resources/shaders/character.vs", "./resources/shaders/character.fs");
    Mesh character_mesh = make_character_mesh(character_data);
    Model character_model = LoadModelFromMesh(character_mesh);
    character_model.materials[0].shader = character_shader;
    
    // Load Animation Data and build Matching Database
    
    std::cout << "Loading database..." << std::endl;
    
    database db;
    database_load(db, "./resources/bin/database.bin");

    std::vector<range_metadata_entry> range_metadata_entries;
    if (!load_range_metadata_csv("./resources/bin/range_metadata.csv", range_metadata_entries))
    {
        if (debug) std::cout << "range_metadata.csv missing or empty; BVH labels disabled" << std::endl;
    }
    
    const char* database_path = "./resources/bin/database.bin";
    const char* features_path = "./resources/bin/features.bin";

    bool rebuild_features = force_rebuild_features || should_rebuild_features(database_path, features_path);
    const int expected_feature_count = matching_feature_count_expected();
    if (rebuild_features)
    {
        if (force_rebuild_features)
        {
            std::cout << "--rebuild-features requested. Rebuilding matching features..." << std::endl;
        }
        else
        {
            std::cout << "Database is new or features.bin is missing. Building matching features..." << std::endl;
        }
    }
    else
    {
        std::cout << "features.bin is up to date. Skipping feature rebuild." << std::endl;
    }
    
    float feature_weight_foot_position = 0.75f;
    float feature_weight_foot_velocity = 1.0f;
    float feature_weight_hip_velocity = 1.0f;
    float feature_weight_trajectory_positions = 1.0f;
    float feature_weight_trajectory_directions = 1.5f;
    float feature_weight_terrain_heights = 0.5f;
    float feature_weight_idle = 2.0f;
    float feature_weight_crouch = 2.0f;
    float feature_weight_jump = 2.0f;
    float feature_weight_cartwheel = 2.0f;

    float feature_weight_prev_frame_multiplier = 1.0f;

    float feature_weight_history_foot_position = feature_weight_foot_position * feature_weight_prev_frame_multiplier;
    float feature_weight_history_foot_velocity = feature_weight_foot_velocity * feature_weight_prev_frame_multiplier;
    float feature_weight_history_hip_velocity = feature_weight_hip_velocity * feature_weight_prev_frame_multiplier;
    float feature_weight_history_trajectory_positions = feature_weight_trajectory_positions * feature_weight_prev_frame_multiplier;
    float feature_weight_history_trajectory_directions = feature_weight_trajectory_directions * feature_weight_prev_frame_multiplier;
    float feature_weight_history_terrain_heights = feature_weight_terrain_heights * feature_weight_prev_frame_multiplier;
    
    enum mm_history_search_mode
    {
        MM_HISTORY_SEARCH_OFF = 0,
        MM_HISTORY_SEARCH_ON = 1,
        MM_HISTORY_SEARCH_BOTH = 2,
    };
    int mm_history_mode = MM_HISTORY_SEARCH_OFF;
    bool mm_history_mode_dropdown_edit = false;
    
    
    if (rebuild_features)
    {
        database_build_matching_features(
            db,
            feature_weight_foot_position,
            feature_weight_foot_velocity,
            feature_weight_hip_velocity,
            feature_weight_trajectory_positions,
            feature_weight_trajectory_directions,
            feature_weight_terrain_heights,
            feature_weight_idle,
            feature_weight_crouch,
            feature_weight_jump,
            feature_weight_cartwheel,
            feature_weight_history_foot_position,
            feature_weight_history_foot_velocity,
            feature_weight_history_hip_velocity,
            feature_weight_history_trajectory_positions,
            feature_weight_history_trajectory_directions,
            feature_weight_history_terrain_heights);
        
        database_save_matching_features(db, features_path, false);
        std::cout << "Features saved. Initializing pose data..." << std::endl;
    }
    else
    {
        database_load_matching_features(db, features_path);

        if (db.nfeatures() != expected_feature_count)
        {
            std::cout
                << "features.bin feature layout mismatch (got " << db.nfeatures()
                << ", expected " << expected_feature_count
                << "). Rebuilding features..." << std::endl;

            database_build_matching_features(
                db,
                feature_weight_foot_position,
                feature_weight_foot_velocity,
                feature_weight_hip_velocity,
                feature_weight_trajectory_positions,
                feature_weight_trajectory_directions,
                feature_weight_terrain_heights,
                feature_weight_idle,
                feature_weight_crouch,
                feature_weight_jump,
                feature_weight_cartwheel,
                feature_weight_history_foot_position,
                feature_weight_history_foot_velocity,
                feature_weight_history_hip_velocity,
                feature_weight_history_trajectory_positions,
                feature_weight_history_trajectory_directions,
                feature_weight_history_terrain_heights);

            database_save_matching_features(db, features_path, false);
            std::cout << "Features rebuilt. Initializing pose data..." << std::endl;
        }
        else
        {
            database_build_bounds(db);
            std::cout << "Using existing features.bin. Initializing pose data..." << std::endl;
        }
    }
    
    int frame_index = db.range_starts(0);
    int mm_last_best_with_history = frame_index;
    int mm_last_best_without_history = frame_index;
    float inertialize_blending_halflife = 0.1f;

    array1d<vec3> curr_bone_positions = db.bone_positions(frame_index);
    array1d<vec3> curr_bone_velocities = db.bone_velocities(frame_index);
    array1d<quat> curr_bone_rotations = db.bone_rotations(frame_index);
    array1d<vec3> curr_bone_angular_velocities = db.bone_angular_velocities(frame_index);
    array1d<bool> curr_bone_contacts = db.contact_states(frame_index);

    array1d<vec3> trns_bone_positions = db.bone_positions(frame_index);
    array1d<vec3> trns_bone_velocities = db.bone_velocities(frame_index);
    array1d<quat> trns_bone_rotations = db.bone_rotations(frame_index);
    array1d<vec3> trns_bone_angular_velocities = db.bone_angular_velocities(frame_index);
    array1d<bool> trns_bone_contacts = db.contact_states(frame_index);

    array1d<vec3> bone_positions = db.bone_positions(frame_index);
    array1d<vec3> bone_velocities = db.bone_velocities(frame_index);
    array1d<quat> bone_rotations = db.bone_rotations(frame_index);
    array1d<vec3> bone_angular_velocities = db.bone_angular_velocities(frame_index);
    
    array1d<vec3> bone_offset_positions(db.nbones());
    array1d<vec3> bone_offset_velocities(db.nbones());
    array1d<quat> bone_offset_rotations(db.nbones());
    array1d<vec3> bone_offset_angular_velocities(db.nbones());

    array1d<vec3> curr_bone_positions_with_history = db.bone_positions(frame_index);
    array1d<vec3> curr_bone_velocities_with_history = db.bone_velocities(frame_index);
    array1d<quat> curr_bone_rotations_with_history = db.bone_rotations(frame_index);
    array1d<vec3> curr_bone_angular_velocities_with_history = db.bone_angular_velocities(frame_index);

    array1d<vec3> bone_positions_with_history = db.bone_positions(frame_index);
    array1d<vec3> bone_velocities_with_history = db.bone_velocities(frame_index);
    array1d<quat> bone_rotations_with_history = db.bone_rotations(frame_index);
    array1d<vec3> bone_angular_velocities_with_history = db.bone_angular_velocities(frame_index);
    
    array1d<vec3> bone_offset_positions_with_history(db.nbones());
    array1d<vec3> bone_offset_velocities_with_history(db.nbones());
    array1d<quat> bone_offset_rotations_with_history(db.nbones());
    array1d<vec3> bone_offset_angular_velocities_with_history(db.nbones());

    vec3 transition_src_position_with_history;
    quat transition_src_rotation_with_history;
    vec3 transition_dst_position_with_history;
    quat transition_dst_rotation_with_history;

    array1d<vec3> curr_bone_positions_without_history = db.bone_positions(frame_index);
    array1d<vec3> curr_bone_velocities_without_history = db.bone_velocities(frame_index);
    array1d<quat> curr_bone_rotations_without_history = db.bone_rotations(frame_index);
    array1d<vec3> curr_bone_angular_velocities_without_history = db.bone_angular_velocities(frame_index);

    array1d<vec3> bone_positions_without_history = db.bone_positions(frame_index);
    array1d<vec3> bone_velocities_without_history = db.bone_velocities(frame_index);
    array1d<quat> bone_rotations_without_history = db.bone_rotations(frame_index);
    array1d<vec3> bone_angular_velocities_without_history = db.bone_angular_velocities(frame_index);
    
    array1d<vec3> bone_offset_positions_without_history(db.nbones());
    array1d<vec3> bone_offset_velocities_without_history(db.nbones());
    array1d<quat> bone_offset_rotations_without_history(db.nbones());
    array1d<vec3> bone_offset_angular_velocities_without_history(db.nbones());

    vec3 transition_src_position_without_history;
    quat transition_src_rotation_without_history;
    vec3 transition_dst_position_without_history;
    quat transition_dst_rotation_without_history;
    
    array1d<vec3> global_bone_positions(db.nbones());
    array1d<vec3> global_bone_velocities(db.nbones());
    array1d<quat> global_bone_rotations(db.nbones());
    array1d<vec3> global_bone_angular_velocities(db.nbones());
    array1d<bool> global_bone_computed(db.nbones());
    
    vec3 transition_src_position;
    quat transition_src_rotation;
    vec3 transition_dst_position;
    quat transition_dst_rotation;
    
    inertialize_pose_reset(
        bone_offset_positions,
        bone_offset_velocities,
        bone_offset_rotations,
        bone_offset_angular_velocities,
        transition_src_position,
        transition_src_rotation,
        transition_dst_position,
        transition_dst_rotation,
        bone_positions(0),
        bone_rotations(0));
    
    inertialize_pose_update(
        bone_positions,
        bone_velocities,
        bone_rotations,
        bone_angular_velocities,
        bone_offset_positions,
        bone_offset_velocities,
        bone_offset_rotations,
        bone_offset_angular_velocities,
        db.bone_positions(frame_index),
        db.bone_velocities(frame_index),
        db.bone_rotations(frame_index),
        db.bone_angular_velocities(frame_index),
        transition_src_position,
        transition_src_rotation,
        transition_dst_position,
        transition_dst_rotation,
        inertialize_blending_halflife,
        0.0f);

    inertialize_pose_update(
        bone_positions_with_history, bone_velocities_with_history,
        bone_rotations_with_history, bone_angular_velocities_with_history,
        bone_offset_positions_with_history, bone_offset_velocities_with_history,
        bone_offset_rotations_with_history, bone_offset_angular_velocities_with_history,
        db.bone_positions(frame_index), db.bone_velocities(frame_index),
        db.bone_rotations(frame_index), db.bone_angular_velocities(frame_index),
        transition_src_position_with_history, transition_src_rotation_with_history,
        transition_dst_position_with_history, transition_dst_rotation_with_history,
        inertialize_blending_halflife, 0.0f);

    inertialize_pose_update(
        bone_positions_without_history, bone_velocities_without_history,
        bone_rotations_without_history, bone_angular_velocities_without_history,
        bone_offset_positions_without_history, bone_offset_velocities_without_history,
        bone_offset_rotations_without_history, bone_offset_angular_velocities_without_history,
        db.bone_positions(frame_index), db.bone_velocities(frame_index),
        db.bone_rotations(frame_index), db.bone_angular_velocities(frame_index),
        transition_src_position_without_history, transition_src_rotation_without_history,
        transition_dst_position_without_history, transition_dst_rotation_without_history,
        inertialize_blending_halflife, 0.0f);
        
    // Trajectory & Gameplay Data
    
    float search_time = 0.1f;
    float search_timer = search_time;
    float force_search_timer = search_time;
    
    vec3 desired_velocity;
    vec3 desired_velocity_change_curr;
    vec3 desired_velocity_change_prev;
    float desired_velocity_change_threshold = 50.0;
    
    quat desired_rotation;
    vec3 desired_rotation_change_curr;
    vec3 desired_rotation_change_prev;
    float desired_rotation_change_threshold = 50.0;
    
    float desired_gait = 0.0f;
    float desired_gait_velocity = 0.0f;
    bool desired_crouch_prev = false;
    bool desired_cartwheel_prev = false;
    bool desired_idle_prev = false;
    bool desired_jump_prev = false;
    float cartwheel_auto_timer = 0.0f;
    const float cartwheel_auto_duration = 1.0f;
    bool cartwheel_search_freeze_prev = false;
    bool cartwheel_query_lock_prev = false;
    vec3 cartwheel_query_lock_forward = vec3(0.0f, 0.0f, 1.0f);
    float cartwheel_query_lock_step_distance = 0.0f;
    float cartwheel_first_search_step_distance = 0.0f;
    
    vec3 simulation_position;
    vec3 simulation_velocity;
    vec3 simulation_acceleration;
    quat simulation_rotation;
    vec3 simulation_angular_velocity;
    
    float simulation_velocity_halflife = 0.27f;
    float simulation_rotation_halflife = 0.27f;
    float terrain_y_clamp_offset = 0.8f;
    
    // All speeds in m/s
    float simulation_run_fwrd_speed = 4.0f;
    float simulation_run_side_speed = 3.0f;
    float simulation_run_back_speed = 2.5f;

    // float simulation_run_fwrd_speed = 5.0f;
    // float simulation_run_side_speed = 4.0f;
    // float simulation_run_back_speed = 3.0f;
    
    float simulation_walk_fwrd_speed = 1.75f;
    float simulation_walk_side_speed = 1.5f;
    float simulation_walk_back_speed = 1.25f;
    
    // float simulation_walk_fwrd_speed = 3.0f;
    // float simulation_walk_side_speed = 2.0f;
    // float simulation_walk_back_speed = 1.5f;

    float simulation_crouch_fwrd_speed = 2.0f;
    float simulation_crouch_side_speed = 1.5f;
    float simulation_crouch_back_speed = 1.25f;
    float cartwheel_speed_boost = 1.2f;
    float jump_speed_boost = 1.5f;

    float climbing_min_speed_factor = 0.1f;
    float climbing_probe_distance = 0.6f;
    float climbing_height_threshold = 1.0f;
    float climbing_max_height_delta = 0.8f;
    
    float jump_root_height_offset = 1.2f;
    const float jump_initial_vertical_speed = 8.0f;
    const float jump_gravity = 20.0f;
    const float jump_ground_snap_epsilon = 0.08f;
    const float jump_ground_velocity_epsilon = 0.35f;
    const float jump_buffer_time = 0.12f;
    const float jump_coyote_time = 0.08f;
    bool jump_active = false;
    float jump_vertical_velocity = 0.0f;
    float jump_buffer_timer = 0.0f;
    float jump_coyote_timer = 0.0f;
    
    array1d<vec3> trajectory_desired_velocities(4);
    array1d<quat> trajectory_desired_rotations(4);
    array1d<vec3> trajectory_positions(4);
    array1d<vec3> trajectory_velocities(4);
    array1d<vec3> trajectory_accelerations(4);
    array1d<quat> trajectory_rotations(4);
    array1d<vec3> trajectory_angular_velocities(4);
    
    // Synchronization
    
    bool synchronization_enabled = false;
    float synchronization_data_factor = 1.0f;
    
    // Adjustment
    
    bool adjustment_enabled = true;
    bool adjustment_by_velocity_enabled = true;
    float adjustment_position_halflife = 0.1f;
    float adjustment_rotation_halflife = 0.2f;
    float adjustment_position_max_ratio = 0.5f;
    float adjustment_rotation_max_ratio = 0.5f;
    
    // Clamping
    
    bool clamping_enabled = true;
    float clamping_max_distance = 0.15f;
    float clamping_max_angle = 0.5f * PIf;
    
    // IK
    
    bool ik_enabled = true;
    float ik_max_length_buffer = 0.015f;
    float ik_foot_height = 0.02f;
    float ik_toe_length = 0.15f;
    float ik_unlock_radius = 0.2f;
    float ik_blending_halflife = 0.1f;
    
    // Contact and Foot Locking data
    
    array1d<int> contact_bones(2);
    contact_bones(0) = Bone_LeftToe;
    contact_bones(1) = Bone_RightToe;
    
    array1d<bool> contact_states(contact_bones.size);
    array1d<bool> contact_locks(contact_bones.size);
    array1d<vec3> contact_positions(contact_bones.size);
    array1d<vec3> contact_velocities(contact_bones.size);
    array1d<vec3> contact_points(contact_bones.size);
    array1d<vec3> contact_targets(contact_bones.size);
    array1d<vec3> contact_offset_positions(contact_bones.size);
    array1d<vec3> contact_offset_velocities(contact_bones.size);
    
    for (int i = 0; i < contact_bones.size; i++)
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
            bone_positions,
            bone_velocities,
            bone_rotations,
            bone_angular_velocities,
            db.bone_parents,
            contact_bones(i));
        
        contact_reset(
            contact_states(i),
            contact_locks(i),
            contact_positions(i),  
            contact_velocities(i),
            contact_points(i),
            contact_targets(i),
            contact_offset_positions(i),
            contact_offset_velocities(i),
            bone_position,
            bone_velocity,
            false);
    }
    
    array1d<vec3> adjusted_bone_positions = bone_positions;
    array1d<quat> adjusted_bone_rotations = bone_rotations;
    
    // Learned Motion Matching
    
    bool lmm_enabled = start_with_lmm_enabled;
    
    if (debug) std::cout << "Loading neural networks..." << std::endl;
    
    nnet decompressor, stepper, projector;    
    if (debug) std::cout << "Loading decompressor..." << std::endl;
    nnet_load(decompressor, "./resources/bin/decompressor.bin");
    if (debug) std::cout << "Loading stepper..." << std::endl;
    nnet_load(stepper, "./resources/bin/stepper.bin");
    if (debug) std::cout << "Loading projector..." << std::endl;
    nnet_load(projector, "./resources/bin/projector.bin");

    const int lmm_latent_size = 32;
    const int expected_features = db.nfeatures();
    const int expected_features_plus_latent = db.nfeatures() + lmm_latent_size;

    const bool decompressor_input_match =
        decompressor.input_mean.size == expected_features_plus_latent;
    const bool stepper_input_match =
        stepper.input_mean.size == expected_features_plus_latent;
    const bool stepper_output_match =
        stepper.output_mean.size == expected_features_plus_latent;
    const bool projector_input_match =
        projector.input_mean.size == expected_features;
    const bool projector_output_match =
        projector.output_mean.size == expected_features_plus_latent;

    bool lmm_networks_compatible =
        decompressor_input_match &&
        stepper_input_match &&
        stepper_output_match &&
        projector_input_match &&
        projector_output_match;

    if (!lmm_networks_compatible)
    {
        printf("Warning: LMM network dimensions do not match feature count (db.nfeatures=%d). Retrain decompressor/projector/stepper for this feature layout.\n", db.nfeatures());
        printf("  [%-8s] decompressor.input_mean.size : actual=%d expected=%d\n",
            decompressor_input_match ? "MATCH" : "MISMATCH",
            decompressor.input_mean.size,
            expected_features_plus_latent);
        printf("  [%-8s] stepper.input_mean.size      : actual=%d expected=%d\n",
            stepper_input_match ? "MATCH" : "MISMATCH",
            stepper.input_mean.size,
            expected_features_plus_latent);
        printf("  [%-8s] stepper.output_mean.size     : actual=%d expected=%d\n",
            stepper_output_match ? "MATCH" : "MISMATCH",
            stepper.output_mean.size,
            expected_features_plus_latent);
        printf("  [%-8s] projector.input_mean.size    : actual=%d expected=%d\n",
            projector_input_match ? "MATCH" : "MISMATCH",
            projector.input_mean.size,
            expected_features);
        printf("  [%-8s] projector.output_mean.size   : actual=%d expected=%d\n",
            projector_output_match ? "MATCH" : "MISMATCH",
            projector.output_mean.size,
            expected_features_plus_latent);
    }
    
    if (debug) std::cout << "Setting up evaluations..." << std::endl;

    nnet_evaluation decompressor_evaluation, stepper_evaluation, projector_evaluation;
    decompressor_evaluation.resize(decompressor);
    stepper_evaluation.resize(stepper);
    projector_evaluation.resize(projector);
    
    if (debug) std::cout << "Initializing features..." << std::endl;
    array1d<float> features_proj = db.features(frame_index);
    array1d<float> features_curr = db.features(frame_index);
    array1d<float> latent_proj(lmm_latent_size); latent_proj.zero();
    array1d<float> latent_curr(lmm_latent_size); latent_curr.zero();
    
    // Future toe positions at 3 future time samples (15, 30, 45 frames)
    // Contains 3 frames x 2 toes = 6 entries: [left0, right0, left1, right1, left2, right2]
    array1d<vec2> future_toe_position(6);
    for (int i = 0; i < 6; i++) { future_toe_position(i) = vec2(0.0f, 0.0f); }
    
    // Future terrain heights at 4 time samples (0, 15, 30, 45 frames)  
    // Each vec2: x=left toe height, y=right toe height (all relative to hips)
    array1d<vec2> future_terrain_heights(4);
    for (int i = 0; i < 4; i++) { future_terrain_heights(i) = vec2(0.0f, 0.0f); }

    std::vector<vec3> root_history_positions;
    std::vector<quat> root_history_rotations;
    std::vector<vec3> history_left_foot_positions;
    std::vector<vec3> history_right_foot_positions;
    std::vector<vec3> history_left_foot_velocities;
    std::vector<vec3> history_right_foot_velocities;
    std::vector<vec3> history_hip_positions;
    std::vector<vec3> history_hip_velocities;
    std::vector<vec2> history_terrain_heights;
    const int root_history_max_frames = 32;
    auto trim_runtime_history = [&]()
    {
        if ((int)root_history_positions.size() > root_history_max_frames)
        {
            root_history_positions.erase(root_history_positions.begin());
            root_history_rotations.erase(root_history_rotations.begin());
            history_left_foot_positions.erase(history_left_foot_positions.begin());
            history_right_foot_positions.erase(history_right_foot_positions.begin());
            history_left_foot_velocities.erase(history_left_foot_velocities.begin());
            history_right_foot_velocities.erase(history_right_foot_velocities.begin());
            history_hip_positions.erase(history_hip_positions.begin());
            history_hip_velocities.erase(history_hip_velocities.begin());
            history_terrain_heights.erase(history_terrain_heights.begin());
        }
    };
    auto push_runtime_history = [&]()
    {
        vec3 left_foot_pos;
        quat left_foot_rot;
        forward_kinematics(
            left_foot_pos,
            left_foot_rot,
            bone_positions,
            bone_rotations,
            db.bone_parents,
            Bone_LeftFoot);

        vec3 right_foot_pos;
        quat right_foot_rot;
        forward_kinematics(
            right_foot_pos,
            right_foot_rot,
            bone_positions,
            bone_rotations,
            db.bone_parents,
            Bone_RightFoot);

        vec3 left_foot_vel;
        vec3 left_foot_ang_vel;
        forward_kinematics_velocity(
            left_foot_pos,
            left_foot_vel,
            left_foot_rot,
            left_foot_ang_vel,
            bone_positions,
            bone_velocities,
            bone_rotations,
            bone_angular_velocities,
            db.bone_parents,
            Bone_LeftFoot);

        vec3 right_foot_vel;
        vec3 right_foot_ang_vel;
        forward_kinematics_velocity(
            right_foot_pos,
            right_foot_vel,
            right_foot_rot,
            right_foot_ang_vel,
            bone_positions,
            bone_velocities,
            bone_rotations,
            bone_angular_velocities,
            db.bone_parents,
            Bone_RightFoot);

        vec3 hip_pos;
        quat hip_rot;
        vec3 hip_vel;
        vec3 hip_ang_vel;
        forward_kinematics_velocity(
            hip_pos,
            hip_vel,
            hip_rot,
            hip_ang_vel,
            bone_positions,
            bone_velocities,
            bone_rotations,
            bone_angular_velocities,
            db.bone_parents,
            Bone_Hips);

        quat inv_root_rot = quat_inv(bone_rotations(0));
        vec3 left_pos_local = quat_mul_vec3(inv_root_rot, left_foot_pos - bone_positions(0));
        vec3 right_pos_local = quat_mul_vec3(inv_root_rot, right_foot_pos - bone_positions(0));
        vec3 left_vel_local = quat_mul_vec3(inv_root_rot, left_foot_vel);
        vec3 right_vel_local = quat_mul_vec3(inv_root_rot, right_foot_vel);
        vec3 hip_vel_local = quat_mul_vec3(inv_root_rot, hip_vel);

        float left_terrain_height = 0.0f;
        float right_terrain_height = 0.0f;
        bool has_left_terrain = sample_terrain_height(ground_plane_model, left_foot_pos, left_terrain_height);
        bool has_right_terrain = sample_terrain_height(ground_plane_model, right_foot_pos, right_terrain_height);
        vec2 terrain_pair = vec2(
            has_left_terrain ? (left_terrain_height - hip_pos.y) : 0.0f,
            has_right_terrain ? (right_terrain_height - hip_pos.y) : 0.0f);

        root_history_positions.push_back(bone_positions(0));
        root_history_rotations.push_back(bone_rotations(0));
        history_left_foot_positions.push_back(left_pos_local);
        history_right_foot_positions.push_back(right_pos_local);
        history_left_foot_velocities.push_back(left_vel_local);
        history_right_foot_velocities.push_back(right_vel_local);
        history_hip_positions.push_back(hip_pos);
        history_hip_velocities.push_back(hip_vel_local);
        history_terrain_heights.push_back(terrain_pair);

        trim_runtime_history();
    };
    push_runtime_history();
    
    // Go

    float dt = 1.0f / 60.0f;
    
    // Metrics tracking
    float frame_time_ms = 0.0f;
    float fps_display = 0.0f;
#if defined(_WIN32) || defined(PLATFORM_WEB)
    runtime_metrics perf_metrics;
    runtime_metrics_init(perf_metrics);
    float perf_sample_timer = 0.0f;
    const float perf_sample_interval = 0.25f;
#endif

    char joystick_recording_folder[512] = "./resources/input-recording";
    char joystick_recording_output_file[768] = "./resources/input-recording/joystick_recording.csv";
    char joystick_recording_last_saved_file[768] = "./resources/input-recording/joystick_recording.csv";
    char joystick_recording_loaded_file[768] = "./resources/input-recording/joystick_recording.csv";
    bool joystick_recording_enabled = false;
    bool joystick_recording_last_save_ok = true;
    int joystick_recording_last_saved_count = 0;
    int joystick_recording_frame = 0;
    float joystick_recording_time = 0.0f;
    std::vector<joystick_record_sample> joystick_recording_samples;
    std::vector<joystick_record_sample> joystick_playback_samples;
    bool joystick_playback_enabled = false;
    bool joystick_playback_last_load_ok = false;
    int joystick_playback_last_loaded_count = 0;
    
    // Playback visualization: store both MM and LMM for rendering during playback
    std::vector<array1d<vec3>> playback_mm_bone_positions;
    std::vector<array1d<quat>> playback_mm_bone_rotations;
    std::vector<array1d<vec3>> playback_lmm_bone_positions;
    std::vector<array1d<quat>> playback_lmm_bone_rotations;
    
    bool show_stickman = false;
    int joystick_playback_index = 0;
    std::vector<std::string> joystick_recording_csv_files;
    int joystick_recording_csv_selected_index = 0;
    bool joystick_recording_csv_dropdown_edit = false;
    char joystick_recording_csv_dropdown_text[4096] = "<no csv files>";
    const float spawn_height_offset = 5.0f;
    vec3 joystick_recording_start_position = bone_positions(0) + vec3(0.0f, spawn_height_offset, 0.0f);
    quat joystick_recording_start_rotation = bone_rotations(0);
    Camera3D joystick_recording_start_camera = camera;
    float joystick_recording_start_camera_azimuth = camera_azimuth;
    float joystick_recording_start_camera_altitude = camera_altitude;
    float joystick_recording_start_camera_distance = camera_distance;

    auto reset_motion_to_recording_start = [&]()
    {
        simulation_position = joystick_recording_start_position;
        simulation_velocity = vec3();
        simulation_acceleration = vec3();
        simulation_rotation = joystick_recording_start_rotation;
        simulation_angular_velocity = vec3();

        desired_velocity = vec3();
        desired_velocity_change_curr = vec3();
        desired_velocity_change_prev = vec3();
        desired_rotation = joystick_recording_start_rotation;
        desired_rotation_change_curr = vec3();
        desired_rotation_change_prev = vec3();

        trajectory_positions.set(simulation_position);
        trajectory_velocities.set(vec3());
        trajectory_accelerations.set(vec3());
        trajectory_rotations.set(simulation_rotation);
        trajectory_angular_velocities.set(vec3());
        trajectory_desired_velocities.set(vec3());
        trajectory_desired_rotations.set(simulation_rotation);

        jump_active = false;
        jump_vertical_velocity = 0.0f;
        jump_buffer_timer = 0.0f;
        jump_coyote_timer = 0.0f;

        camera = joystick_recording_start_camera;
        camera_azimuth = joystick_recording_start_camera_azimuth;
        camera_altitude = joystick_recording_start_camera_altitude;
        camera_distance = joystick_recording_start_camera_distance;
    };

    // Snapshot baseline state so --analyze can replay the same clip in MM and LMM.
    const int base_frame_index = frame_index;
    const array1d<vec3> base_curr_bone_positions = curr_bone_positions;
    const array1d<vec3> base_curr_bone_velocities = curr_bone_velocities;
    const array1d<quat> base_curr_bone_rotations = curr_bone_rotations;
    const array1d<vec3> base_curr_bone_angular_velocities = curr_bone_angular_velocities;
    const array1d<bool> base_curr_bone_contacts = curr_bone_contacts;
    const array1d<vec3> base_trns_bone_positions = trns_bone_positions;
    const array1d<vec3> base_trns_bone_velocities = trns_bone_velocities;
    const array1d<quat> base_trns_bone_rotations = trns_bone_rotations;
    const array1d<vec3> base_trns_bone_angular_velocities = trns_bone_angular_velocities;
    const array1d<bool> base_trns_bone_contacts = trns_bone_contacts;
    const array1d<vec3> base_bone_positions = bone_positions;
    const array1d<vec3> base_bone_velocities = bone_velocities;
    const array1d<quat> base_bone_rotations = bone_rotations;
    const array1d<vec3> base_bone_angular_velocities = bone_angular_velocities;
    const array1d<vec3> base_bone_offset_positions = bone_offset_positions;
    const array1d<vec3> base_bone_offset_velocities = bone_offset_velocities;
    const array1d<quat> base_bone_offset_rotations = bone_offset_rotations;
    const array1d<vec3> base_bone_offset_angular_velocities = bone_offset_angular_velocities;
    const vec3 base_transition_src_position = transition_src_position;
    const quat base_transition_src_rotation = transition_src_rotation;
    const vec3 base_transition_dst_position = transition_dst_position;
    const quat base_transition_dst_rotation = transition_dst_rotation;
    const float base_search_timer = search_timer;
    const float base_force_search_timer = force_search_timer;
    const vec3 base_desired_velocity = desired_velocity;
    const vec3 base_desired_velocity_change_curr = desired_velocity_change_curr;
    const vec3 base_desired_velocity_change_prev = desired_velocity_change_prev;
    const quat base_desired_rotation = desired_rotation;
    const vec3 base_desired_rotation_change_curr = desired_rotation_change_curr;
    const vec3 base_desired_rotation_change_prev = desired_rotation_change_prev;
    const float base_desired_gait = desired_gait;
    const float base_desired_gait_velocity = desired_gait_velocity;
    const bool base_desired_crouch_prev = desired_crouch_prev;
    const bool base_desired_cartwheel_prev = desired_cartwheel_prev;
    const bool base_desired_idle_prev = desired_idle_prev;
    const bool base_desired_jump_prev = desired_jump_prev;
    const float base_cartwheel_auto_timer = cartwheel_auto_timer;
    const bool base_cartwheel_search_freeze_prev = cartwheel_search_freeze_prev;
    const bool base_cartwheel_query_lock_prev = cartwheel_query_lock_prev;
    const vec3 base_cartwheel_query_lock_forward = cartwheel_query_lock_forward;
    const float base_cartwheel_query_lock_step_distance = cartwheel_query_lock_step_distance;
    const float base_cartwheel_first_search_step_distance = cartwheel_first_search_step_distance;
    const vec3 base_simulation_position = simulation_position;
    const vec3 base_simulation_velocity = simulation_velocity;
    const vec3 base_simulation_acceleration = simulation_acceleration;
    const quat base_simulation_rotation = simulation_rotation;
    const vec3 base_simulation_angular_velocity = simulation_angular_velocity;
    const bool base_jump_active = jump_active;
    const float base_jump_vertical_velocity = jump_vertical_velocity;
    const float base_jump_buffer_timer = jump_buffer_timer;
    const float base_jump_coyote_timer = jump_coyote_timer;
    const array1d<vec3> base_trajectory_desired_velocities = trajectory_desired_velocities;
    const array1d<quat> base_trajectory_desired_rotations = trajectory_desired_rotations;
    const array1d<vec3> base_trajectory_positions = trajectory_positions;
    const array1d<vec3> base_trajectory_velocities = trajectory_velocities;
    const array1d<vec3> base_trajectory_accelerations = trajectory_accelerations;
    const array1d<quat> base_trajectory_rotations = trajectory_rotations;
    const array1d<vec3> base_trajectory_angular_velocities = trajectory_angular_velocities;
    const array1d<bool> base_contact_states = contact_states;
    const array1d<bool> base_contact_locks = contact_locks;
    const array1d<vec3> base_contact_positions = contact_positions;
    const array1d<vec3> base_contact_velocities = contact_velocities;
    const array1d<vec3> base_contact_points = contact_points;
    const array1d<vec3> base_contact_targets = contact_targets;
    const array1d<vec3> base_contact_offset_positions = contact_offset_positions;
    const array1d<vec3> base_contact_offset_velocities = contact_offset_velocities;
    const array1d<vec3> base_adjusted_bone_positions = adjusted_bone_positions;
    const array1d<quat> base_adjusted_bone_rotations = adjusted_bone_rotations;
    const array1d<float> base_features_proj = features_proj;
    const array1d<float> base_features_curr = features_curr;
    const array1d<float> base_latent_proj = latent_proj;
    const array1d<float> base_latent_curr = latent_curr;
    const array1d<vec2> base_future_toe_position = future_toe_position;
    const array1d<vec2> base_future_terrain_heights = future_terrain_heights;

    auto reset_runtime_for_analysis = [&]()
    {
        frame_index = base_frame_index;
        curr_bone_positions = base_curr_bone_positions;
        curr_bone_velocities = base_curr_bone_velocities;
        curr_bone_rotations = base_curr_bone_rotations;
        curr_bone_angular_velocities = base_curr_bone_angular_velocities;
        curr_bone_contacts = base_curr_bone_contacts;
        trns_bone_positions = base_trns_bone_positions;
        trns_bone_velocities = base_trns_bone_velocities;
        trns_bone_rotations = base_trns_bone_rotations;
        trns_bone_angular_velocities = base_trns_bone_angular_velocities;
        trns_bone_contacts = base_trns_bone_contacts;
        bone_positions = base_bone_positions;
        bone_velocities = base_bone_velocities;
        bone_rotations = base_bone_rotations;
        bone_angular_velocities = base_bone_angular_velocities;
        bone_offset_positions = base_bone_offset_positions;
        bone_offset_velocities = base_bone_offset_velocities;
        bone_offset_rotations = base_bone_offset_rotations;
        bone_offset_angular_velocities = base_bone_offset_angular_velocities;
        transition_src_position = base_transition_src_position;
        transition_src_rotation = base_transition_src_rotation;
        transition_dst_position = base_transition_dst_position;
        transition_dst_rotation = base_transition_dst_rotation;
        search_timer = base_search_timer;
        force_search_timer = base_force_search_timer;
        desired_velocity = base_desired_velocity;
        desired_velocity_change_curr = base_desired_velocity_change_curr;
        desired_velocity_change_prev = base_desired_velocity_change_prev;
        desired_rotation = base_desired_rotation;
        desired_rotation_change_curr = base_desired_rotation_change_curr;
        desired_rotation_change_prev = base_desired_rotation_change_prev;
        desired_gait = base_desired_gait;
        desired_gait_velocity = base_desired_gait_velocity;
        desired_crouch_prev = base_desired_crouch_prev;
        desired_cartwheel_prev = base_desired_cartwheel_prev;
        desired_idle_prev = base_desired_idle_prev;
        desired_jump_prev = base_desired_jump_prev;
        cartwheel_auto_timer = base_cartwheel_auto_timer;
        cartwheel_search_freeze_prev = base_cartwheel_search_freeze_prev;
        cartwheel_query_lock_prev = base_cartwheel_query_lock_prev;
        cartwheel_query_lock_forward = base_cartwheel_query_lock_forward;
        cartwheel_query_lock_step_distance = base_cartwheel_query_lock_step_distance;
        cartwheel_first_search_step_distance = base_cartwheel_first_search_step_distance;
        simulation_position = base_simulation_position;
        simulation_velocity = base_simulation_velocity;
        simulation_acceleration = base_simulation_acceleration;
        simulation_rotation = base_simulation_rotation;
        simulation_angular_velocity = base_simulation_angular_velocity;
        jump_active = base_jump_active;
        jump_vertical_velocity = base_jump_vertical_velocity;
        jump_buffer_timer = base_jump_buffer_timer;
        jump_coyote_timer = base_jump_coyote_timer;
        trajectory_desired_velocities = base_trajectory_desired_velocities;
        trajectory_desired_rotations = base_trajectory_desired_rotations;
        trajectory_positions = base_trajectory_positions;
        trajectory_velocities = base_trajectory_velocities;
        trajectory_accelerations = base_trajectory_accelerations;
        trajectory_rotations = base_trajectory_rotations;
        trajectory_angular_velocities = base_trajectory_angular_velocities;
        contact_states = base_contact_states;
        contact_locks = base_contact_locks;
        contact_positions = base_contact_positions;
        contact_velocities = base_contact_velocities;
        contact_points = base_contact_points;
        contact_targets = base_contact_targets;
        contact_offset_positions = base_contact_offset_positions;
        contact_offset_velocities = base_contact_offset_velocities;
        adjusted_bone_positions = base_adjusted_bone_positions;
        adjusted_bone_rotations = base_adjusted_bone_rotations;
        features_proj = base_features_proj;
        features_curr = base_features_curr;
        latent_proj = base_latent_proj;
        latent_curr = base_latent_curr;
        future_toe_position = base_future_toe_position;
        future_terrain_heights = base_future_terrain_heights;

        root_history_positions.clear();
        root_history_rotations.clear();
        history_left_foot_positions.clear();
        history_right_foot_positions.clear();
        history_left_foot_velocities.clear();
        history_right_foot_velocities.clear();
        history_hip_positions.clear();
        history_hip_velocities.clear();
        history_terrain_heights.clear();
        push_runtime_history();

        joystick_playback_enabled = false;
        joystick_playback_index = 0;
        joystick_playback_samples.clear();
        playback_mm_bone_positions.clear();
        playback_mm_bone_rotations.clear();
        playback_lmm_bone_positions.clear();
        playback_lmm_bone_rotations.clear();

        reset_motion_to_recording_start();
    };

    bool analysis_capture_enabled = false;
    std::vector<array1d<vec3>> analysis_capture_bone_positions;

#if defined(_WIN32)
    _mkdir(joystick_recording_folder);
#endif

    joystick_recording_refresh_csv_files(joystick_recording_folder, joystick_recording_csv_files);
    joystick_recording_build_dropdown_text(
        joystick_recording_csv_files,
        joystick_recording_csv_dropdown_text,
        sizeof(joystick_recording_csv_dropdown_text));

    if (debug) std::cout << "hm" << std::endl;

    auto update_func = [&]()
    {
        if (debug) std::cout << "update" << std::endl;
        // Get gamepad stick states
        vec3 gamepadstick_left = gamepad_get_stick(GAMEPAD_STICK_LEFT);
        vec3 gamepadstick_right = gamepad_get_stick(GAMEPAD_STICK_RIGHT);

        if (joystick_playback_enabled)
        {
            if (joystick_playback_index < (int)joystick_playback_samples.size())
            {
                const joystick_record_sample& sample = joystick_playback_samples[joystick_playback_index];
                gamepadstick_left = sample.left_stick;
                gamepadstick_right = sample.right_stick;
                joystick_playback_index += 1;
            }
            else
            {
                joystick_playback_enabled = false;
            }
        }
        
        // Clear playback data at the start of each frame when not in playback
        if (!joystick_playback_enabled)
        {
            playback_mm_bone_positions.clear();
            playback_mm_bone_rotations.clear();
            playback_lmm_bone_positions.clear();
            playback_lmm_bone_rotations.clear();
        }

        if (joystick_recording_enabled)
        {
            joystick_recording_samples.push_back(
                joystick_record_sample{
                    joystick_recording_frame,
                    joystick_recording_time,
                    gamepadstick_left,
                    gamepadstick_right,
                    simulation_position
                });
            joystick_recording_frame += 1;
            joystick_recording_time += dt;
        }

        // Keep runtime history so query history features use actual
        // previous character motion rather than database snapshots.
        push_runtime_history();
        
        // Get if strafe is desired
        bool desired_strafe = desired_strafe_update();
        bool desired_walk =
            IsGamepadButtonDown(GAMEPAD_PLAYER, GAMEPAD_BUTTON_RIGHT_FACE_DOWN) ||
            IsKeyDown(KEY_J);
        bool desired_crouch =
            IsGamepadButtonDown(GAMEPAD_PLAYER, GAMEPAD_BUTTON_RIGHT_FACE_UP) ||
            IsKeyDown(KEY_K);
        bool cartwheel_pressed =
            IsGamepadButtonPressed(GAMEPAD_PLAYER, GAMEPAD_BUTTON_RIGHT_FACE_LEFT) ||
            IsKeyPressed(KEY_L);
        bool desired_cartwheel =
            IsGamepadButtonDown(GAMEPAD_PLAYER, GAMEPAD_BUTTON_RIGHT_FACE_LEFT) ||
            IsKeyDown(KEY_L);
        bool crouch_pressed = desired_crouch;
        bool jump_pressed =
            IsGamepadButtonPressed(GAMEPAD_PLAYER, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT) ||
            IsKeyPressed(KEY_SPACE);

        if (cartwheel_pressed)
        {
            cartwheel_auto_timer = cartwheel_auto_duration;
        }

        bool cartwheel_auto_active = cartwheel_auto_timer > 0.0f;
        desired_cartwheel = desired_cartwheel || cartwheel_auto_active;
        bool cartwheel_query_lock_active = desired_cartwheel;

        if (joystick_playback_enabled)
        {
            desired_strafe = false;
            desired_walk = false;
            desired_crouch = false;
            desired_cartwheel = false;
            cartwheel_auto_timer = 0.0f;
            cartwheel_auto_active = false;
            crouch_pressed = false;
            jump_pressed = false;
            jump_buffer_timer = 0.0f;
        }

        cartwheel_query_lock_active = desired_cartwheel;

        bool cartwheel_search_freeze_active = cartwheel_auto_active;
        bool cartwheel_search_freeze_started =
            cartwheel_search_freeze_active && !cartwheel_search_freeze_prev;
        cartwheel_search_freeze_prev = cartwheel_search_freeze_active;
        if (cartwheel_search_freeze_started)
        {
            cartwheel_first_search_step_distance = simulation_run_fwrd_speed * (20.0f * dt);
        }

        if (cartwheel_auto_active)
        {
            desired_strafe = true;
        }

        jump_root_height_offset = crouch_pressed ? 0.7f : 1.2f;

        if (jump_pressed)
        {
            jump_buffer_timer = jump_buffer_time;
        }
        else
        {
            jump_buffer_timer = maxf(0.0f, jump_buffer_timer - dt);
        }
        
        // Get the desired gait (walk / run)
        desired_gait_update(
            desired_gait,
            desired_gait_velocity,
            desired_walk,
            dt);
        
        // Get the desired simulation speeds based on the gait
        float simulation_fwrd_speed = lerpf(simulation_run_fwrd_speed, simulation_walk_fwrd_speed, desired_gait);
        float simulation_side_speed = lerpf(simulation_run_side_speed, simulation_walk_side_speed, desired_gait);
        float simulation_back_speed = lerpf(simulation_run_back_speed, simulation_walk_back_speed, desired_gait);

        if (desired_crouch)
        {
            simulation_fwrd_speed = simulation_crouch_fwrd_speed;
            simulation_side_speed = simulation_crouch_side_speed;
            simulation_back_speed = simulation_crouch_back_speed;
        }

        float climbing_speed_scale = 1.0f;
        vec3 move_input_world = cartwheel_query_lock_active
            ? cartwheel_query_lock_forward
            : quat_mul_vec3(
                quat_from_angle_axis(camera_azimuth, vec3(0, 1, 0)),
                gamepadstick_left);
        move_input_world.y = 0.0f;

        if (length(move_input_world) > 0.01f)
        {
            vec3 move_dir = normalize(move_input_world);
            float terrain_height_curr = 0.0f;
            float terrain_height_ahead = 0.0f;
            vec3 probe_ahead = simulation_position + move_dir * climbing_probe_distance;

            if (sample_terrain_height(ground_plane_model, simulation_position, terrain_height_curr) &&
                sample_terrain_height(ground_plane_model, probe_ahead, terrain_height_ahead))
            {
                float slope_height_delta = terrain_height_ahead - terrain_height_curr;
                float slope_height_delta_abs = fabsf(slope_height_delta);
                if (slope_height_delta_abs > climbing_height_threshold)
                {
                    float steepness_t = (slope_height_delta_abs - climbing_height_threshold) /
                        maxf(climbing_max_height_delta - climbing_height_threshold, 0.0001f);
                    steepness_t = clampf(steepness_t, 0.0f, 1.0f);
                    climbing_speed_scale = lerpf(1.0f, climbing_min_speed_factor, steepness_t);
                }
            }
        }

        simulation_fwrd_speed *= climbing_speed_scale;
        simulation_side_speed *= climbing_speed_scale;
        simulation_back_speed *= climbing_speed_scale;

        if (cartwheel_query_lock_active)
        {
            if (!cartwheel_query_lock_prev)
            {
                vec3 lock_forward = quat_mul_vec3(simulation_rotation, vec3(0.0f, 0.0f, 1.0f));
                lock_forward.y = 0.0f;

                if (length(lock_forward) > 0.001f)
                {
                    cartwheel_query_lock_forward = normalize(lock_forward);
                }

                cartwheel_query_lock_step_distance = simulation_fwrd_speed * (20.0f * dt);
            }

            cartwheel_query_lock_prev = true;
        }
        else
        {
            cartwheel_query_lock_prev = false;
        }
        
        // Get the desired velocity
        vec3 desired_velocity_curr;
        if (cartwheel_query_lock_active)
        {
            // Ignore player steering while cartwheeling.
            desired_velocity_curr = cartwheel_query_lock_forward * simulation_fwrd_speed;
            desired_velocity_curr.y = 0.0f;
        }
        else
        {
            desired_velocity_curr = desired_velocity_update(
                gamepadstick_left,
                camera_azimuth,
                simulation_rotation,
                simulation_fwrd_speed,
                simulation_side_speed,
                simulation_back_speed);
        }

        float jump_ground_height = 0.0f;
        bool has_jump_ground = sample_terrain_height(
            ground_plane_model,
            simulation_position,
            jump_ground_height);
        float jump_grounded_target_height = jump_ground_height + jump_root_height_offset;
        bool jump_grounded = has_jump_ground &&
            fabsf(simulation_position.y - jump_grounded_target_height) <= jump_ground_snap_epsilon &&
            fabsf(simulation_velocity.y) <= jump_ground_velocity_epsilon;

        if (jump_grounded)
        {
            jump_coyote_timer = jump_coyote_time;
        }
        else
        {
            jump_coyote_timer = maxf(0.0f, jump_coyote_timer - dt);
        }

        if (jump_buffer_timer > 0.0f && jump_coyote_timer > 0.0f && !jump_active)
        {
            jump_active = true;
            jump_vertical_velocity = jump_initial_vertical_speed;
            jump_buffer_timer = 0.0f;
            jump_coyote_timer = 0.0f;
        }

        if (jump_active)
        {
            jump_vertical_velocity -= jump_gravity * dt;
            desired_velocity_curr.y = jump_vertical_velocity;
        }

        // Boost horizontal movement slightly for cartwheel and jump motions.
        {
            float motion_speed_boost = 1.0f;
            if (desired_cartwheel)
            {
                motion_speed_boost *= cartwheel_speed_boost;
            }
            if (jump_active)
            {
                motion_speed_boost *= jump_speed_boost;
            }

            if (motion_speed_boost > 1.0f)
            {
                vec3 planar = vec3(desired_velocity_curr.x, 0.0f, desired_velocity_curr.z);
                desired_velocity_curr.x = planar.x * motion_speed_boost;
                desired_velocity_curr.z = planar.z * motion_speed_boost;
            }
        }

        vec3 input_planar = gamepadstick_left;
        input_planar.y = 0.0f;
        float desired_input_magnitude = length(input_planar);

        vec3 simulation_velocity_planar = simulation_velocity;
        simulation_velocity_planar.y = 0.0f;
        float simulation_planar_speed = length(simulation_velocity_planar);

        const float idle_exit_input_threshold = 0.14f;
        const float idle_enter_speed_threshold = 0.15f;
        const float idle_exit_speed_threshold = 0.25f;

        bool desired_idle = desired_idle_prev;
        bool idle_enter =
            !jump_active &&
            simulation_planar_speed <= idle_enter_speed_threshold;
        bool idle_exit =
            jump_active ||
            desired_input_magnitude >= idle_exit_input_threshold ||
            simulation_planar_speed >= idle_exit_speed_threshold;

        if (desired_idle)
        {
            if (idle_exit)
            {
                desired_idle = false;
            }
        }
        else if (idle_enter)
        {
            desired_idle = true;
        }

        if (joystick_playback_enabled)
        {
            desired_idle = false;
        }

        bool desired_jump = jump_pressed || jump_active;
        if (joystick_playback_enabled)
        {
            desired_jump = false;
        }

        if (debug) std::cout << "test6" << std::endl;
        // Get the desired rotation/direction
        quat desired_rotation_curr = desired_rotation_update(
            desired_rotation,
            gamepadstick_left,
            gamepadstick_right,
            camera_azimuth,
            desired_strafe,
            desired_velocity_curr);

        if (cartwheel_auto_active)
        {
            // Keep trajectory position unchanged
            desired_rotation_curr = quat_mul(
                quat_from_angle_axis(0, vec3(0, 1, 0)),
                desired_rotation_curr);
        }
        
        // Check if we should force a search because input changed quickly
        desired_velocity_change_prev = desired_velocity_change_curr;
        desired_velocity_change_curr =  (desired_velocity_curr - desired_velocity) / dt;
        desired_velocity = desired_velocity_curr;
        
        desired_rotation_change_prev = desired_rotation_change_curr;
        desired_rotation_change_curr = quat_to_scaled_angle_axis(quat_abs(quat_mul_inv(desired_rotation_curr, desired_rotation))) / dt;
        desired_rotation =  desired_rotation_curr;
        
        bool force_search = false;

        if (desired_crouch != desired_crouch_prev)
        {
            force_search = true;
            force_search_timer = search_time;
            desired_crouch_prev = desired_crouch;
        }

        if (desired_cartwheel != desired_cartwheel_prev)
        {
            force_search = true;
            force_search_timer = search_time;
            desired_cartwheel_prev = desired_cartwheel;
        }

        if (desired_idle != desired_idle_prev)
        {
            force_search = true;
            force_search_timer = search_time;
            desired_idle_prev = desired_idle;
        }

        if (desired_jump != desired_jump_prev)
        {
            force_search = true;
            force_search_timer = search_time;
            desired_jump_prev = desired_jump;
        }

        if (force_search_timer <= 0.0f && (
            (length(desired_velocity_change_prev) >= desired_velocity_change_threshold && 
             length(desired_velocity_change_curr)  < desired_velocity_change_threshold)
        ||  (length(desired_rotation_change_prev) >= desired_rotation_change_threshold && 
             length(desired_rotation_change_curr)  < desired_rotation_change_threshold)))
        {
            force_search = true;
            force_search_timer = search_time;
        }
        else if (force_search_timer > 0)
        {
            force_search_timer -= dt;
        }

            if (cartwheel_auto_timer > 0.0f)
            {
                cartwheel_auto_timer = maxf(0.0f, cartwheel_auto_timer - dt);
            }
        
        // Predict Future Trajectory
        
        trajectory_desired_rotations_predict(
          trajectory_desired_rotations,
          trajectory_desired_velocities,
          desired_rotation,
          camera_azimuth,
                    gamepadstick_left,
                    gamepadstick_right,
          desired_strafe,
          20.0f * dt);
        
        trajectory_rotations_predict(
            trajectory_rotations,
            trajectory_angular_velocities,
            simulation_rotation,
            simulation_angular_velocity,
            trajectory_desired_rotations,
            simulation_rotation_halflife,
            20.0f * dt);
        
        trajectory_desired_velocities_predict(
          trajectory_desired_velocities,
          trajectory_rotations,
          desired_velocity,
          simulation_position,
          ground_plane_model,
          jump_active,
          jump_vertical_velocity,
          jump_gravity,
          jump_root_height_offset,
          camera_azimuth,
          gamepadstick_left,
          gamepadstick_right,
          desired_strafe,
          simulation_fwrd_speed,
          simulation_side_speed,
          simulation_back_speed,
          20.0f * dt);
        
        trajectory_positions_predict(
            trajectory_positions,
            trajectory_velocities,
            trajectory_accelerations,
            simulation_position,
            simulation_velocity,
            simulation_acceleration,
            trajectory_desired_velocities,
            simulation_velocity_halflife,
            20.0f * dt,
            ground_plane_model);

        if (length(gamepadstick_left) < 0.01f)
        {
            for (int i = 1; i < trajectory_positions.size; i++)
            {
                trajectory_positions(i).x = simulation_position.x;
                trajectory_positions(i).z = simulation_position.z;
            }
        }

        float current_terrain_height = 0.0f;
        const bool has_current_terrain = sample_terrain_height(
            ground_plane_model,
            simulation_position,
            current_terrain_height);

        const float root_ground_offset = has_current_terrain
            ? (simulation_position.y - current_terrain_height)
            : jump_root_height_offset;

        const float trajectory_y_speed = 5.0f; // m/s
        auto terrain_anchor_trajectory = [&](slice1d<vec3> trajectory)
        {
            for (int i = 1; i < trajectory.size; i++)
            {
                float terrain_height = 0.0f;
                if (sample_terrain_height(ground_plane_model, trajectory(i), terrain_height))
                {
                    float target_y = terrain_height + jump_root_height_offset;
                    float current_y = trajectory(i - 1).y;
                    float max_delta = trajectory_y_speed * (20.0f * dt);
                    
                    if (current_y < target_y) {
                        trajectory(i).y = minf(current_y + max_delta, target_y);
                    } else {
                        trajectory(i).y = maxf(current_y - max_delta, target_y);
                    }
                }
            }
        };

        terrain_anchor_trajectory(trajectory_positions);

        // If future trajectory rises upward, reduce horizontal reach for that point.
        const float uphill_horizontal_reduce_gain = 1.0f;
        const float uphill_horizontal_reduce_max = 0.75f;
        float min_trajectory_scale_xz = 1.0f;
        for (int i = 1; i < trajectory_positions.size; i++)
        {
            vec3 rel = trajectory_positions(i) - trajectory_positions(i-1);
            if (true) // rel.y > 0.0f
            {
                float reduce = clampf(fabsf(rel.y) * uphill_horizontal_reduce_gain, 0.0f, uphill_horizontal_reduce_max);
                float scale_xz = 1.0f - reduce;
                if (i < 3) {
                    min_trajectory_scale_xz = minf(min_trajectory_scale_xz, scale_xz);
                }
                
                trajectory_positions(i).x = trajectory_positions(i-1).x + rel.x * scale_xz;
                trajectory_positions(i).z = trajectory_positions(i-1).z + rel.z * scale_xz;
            }
        }

        // Keep desired velocity in sync with the strongest trajectory compression.
        float trajectory_xz_speed_scale = clampf(min_trajectory_scale_xz, 0.0f, 1.0f);
        desired_velocity_curr.x *= trajectory_xz_speed_scale;
        desired_velocity_curr.z *= trajectory_xz_speed_scale;
        desired_velocity.x *= trajectory_xz_speed_scale;
        desired_velocity.z *= trajectory_xz_speed_scale;

        // XZ rescaling above changes sample points, so project Y again to
        // keep the visible trajectory aligned with terrain elevation.
        terrain_anchor_trajectory(trajectory_positions);

        array1d<vec3> query_trajectory_positions = trajectory_positions;
        array1d<quat> query_trajectory_rotations = trajectory_rotations;

        if (cartwheel_query_lock_active)
        {
            float lock_yaw = atan2f(cartwheel_query_lock_forward.x, cartwheel_query_lock_forward.z);
            quat lock_rotation = quat_from_angle_axis(lock_yaw - 0.5f * PIf, vec3(0.0f, 1.0f, 0.0f));
            vec3 base_position = bone_positions(0);

            query_trajectory_positions(0) = base_position;
            query_trajectory_rotations(0) = lock_rotation;

            for (int i = 1; i < query_trajectory_positions.size; i++)
            {
                float step_distance = cartwheel_search_freeze_started
                    ? cartwheel_first_search_step_distance
                    : cartwheel_query_lock_step_distance;
                float distance = step_distance * (float)i;
                query_trajectory_positions(i) = base_position + cartwheel_query_lock_forward * distance;
                query_trajectory_rotations(i) = lock_rotation;
            }
        }

        terrain_anchor_trajectory(query_trajectory_positions);

        // Override: Add vertical velocity to move root toward terrain sampled along future trajectory.
        float traj_ground_height = 0.0f;
        bool traj_hit = false;
        int nearest_future_idx = trajectory_positions.size > 1 ? 1 : 0;
        Ray traj_ray = { to_Vector3(trajectory_positions(nearest_future_idx) + vec3(0, 10, 0)), {0, -1, 0} };
        for (int i = 0; i < ground_plane_model.meshCount; i++)
        {
            RayCollision traj_collision = GetRayCollisionMesh(traj_ray, ground_plane_model.meshes[i], ground_plane_model.transform);
            if (traj_collision.hit && (!traj_hit || traj_collision.point.y > traj_ground_height))
            {
                traj_ground_height = traj_collision.point.y;
                traj_hit = true;
            }
        }

        if (!jump_active)
        {
            float target_root_height = traj_ground_height + jump_root_height_offset;
            float height_error = target_root_height - simulation_position.y;
            const float vertical_gain = 4.0f;
            const float damping_gain = 0.5f;  // Damping coefficient
            
            // Reduce desired velocity if already moving in that direction
            float damped_command = height_error * vertical_gain - damping_gain * simulation_velocity.y;
            desired_velocity_curr.y = clampf(damped_command, kTerrainFollowMinVerticalSpeed, kTerrainFollowMaxVerticalSpeed);
        }

        // Blend a small amount of root velocity to reduce abrupt target changes.
        const float desired_velocity_root_blend = 0.0f;
        vec3 desired_velocity_blended = lerp(desired_velocity_curr, bone_velocities(0), desired_velocity_root_blend);
        desired_velocity_blended.y = desired_velocity_curr.y;
        desired_velocity_curr = desired_velocity_blended;
        desired_velocity.y = desired_velocity_curr.y;
        
        // Compute future toe terrain heights relative to hips
        // 4 time samples: current (0), +15, +30, +45 frames
        // future_toe_position contains: 3 frames x 2 toes x 2D positions
        {
            // Compute forward kinematics to get hip global position
            global_bone_computed.zero();
            forward_kinematics_partial(
                global_bone_positions,
                global_bone_rotations,
                global_bone_computed,
                bone_positions,
                bone_rotations,
                db.bone_parents,
                Bone_Hips);
            
            float hip_height = global_bone_positions(Bone_Hips).y;
            
            // Store relative heights for both toes at each time sample
            for (int time_idx = 0; time_idx < 4; time_idx++)
            {
                vec3 left_toe_pos;
                vec3 right_toe_pos;
                
                if (time_idx == 0)
                {
                    // Current frame: get actual toe bone positions
                    forward_kinematics_velocity(
                        left_toe_pos,
                        bone_velocities(contact_bones(0)),
                        bone_rotations(contact_bones(0)),
                        bone_angular_velocities(contact_bones(0)),
                        bone_positions,
                        bone_velocities,
                        bone_rotations,
                        bone_angular_velocities,
                        db.bone_parents,
                        contact_bones(0));
                    
                    forward_kinematics_velocity(
                        right_toe_pos,
                        bone_velocities(contact_bones(1)),
                        bone_rotations(contact_bones(1)),
                        bone_angular_velocities(contact_bones(1)),
                        bone_positions,
                        bone_velocities,
                        bone_rotations,
                        bone_angular_velocities,
                        db.bone_parents,
                        contact_bones(1));
                }
                else
                {
                    // Future frames: extract from future_toe_position
                    // Array stores: [left0, right0, left1, right1, left2, right2]
                    int future_idx = time_idx - 1;
                    
                    // Extract left toe position (x, z) to 3D (x, 0, z)
                    vec3 left_toe_local = vec3(
                        future_toe_position(future_idx * 2 + 0).x, 
                        0.0f, 
                        future_toe_position(future_idx * 2 + 0).y);
                    
                    // Extract right toe position (x, z) to 3D (x, 0, z)
                    vec3 right_toe_local = vec3(
                        future_toe_position(future_idx * 2 + 1).x,
                        0.0f,
                        future_toe_position(future_idx * 2 + 1).y);
                    
                    // Transform from character-local to world space
                    left_toe_pos = quat_mul_vec3(bone_rotations(0), left_toe_local) + bone_positions(0);
                    right_toe_pos = quat_mul_vec3(bone_rotations(0), right_toe_local) + bone_positions(0);
                }
                
                // Raycast from above to find terrain height
                float left_terrain_height = 0.0f;
                float right_terrain_height = 0.0f;
                
                // Cast rays from 10 units above down to 10 units below
                Ray left_ray = { to_Vector3(left_toe_pos + vec3(0, 10, 0)), {0, -1, 0} };
                Ray right_ray = { to_Vector3(right_toe_pos + vec3(0, 10, 0)), {0, -1, 0} };
                
                // Check collision with ground plane meshes
                for (int i = 0; i < ground_plane_model.meshCount; i++)
                {
                    RayCollision left_collision = GetRayCollisionMesh(left_ray, ground_plane_model.meshes[i], ground_plane_model.transform);
                    if (left_collision.hit && (left_terrain_height == 0.0f || left_collision.point.y > left_terrain_height))
                    {
                        left_terrain_height = left_collision.point.y;
                    }
                    
                    RayCollision right_collision = GetRayCollisionMesh(right_ray, ground_plane_model.meshes[i], ground_plane_model.transform);
                    if (right_collision.hit && (right_terrain_height == 0.0f || right_collision.point.y > right_terrain_height))
                    {
                        right_terrain_height = right_collision.point.y;
                    }
                }
                
                // Store relative to hip height, but clamp to avoid extreme negatives while falling.
                const float min_terrain_feature_height = -2.0f;
                float left_relative_terrain_height = left_terrain_height - hip_height;
                float right_relative_terrain_height = right_terrain_height - hip_height;

                future_terrain_heights(time_idx) = vec2(
                    maxf(left_relative_terrain_height, min_terrain_feature_height),
                    maxf(right_relative_terrain_height, min_terrain_feature_height));

                // std::cout << "LeftToe Terrain World Terrain height: " << left_terrain_height << std::endl;
                // std::cout << "Hip's world position: " << hip_height << std::endl;
                // std::cout << "Player's world position: " << bone_positions(0).x << " " << bone_positions(0).y << " " << bone_positions(0).z << std::endl;
                // std::cout << std::endl;
            }
        }
        if (debug) std::cout << "test5" << std::endl;
           
        // Make query vector for search.
        // In theory this only needs to be done when a search is 
        // actually required however for visualization purposes it
        // can be nice to do it every frame
        array1d<float> query(db.nfeatures());
                
        // Compute the features of the query vector
        if (debug) std::cout << "Getting query features..." << std::endl;
        if (debug) std::cout << "frame_index=" << frame_index << std::endl;

        
        bool lmm_runtime_enabled = lmm_enabled && lmm_networks_compatible;
        slice1d<float> query_features = lmm_runtime_enabled ? slice1d<float>(features_curr) : db.features(frame_index);
        if (debug) std::cout << "Got query features, size=" << query_features.size << std::endl;
        
        int offset = 0;
        if (debug) std::cout << "Query" << std::endl;
        if (debug) std::cout << "  Copying left foot position..." << std::endl;
        query_copy_denormalized_feature(query, offset, 3, query_features, db.features_offset, db.features_scale); // Left Foot Position
        if (debug) std::cout << "  Copying right foot position..." << std::endl;
        query_copy_denormalized_feature(query, offset, 3, query_features, db.features_offset, db.features_scale); // Right Foot Position
        if (debug) std::cout << "  Copying left foot velocity..." << std::endl;
        query_copy_denormalized_feature(query, offset, 3, query_features, db.features_offset, db.features_scale); // Left Foot Velocity
        if (debug) std::cout << "  Copying right foot velocity..." << std::endl;
        query_copy_denormalized_feature(query, offset, 3, query_features, db.features_offset, db.features_scale); // Right Foot Velocity
        if (debug) std::cout << "  Copying hip velocity..." << std::endl;
        query_copy_denormalized_feature(query, offset, 3, query_features, db.features_offset, db.features_scale); // Hip Velocity
        if (debug) std::cout << "  Computing trajectory position feature..." << std::endl;
        query_compute_trajectory_position_feature(query, offset, bone_positions(0), bone_rotations(0), query_trajectory_positions);
        if (debug) std::cout << "  Computing trajectory direction feature..." << std::endl;
        query_compute_trajectory_direction_feature(
            query,
            offset,
            bone_positions(0),
            bone_rotations(0),
            query_trajectory_positions,
            query_trajectory_rotations);
        if (debug) std::cout << "  Computing terrain height feature..." << std::endl;
        query_compute_terrain_height_feature(query, offset, future_terrain_heights);

        if (offset < db.nfeatures())
        {
            if (debug) std::cout << "  Setting idle flag..." << std::endl;
            query(offset) = desired_idle ? 1.0f : 0.0f;
            offset += 1;
        }
        if (offset < db.nfeatures())
        {
            if (debug) std::cout << "  Setting crouch flag..." << std::endl;
            query(offset) = desired_crouch ? 1.0f : 0.0f;
            offset += 1;
        }
        if (offset < db.nfeatures())
        {
            if (debug) std::cout << "  Setting jump flag..." << std::endl;
            query(offset) = desired_jump ? 1.0f : 0.0f;
            offset += 1;
        }
        if (offset < db.nfeatures())
        {
            if (debug) std::cout << "  Setting cartwheel flag..." << std::endl;
            query(offset) = desired_cartwheel ? 1.0f : 0.0f;
            offset += 1;
        }

        auto sample_runtime_history_idx = [&](int relative_offset) -> int
        {
            if (root_history_positions.empty())
            {
                return 0;
            }
            int last = (int)root_history_positions.size() - 1;
            return clamp(last + relative_offset, 0, last);
        };

        auto query_write_runtime_history_vec3 = [&](const std::vector<vec3>& history, int relative_offset)
        {
            if (history.empty())
            {
                query(offset + 0) = 0.0f;
                query(offset + 1) = 0.0f;
                query(offset + 2) = 0.0f;
            }
            else
            {
                int idx = sample_runtime_history_idx(relative_offset);
                query(offset + 0) = history[idx].x;
                query(offset + 1) = history[idx].y;
                query(offset + 2) = history[idx].z;
            }
            offset += 3;
        };

        query_write_runtime_history_vec3(history_left_foot_positions, -20);
        query_write_runtime_history_vec3(history_right_foot_positions, -20);
        query_write_runtime_history_vec3(history_left_foot_velocities, -20);
        query_write_runtime_history_vec3(history_right_foot_velocities, -20);
        query_write_runtime_history_vec3(history_hip_velocities, -20);

        auto sample_runtime_history_root = [&](int relative_offset, vec3& out_pos, quat& out_rot)
        {
            if (root_history_positions.empty())
            {
                out_pos = bone_positions(0);
                out_rot = bone_rotations(0);
                return;
            }

            int idx = sample_runtime_history_idx(relative_offset);
            out_pos = root_history_positions[idx];
            out_rot = root_history_rotations[idx];
        };

        auto query_write_runtime_history_trajectory = [&](int history_offset)
        {
            vec3 p_pos, t_pos;
            quat p_rot, t_rot;

            sample_runtime_history_root(history_offset, p_pos, p_rot);
            sample_runtime_history_root(history_offset + 20, t_pos, t_rot);

            vec3 traj_pos = quat_inv_mul_vec3(p_rot, t_pos - p_pos);
            vec3 traj_dir = quat_inv_mul_vec3(p_rot, quat_mul_vec3(t_rot, vec3(0, 0, 1)));

            const float eps = 1e-4f;
            float h = length(vec3(traj_pos.x, 0.0f, traj_pos.z));
            traj_dir.y = traj_pos.y / maxf(h, eps);
            traj_dir = normalize(traj_dir);

            query(offset + 0) = traj_pos.x;
            query(offset + 1) = traj_pos.y;
            query(offset + 2) = traj_pos.z;
            offset += 3;

            query(offset + 0) = traj_dir.x;
            query(offset + 1) = traj_dir.y;
            query(offset + 2) = traj_dir.z;
            offset += 3;
        };

        query_write_runtime_history_trajectory(-20);

        if (history_terrain_heights.empty())
        {
            query(offset + 0) = 0.0f;
            query(offset + 1) = 0.0f;
        }
        else
        {
            int terrain_idx = sample_runtime_history_idx(-15);
            query(offset + 0) = history_terrain_heights[terrain_idx].x;
            query(offset + 1) = history_terrain_heights[terrain_idx].y;
        }
        offset += 2;
        if (debug) std::cout << "Done Query" << std::endl;
        assert(offset == db.nfeatures());
        if (debug) std::cout << "Done assert" << std::endl;
        // Check if we reached the end of the current anim
        bool end_of_anim = database_index_clamp(db, frame_index, 1) == frame_index;
        
        // Do we need to search?
        if (debug) std::cout << "Do we?" << std::endl;
        bool search_requested = force_search || search_timer <= 0.0f || end_of_anim;
        bool force_projector_on_cartwheel_freeze_start =
            lmm_runtime_enabled && cartwheel_search_freeze_started;
        bool force_mm_search_on_cartwheel_freeze_start =
            !lmm_runtime_enabled && cartwheel_search_freeze_started;
        bool allow_mm_search =
            !cartwheel_search_freeze_active || force_mm_search_on_cartwheel_freeze_start;
        bool allow_lmm_projector =
            !cartwheel_search_freeze_active || force_projector_on_cartwheel_freeze_start;
        bool ran_search_or_projector = false;

        if (search_requested || force_projector_on_cartwheel_freeze_start)
        {
            if (lmm_runtime_enabled)
            {
                if (allow_lmm_projector)
                {
                    // Project query onto nearest feature vector
                    
                    float best_cost = FLT_MAX;
                    bool transition = false;
                    ran_search_or_projector = true;
                    
                    projector_evaluate(
                        transition,
                        best_cost,
                        features_proj,
                        latent_proj,
                        projector_evaluation,
                        query,
                        db.features_offset,
                        db.features_scale,
                        features_curr,
                        projector);
                    
                    // If projection is sufficiently different from current
                    if (transition)
                    {   
                        // Evaluate pose for projected features
                        decompressor_evaluate(
                            trns_bone_positions,
                            trns_bone_velocities,
                            trns_bone_rotations,
                            trns_bone_angular_velocities,
                            trns_bone_contacts,
                            future_toe_position,
                            decompressor_evaluation,
                            features_proj,
                            latent_proj,
                            curr_bone_positions(0),
                            curr_bone_rotations(0),
                            decompressor,
                            dt);
                        
                        // Transition inertializer to this pose
                        inertialize_pose_transition(
                            bone_offset_positions,
                            bone_offset_velocities,
                            bone_offset_rotations,
                            bone_offset_angular_velocities,
                            transition_src_position,
                            transition_src_rotation,
                            transition_dst_position,
                            transition_dst_rotation,
                            bone_positions(0),
                            bone_velocities(0),
                            bone_rotations(0),
                            bone_angular_velocities(0),
                            curr_bone_positions,
                            curr_bone_velocities,
                            curr_bone_rotations,
                            curr_bone_angular_velocities,
                            trns_bone_positions,
                            trns_bone_velocities,
                            trns_bone_rotations,
                            trns_bone_angular_velocities);
                        
                        // Update current features and latents
                        features_curr = features_proj;
                        latent_curr = latent_proj;
                    }
                }
            }
            else
            {
                if (allow_mm_search)
                {
                    int best_index_with_history = end_of_anim ? -1 : frame_index;
                    int best_index_without_history = end_of_anim ? -1 : frame_index;
                    float best_cost_with_history = FLT_MAX;
                    float best_cost_without_history = FLT_MAX;
                    ran_search_or_projector = true;

                    const bool run_with_history =
                        mm_history_mode == MM_HISTORY_SEARCH_ON ||
                        mm_history_mode == MM_HISTORY_SEARCH_BOTH;
                    const bool run_without_history =
                        mm_history_mode == MM_HISTORY_SEARCH_OFF ||
                        mm_history_mode == MM_HISTORY_SEARCH_BOTH;

                    if (run_with_history)
                    {
                        database_search(
                            best_index_with_history,
                            best_cost_with_history,
                            db,
                            query,
                            0.0f,
                            20,
                            20,
                            true);

                        if (best_index_with_history < 0 || best_index_with_history >= db.nframes())
                        {
                            best_index_with_history = frame_index;
                        }
                        if (best_index_with_history != mm_last_best_with_history)
                        {
                            array1d<vec3> trns_pos = db.bone_positions(best_index_with_history);
                            array1d<vec3> trns_vel = db.bone_velocities(best_index_with_history);
                            array1d<quat> trns_rot = db.bone_rotations(best_index_with_history);
                            array1d<vec3> trns_ang_vel = db.bone_angular_velocities(best_index_with_history);
                            
                            inertialize_pose_transition(
                                bone_offset_positions_with_history, bone_offset_velocities_with_history,
                                bone_offset_rotations_with_history, bone_offset_angular_velocities_with_history,
                                transition_src_position_with_history, transition_src_rotation_with_history,
                                transition_dst_position_with_history, transition_dst_rotation_with_history,
                                bone_positions_with_history(0), bone_velocities_with_history(0),
                                bone_rotations_with_history(0), bone_angular_velocities_with_history(0),
                                curr_bone_positions_with_history, curr_bone_velocities_with_history,
                                curr_bone_rotations_with_history, curr_bone_angular_velocities_with_history,
                                trns_pos, trns_vel, trns_rot, trns_ang_vel);
                        }
                        mm_last_best_with_history = best_index_with_history;
                    }

                    if (run_without_history)
                    {
                        database_search(
                            best_index_without_history,
                            best_cost_without_history,
                            db,
                            query,
                            0.0f,
                            20,
                            20,
                            false);

                        if (best_index_without_history < 0 || best_index_without_history >= db.nframes())
                        {
                            best_index_without_history = frame_index;
                        }
                        if (best_index_without_history != mm_last_best_without_history)
                        {
                            array1d<vec3> trns_pos = db.bone_positions(best_index_without_history);
                            array1d<vec3> trns_vel = db.bone_velocities(best_index_without_history);
                            array1d<quat> trns_rot = db.bone_rotations(best_index_without_history);
                            array1d<vec3> trns_ang_vel = db.bone_angular_velocities(best_index_without_history);
                            
                            inertialize_pose_transition(
                                bone_offset_positions_without_history, bone_offset_velocities_without_history,
                                bone_offset_rotations_without_history, bone_offset_angular_velocities_without_history,
                                transition_src_position_without_history, transition_src_rotation_without_history,
                                transition_dst_position_without_history, transition_dst_rotation_without_history,
                                bone_positions_without_history(0), bone_velocities_without_history(0),
                                bone_rotations_without_history(0), bone_angular_velocities_without_history(0),
                                curr_bone_positions_without_history, curr_bone_velocities_without_history,
                                curr_bone_rotations_without_history, curr_bone_angular_velocities_without_history,
                                trns_pos, trns_vel, trns_rot, trns_ang_vel);
                        }
                        mm_last_best_without_history = best_index_without_history;
                    }

                    int best_index = frame_index;
                    if (mm_history_mode == MM_HISTORY_SEARCH_ON)
                    {
                        best_index = best_index_with_history;
                    }
                    else if (mm_history_mode == MM_HISTORY_SEARCH_OFF)
                    {
                        best_index = best_index_without_history;
                    }
                    else
                    {
                        // In BOTH mode, drive runtime from the history-enabled search.
                        best_index = best_index_with_history;
                    }
                    
                    // Transition if better frame found
                    if (debug) std::cout << "Do2" << std::endl;
                    if (best_index != frame_index)
                    {
                        trns_bone_positions = db.bone_positions(best_index);
                        trns_bone_velocities = db.bone_velocities(best_index);
                        trns_bone_rotations = db.bone_rotations(best_index);
                        trns_bone_angular_velocities = db.bone_angular_velocities(best_index);
                        
                        inertialize_pose_transition(
                            bone_offset_positions,
                            bone_offset_velocities,
                            bone_offset_rotations,
                            bone_offset_angular_velocities,
                            transition_src_position,
                            transition_src_rotation,
                            transition_dst_position,
                            transition_dst_rotation,
                            bone_positions(0),
                            bone_velocities(0),
                            bone_rotations(0),
                            bone_angular_velocities(0),
                            curr_bone_positions,
                            curr_bone_velocities,
                            curr_bone_rotations,
                            curr_bone_angular_velocities,
                            trns_bone_positions,
                            trns_bone_velocities,
                            trns_bone_rotations,
                            trns_bone_angular_velocities);
                        
                        frame_index = best_index;
                    }
                }
            }

            // Reset search timer
            if (ran_search_or_projector)
            {
                search_timer = search_time;
            }
        }
        
        // Tick down search timer
        search_timer -= dt;
        if (debug) std::cout << "test4" << std::endl;

        if (lmm_runtime_enabled)
        {
            // Update features and latents
            stepper_evaluate(
                features_curr,
                latent_curr,
                stepper_evaluation,
                stepper,
                dt);
            
            // Decompress next pose
            decompressor_evaluate(
                curr_bone_positions,
                curr_bone_velocities,
                curr_bone_rotations,
                curr_bone_angular_velocities,
                curr_bone_contacts,
                future_toe_position,
                decompressor_evaluation,
                features_curr,
                latent_curr,
                curr_bone_positions(0),
                curr_bone_rotations(0),
                decompressor,
                dt);
        }
        else
        {
            // Tick frame
            frame_index++; // Assumes dt is fixed to 60fps
            mm_last_best_with_history = database_index_clamp(db, mm_last_best_with_history, 1);
            mm_last_best_without_history = database_index_clamp(db, mm_last_best_without_history, 1);
            
            // Look-up Next Pose
            curr_bone_positions = db.bone_positions(frame_index);
            curr_bone_velocities = db.bone_velocities(frame_index);
            curr_bone_rotations = db.bone_rotations(frame_index);
            curr_bone_angular_velocities = db.bone_angular_velocities(frame_index);
            curr_bone_contacts = db.contact_states(frame_index);
            curr_bone_positions_with_history = db.bone_positions(mm_last_best_with_history);
            curr_bone_velocities_with_history = db.bone_velocities(mm_last_best_with_history);
            curr_bone_rotations_with_history = db.bone_rotations(mm_last_best_with_history);
            curr_bone_angular_velocities_with_history = db.bone_angular_velocities(mm_last_best_with_history);

            curr_bone_positions_without_history = db.bone_positions(mm_last_best_without_history);
            curr_bone_velocities_without_history = db.bone_velocities(mm_last_best_without_history);
            curr_bone_rotations_without_history = db.bone_rotations(mm_last_best_without_history);
            curr_bone_angular_velocities_without_history = db.bone_angular_velocities(mm_last_best_without_history);
            
            // Retrieve precomputed future_toe_position from database
            // Database stores 12 floats per frame: [L15_x, L15_z, R15_x, R15_z, L30_x, L30_z, R30_x, R30_z, L45_x, L45_z, R45_x, R45_z]
            // Convert to 6 vec2 values: [left0, right0, left1, right1, left2, right2]
            for (int i = 0; i < 6; i++)
            {
                future_toe_position(i) = vec2(
                    db.future_toe_positions(frame_index, i * 2 + 0),
                    db.future_toe_positions(frame_index, i * 2 + 1));
            }
        }
        
        // Update inertializer
        
        inertialize_pose_update(
            bone_positions,
            bone_velocities,
            bone_rotations,
            bone_angular_velocities,
            bone_offset_positions,
            bone_offset_velocities,
            bone_offset_rotations,
            bone_offset_angular_velocities,
            curr_bone_positions,
            curr_bone_velocities,
            curr_bone_rotations,
            curr_bone_angular_velocities,
            transition_src_position,
            transition_src_rotation,
            transition_dst_position,
            transition_dst_rotation,
            inertialize_blending_halflife,
            dt);
        
        inertialize_pose_update(
            bone_positions_with_history, bone_velocities_with_history,
            bone_rotations_with_history, bone_angular_velocities_with_history,
            bone_offset_positions_with_history, bone_offset_velocities_with_history,
            bone_offset_rotations_with_history, bone_offset_angular_velocities_with_history,
            curr_bone_positions_with_history, curr_bone_velocities_with_history,
            curr_bone_rotations_with_history, curr_bone_angular_velocities_with_history,
            transition_src_position_with_history, transition_src_rotation_with_history,
            transition_dst_position_with_history, transition_dst_rotation_with_history,
            inertialize_blending_halflife, dt);

        inertialize_pose_update(
            bone_positions_without_history, bone_velocities_without_history,
            bone_rotations_without_history, bone_angular_velocities_without_history,
            bone_offset_positions_without_history, bone_offset_velocities_without_history,
            bone_offset_rotations_without_history, bone_offset_angular_velocities_without_history,
            curr_bone_positions_without_history, curr_bone_velocities_without_history,
            curr_bone_rotations_without_history, curr_bone_angular_velocities_without_history,
            transition_src_position_without_history, transition_src_rotation_without_history,
            transition_dst_position_without_history, transition_dst_rotation_without_history,
            inertialize_blending_halflife, dt);
        
        // Update Simulation
        if (debug) std::cout << "test3" << std::endl;
        
        vec3 simulation_position_prev = simulation_position;

        // Move by raw matched root velocity (no spring damping/smoothing).
        if (jump_active)
        {
            simulation_velocity.y = jump_vertical_velocity;
            simulation_position.y += simulation_velocity.y * dt;
            simulation_velocity.x = bone_velocities(0).x;
            simulation_velocity.z = bone_velocities(0).z;
            simulation_position.x = bone_positions(0).x;
            simulation_position.z = bone_positions(0).z;
        }
        else
        {
            simulation_velocity = bone_velocities(0);
            simulation_position = bone_positions(0);
        }
        simulation_acceleration = vec3();

        if (jump_active)
        {
            float landing_terrain_height = 0.0f;
            if (sample_terrain_height(ground_plane_model, simulation_position, landing_terrain_height))
            {
                float landing_target_height = landing_terrain_height + jump_root_height_offset;
                bool landed =
                    simulation_position.y <= landing_target_height + jump_ground_snap_epsilon &&
                    simulation_velocity.y <= 0.0f;

                if (landed)
                {
                    jump_active = false;
                    jump_vertical_velocity = 0.0f;
                    simulation_position.y = maxf(simulation_position.y, landing_target_height);
                    simulation_velocity.y = 0.0f;
                    simulation_acceleration.y = 0.0f;
                }
            }
        }
            
        simulation_rotations_update(
            simulation_rotation, 
            simulation_angular_velocity, 
            desired_rotation,
            simulation_rotation_halflife,
            dt);
        
        // Synchronization 
        
        if (synchronization_enabled)
        {
            vec3 synchronized_position = lerp(
                simulation_position, 
                bone_positions(0),
                synchronization_data_factor);
                
            quat synchronized_rotation = quat_nlerp_shortest(
                simulation_rotation,
                bone_rotations(0), 
                synchronization_data_factor);
          
            // synchronized_position = simulation_collide_obstacles(
            //     simulation_position_prev,
            //     synchronized_position,
            //     ground_plane_model);
            
            simulation_position = synchronized_position;
            simulation_rotation = synchronized_rotation;
            
            inertialize_root_adjust(
                bone_offset_positions(0),
                transition_src_position,
                transition_src_rotation,
                transition_dst_position,
                transition_dst_rotation,
                bone_positions(0),
                bone_rotations(0),
                synchronized_position,
                synchronized_rotation);
        }
        
        // Adjustment 
        if (debug) std::cout << "test2" << std::endl;
        
        if (!synchronization_enabled && adjustment_enabled)
        {   
            vec3 adjusted_position = bone_positions(0);
            quat adjusted_rotation = bone_rotations(0);
            
            if (adjustment_by_velocity_enabled)
            {
                adjusted_position = adjust_character_position_by_velocity(
                    bone_positions(0),
                    bone_velocities(0),
                    simulation_position,
                    adjustment_position_max_ratio,
                    adjustment_position_halflife,
                    dt);
                
                adjusted_rotation = adjust_character_rotation_by_velocity(
                    bone_rotations(0),
                    bone_angular_velocities(0),
                    simulation_rotation,
                    adjustment_rotation_max_ratio,
                    adjustment_rotation_halflife,
                    dt);
            }
            else
            {
                adjusted_position = adjust_character_position(
                    bone_positions(0),
                    simulation_position,
                    adjustment_position_halflife,
                    dt);
                
                adjusted_rotation = adjust_character_rotation(
                    bone_rotations(0),
                    simulation_rotation,
                    adjustment_rotation_halflife,
                    dt);
            }
      
            inertialize_root_adjust(
                bone_offset_positions(0),
                transition_src_position,
                transition_src_rotation,
                transition_dst_position,
                transition_dst_rotation,
                bone_positions(0),
                bone_rotations(0),
                adjusted_position,
                adjusted_rotation);
        }
        
        // Clamping
        
        if (!synchronization_enabled && clamping_enabled)
        {
            vec3 adjusted_position = bone_positions(0);
            quat adjusted_rotation = bone_rotations(0);
            
            adjusted_position = clamp_character_position(
                adjusted_position,
                simulation_position,
                clamping_max_distance);
            
            adjusted_rotation = clamp_character_rotation(
                adjusted_rotation,
                simulation_rotation,
                clamping_max_angle);
            
            inertialize_root_adjust(
                bone_offset_positions(0),
                transition_src_position,
                transition_src_rotation,
                transition_dst_position,
                transition_dst_rotation,
                bone_positions(0),
                bone_rotations(0),
                adjusted_position,
                adjusted_rotation);
        }

        // Keep player simulation root above terrain floor each frame.
        // clamp_position_min_terrain_y(
        //     simulation_position,
        //     ground_plane_model,
        //     terrain_y_clamp_offset);
        
        // Contact fixup with foot locking and IK

        adjusted_bone_positions = bone_positions;
        adjusted_bone_rotations = bone_rotations;
        if (debug) std::cout << "test1" << std::endl;
        if (ik_enabled)
        {
            for (int i = 0; i < contact_bones.size; i++)
            {
                // Find all the relevant bone indices
                int toe_bone = contact_bones(i);
                int heel_bone = db.bone_parents(toe_bone);
                int knee_bone = db.bone_parents(heel_bone);
                int hip_bone = db.bone_parents(knee_bone);
                int root_bone = db.bone_parents(hip_bone);
                
                // Compute the world space position for the toe
                global_bone_computed.zero();
                
                forward_kinematics_partial(
                    global_bone_positions,
                    global_bone_rotations,
                    global_bone_computed,
                    bone_positions,
                    bone_rotations,
                    db.bone_parents,
                    toe_bone);
                
                // Raycast to find terrain height under this foot
                float terrain_height = 0.0f;
                Ray foot_ray = { to_Vector3(global_bone_positions(toe_bone) + vec3(0, 10, 0)), {0, -1, 0} };
                for (int mesh_idx = 0; mesh_idx < ground_plane_model.meshCount; mesh_idx++)
                {
                    RayCollision collision = GetRayCollisionMesh(foot_ray, ground_plane_model.meshes[mesh_idx], ground_plane_model.transform);
                    if (collision.hit && (terrain_height == 0.0f || collision.point.y > terrain_height))
                    {
                        terrain_height = collision.point.y;
                    }
                }
                float foot_target_height = terrain_height + ik_foot_height;
                
                // Update the contact state
                contact_update(
                    contact_states(i),
                    contact_locks(i),
                    contact_positions(i),  
                    contact_velocities(i),
                    contact_points(i),
                    contact_targets(i),
                    contact_offset_positions(i),
                    contact_offset_velocities(i),
                    global_bone_positions(toe_bone),
                    curr_bone_contacts(i),
                    ik_unlock_radius,
                    foot_target_height,
                    ik_blending_halflife,
                    dt);
                
                // Ensure contact position never goes through floor
                vec3 contact_position_clamp = contact_positions(i);
                contact_position_clamp.y = maxf(contact_position_clamp.y, foot_target_height);
                
                // Re-compute toe, heel, knee, hip, and root bone positions
                for (int bone : {heel_bone, knee_bone, hip_bone, root_bone})
                {
                    forward_kinematics_partial(
                        global_bone_positions,
                        global_bone_rotations,
                        global_bone_computed,
                        bone_positions,
                        bone_rotations,
                        db.bone_parents,
                        bone);
                }
                
                // Perform simple two-joint IK to place heel
                ik_two_bone(
                    adjusted_bone_rotations(hip_bone),
                    adjusted_bone_rotations(knee_bone),
                    global_bone_positions(hip_bone),
                    global_bone_positions(knee_bone),
                    global_bone_positions(heel_bone),
                    contact_position_clamp + (global_bone_positions(heel_bone) - global_bone_positions(toe_bone)),
                    quat_mul_vec3(global_bone_rotations(knee_bone), vec3(0.0f, 1.0f, 0.0f)),
                    global_bone_rotations(hip_bone),
                    global_bone_rotations(knee_bone),
                    global_bone_rotations(root_bone),
                    ik_max_length_buffer);
                
                // Re-compute toe, heel, and knee positions 
                global_bone_computed.zero();
                
                for (int bone : {toe_bone, heel_bone, knee_bone})
                {
                    forward_kinematics_partial(
                        global_bone_positions,
                        global_bone_rotations,
                        global_bone_computed,
                        adjusted_bone_positions,
                        adjusted_bone_rotations,
                        db.bone_parents,
                        bone);
                }
                
                // Rotate heel so toe is facing toward contact point
                ik_look_at(
                    adjusted_bone_rotations(heel_bone),
                    global_bone_rotations(knee_bone),
                    global_bone_rotations(heel_bone),
                    global_bone_positions(heel_bone),
                    global_bone_positions(toe_bone),
                    contact_position_clamp);
                
                // Re-compute toe and heel positions
                global_bone_computed.zero();
                
                for (int bone : {toe_bone, heel_bone})
                {
                    forward_kinematics_partial(
                        global_bone_positions,
                        global_bone_rotations,
                        global_bone_computed,
                        adjusted_bone_positions,
                        adjusted_bone_rotations,
                        db.bone_parents,
                        bone);
                }
                
                // Rotate toe bone so that the end of the toe 
                // does not intersect with the ground
                vec3 toe_end_curr = quat_mul_vec3(
                    global_bone_rotations(toe_bone), vec3(ik_toe_length, 0.0f, 0.0f)) + 
                    global_bone_positions(toe_bone);
                    
                vec3 toe_end_targ = toe_end_curr;
                toe_end_targ.y = maxf(toe_end_targ.y, ik_foot_height);
                
                ik_look_at(
                    adjusted_bone_rotations(toe_bone),
                    global_bone_rotations(heel_bone),
                    global_bone_rotations(toe_bone),
                    global_bone_positions(toe_bone),
                    toe_end_curr,
                    toe_end_targ);
            }
        }
        
        // Full pass of forward kinematics to compute 
        // all bone positions and rotations in the world
        // space ready for rendering
        
        forward_kinematics_full(
            global_bone_positions,
            global_bone_rotations,
            adjusted_bone_positions,
            adjusted_bone_rotations,
            db.bone_parents);

        // printf("root_y=%.6f\n", global_bone_positions(0).y);

        if (analysis_capture_enabled)
        {
            analysis_capture_bone_positions.push_back(global_bone_positions);
        }
        
        // Update camera
        
        orbit_camera_update(
            camera, 
            camera_azimuth,
            camera_altitude,
            camera_distance,
            bone_positions(0) + vec3(0, 0, 0),
            // simulation_position + vec3(0, 1, 0),
            gamepadstick_right,
            desired_strafe,
            dt);
        
        // Store playback visualization from the already inertialized runtime
        // pose. This keeps transitions smooth instead of jumping on search hits.
        if (joystick_playback_enabled)
        {
            bool lmm_runtime_enabled = lmm_enabled && lmm_networks_compatible;

            if (lmm_runtime_enabled)
            {
                playback_lmm_bone_positions.push_back(global_bone_positions);
                playback_lmm_bone_rotations.push_back(global_bone_rotations);

                // Keep MM history length aligned so indexing remains valid.
                // Use the current runtime pose instead of freezing at the first frame.
                playback_mm_bone_positions.push_back(global_bone_positions);
                playback_mm_bone_rotations.push_back(global_bone_rotations);
            }
            else
            {
                playback_mm_bone_positions.push_back(global_bone_positions);
                playback_mm_bone_rotations.push_back(global_bone_rotations);

                // Keep LMM history length aligned so indexing remains valid.
                // Use the current runtime pose instead of freezing at the first frame.
                playback_lmm_bone_positions.push_back(global_bone_positions);
                playback_lmm_bone_rotations.push_back(global_bone_rotations);
            }
        }

        if (mode == APP_MODE_WINDOW)
        {
            // Render
            
            // Calculate metrics
            if (debug) std::cout << "Collecting metrics" << std::endl;
            frame_time_ms = GetFrameTime() * 1000.0f;  // Convert to milliseconds
            fps_display = GetFPS();
            
            if (debug) std::cout << "Done collecting frame & fps" << std::endl;
        #if defined(_WIN32) || defined(PLATFORM_WEB)
            perf_sample_timer += dt;
            if (perf_sample_timer >= perf_sample_interval)
            {
        #if defined(_WIN32)
                runtime_metrics_update(perf_metrics);
        #elif defined(PLATFORM_WEB)
                runtime_metrics_update(perf_metrics, frame_time_ms);
        #endif
                perf_sample_timer = 0.0f;
            }
        #endif
            
            BeginDrawing();
            ClearBackground(RAYWHITE);
            
            BeginMode3D(camera);
        
        // Draw Simulation Object
        
        DrawCylinderWires(to_Vector3(simulation_position), 0.6f, 0.6f, 0.001f, 17, ORANGE);
        DrawSphereWires(to_Vector3(simulation_position), 0.05f, 4, 10, ORANGE);
        DrawLine3D(to_Vector3(simulation_position), to_Vector3(
            simulation_position + 0.6f * quat_mul_vec3(simulation_rotation, vec3(0.0f, 0.0f, 1.0f))), ORANGE);
        
        // Draw Clamping Radius/Angles
        
        if (clamping_enabled)
        {
            DrawCylinderWires(
                to_Vector3(simulation_position), 
                clamping_max_distance, 
                clamping_max_distance, 
                0.001f, 17, SKYBLUE);
            
            quat rotation_clamp_0 = quat_mul(quat_from_angle_axis(+clamping_max_angle, vec3(0.0f, 1.0f, 0.0f)), simulation_rotation);
            quat rotation_clamp_1 = quat_mul(quat_from_angle_axis(-clamping_max_angle, vec3(0.0f, 1.0f, 0.0f)), simulation_rotation);
            
            vec3 rotation_clamp_0_dir = simulation_position + 0.6f * quat_mul_vec3(rotation_clamp_0, vec3(0.0f, 0.0f, 1.0f));
            vec3 rotation_clamp_1_dir = simulation_position + 0.6f * quat_mul_vec3(rotation_clamp_1, vec3(0.0f, 0.0f, 1.0f));

            DrawLine3D(to_Vector3(simulation_position), to_Vector3(rotation_clamp_0_dir), SKYBLUE);
            DrawLine3D(to_Vector3(simulation_position), to_Vector3(rotation_clamp_1_dir), SKYBLUE);
        }
        
        // Draw IK foot lock positions
        
        if (ik_enabled)
        {
            for (int i = 0; i <  contact_positions.size; i++)
            {
                if (contact_locks(i))
                {
                    DrawSphereWires(to_Vector3(contact_positions(i)), 0.05f, 4, 10, PINK);
                }
            }
        }
        
        draw_trajectory(
            trajectory_positions,
            trajectory_rotations,
            ORANGE);
        
        deform_character_mesh(
            character_mesh, 
            character_data, 
            global_bone_positions, 
            global_bone_rotations,
            db.bone_parents);

        // During playback, render only MM/LMM selections to avoid a third
        // duplicate character from the default draw path.
        // Also hide the default character when comparing both MM search modes.
        bool render_playback_characters = joystick_playback_enabled || (!lmm_runtime_enabled && mm_history_mode == MM_HISTORY_SEARCH_BOTH);

        if (!render_playback_characters)
        {
            if (show_stickman)
            {
                draw_stickman(global_bone_positions, db.bone_parents, BROWN);
            }
            else
            {
                DrawModel(character_model, (Vector3){0.0f, 0.0f, 0.0f}, 1.0f, RAYWHITE);
            }
        }
        
        // Draw playback pose during recording playback using the
        // learned-motion-matching toggle as mode selector.
        if (joystick_playback_enabled && !playback_mm_bone_positions.empty())
        {
            int current_frame = std::min((int)playback_mm_bone_positions.size() - 1, joystick_playback_index - 1);
            bool playback_use_lmm = lmm_enabled;
            
            // Draw MM (Motion Matching) from database when LMM mode is off.
            if (!playback_use_lmm && current_frame >= 0 && current_frame < (int)playback_mm_bone_positions.size())
            {
                if (show_stickman)
                {
                    draw_stickman(playback_mm_bone_positions[current_frame], db.bone_parents, GREEN);
                }
                else
                {
                    // Draw MM model with green tint
                    deform_character_mesh(
                        character_mesh,
                        character_data,
                        playback_mm_bone_positions[current_frame],
                        playback_mm_bone_rotations[current_frame],
                        db.bone_parents);
                    DrawModel(character_model, (Vector3){0.0f, 0.0f, 0.0f}, 1.0f, GREEN);
                }
            }
            
            // Draw LMM (Learned Motion Matching) when LMM mode is on.
            if (playback_use_lmm && current_frame >= 0 && current_frame < (int)playback_lmm_bone_positions.size())
            {
                if (show_stickman)
                {
                    draw_stickman(playback_lmm_bone_positions[current_frame], db.bone_parents, RED);
                }
                else
                {
                    // Draw LMM model with red tint
                    deform_character_mesh(
                        character_mesh,
                        character_data,
                        playback_lmm_bone_positions[current_frame],
                        playback_lmm_bone_rotations[current_frame],
                        db.bone_parents);
                    DrawModel(character_model, (Vector3){0.0f, 0.0f, 0.0f}, 1.0f, RED);
                }
            }
        }
        
        // Draw matched features
        
        array1d<float> current_features = lmm_runtime_enabled ? slice1d<float>(features_curr) : db.features(frame_index);
        denormalize_features(current_features, db.features_offset, db.features_scale);        
        draw_features(current_features, global_bone_positions(0), global_bone_rotations(0), MAROON,
            future_toe_position, future_terrain_heights, global_bone_positions(Bone_Hips), global_bone_positions, contact_bones,
            root_history_positions, root_history_rotations,
            history_left_foot_positions, history_right_foot_positions,
            history_left_foot_velocities, history_right_foot_velocities,
            history_hip_positions, history_hip_velocities,
            history_terrain_heights);

        if (!lmm_runtime_enabled && mm_history_mode == MM_HISTORY_SEARCH_BOTH)
        {
            // Compare both MM search modes directly in scene using stickman overlays.
            array1d<vec3> mm_with_history_positions(db.nbones());
            array1d<quat> mm_with_history_rotations(db.nbones());
            array1d<vec3> mm_without_history_positions(db.nbones());
            array1d<quat> mm_without_history_rotations(db.nbones());

            forward_kinematics_full(
                mm_with_history_positions,
                mm_with_history_rotations,
                bone_positions_with_history,
                bone_rotations_with_history,
                db.bone_parents);

            forward_kinematics_full(
                mm_without_history_positions,
                mm_without_history_rotations,
                bone_positions_without_history,
                bone_rotations_without_history,
                db.bone_parents);

            auto rebase_pose_to_runtime_root = [&](array1d<vec3>& pose_positions, const array1d<quat>& pose_rotations)
            {
                if (pose_positions.size <= 0)
                {
                    return;
                }

                vec3 src_root_pos = pose_positions(0);
                quat src_root_rot = pose_rotations(0);
                vec3 dst_root_pos = global_bone_positions(0);
                quat dst_root_rot = global_bone_rotations(0);

                for (int bi = 0; bi < pose_positions.size; bi++)
                {
                    vec3 local_from_src_root = quat_inv_mul_vec3(src_root_rot, pose_positions(bi) - src_root_pos);
                    pose_positions(bi) = quat_mul_vec3(dst_root_rot, local_from_src_root) + dst_root_pos;
                }
            };

            rebase_pose_to_runtime_root(mm_with_history_positions, mm_with_history_rotations);
            rebase_pose_to_runtime_root(mm_without_history_positions, mm_without_history_rotations);

            draw_stickman(mm_without_history_positions, db.bone_parents, SKYBLUE);
            draw_stickman(mm_with_history_positions, db.bone_parents, GREEN);
        }
        
        // Draw Simuation Bone
        
        DrawSphereWires(to_Vector3(bone_positions(0)), 0.05f, 4, 10, MAROON);
        DrawLine3D(to_Vector3(bone_positions(0)), to_Vector3(
            bone_positions(0) + 0.6f * quat_mul_vec3(bone_rotations(0), vec3(0.0f, 0.0f, 1.0f))), MAROON);
        
        // Draw Ground Plane

        // Visual reference cube for scene scale (1.0f tall).
        DrawCube((Vector3){2.0f, 0.5f, 0.0f}, 1.0f, 1.0f, 1.0f, GRAY);
        DrawCubeWires((Vector3){2.0f, 0.5f, 0.0f}, 1.0f, 1.0f, 1.0f, DARKGRAY);
        
        DrawModel(ground_plane_model, (Vector3){0.0f, -0.01f, 0.0f}, 1.0f, WHITE);
        DrawGrid(20, 1.0f);
        draw_axis(vec3(), quat());
        
        EndMode3D();

        // UI
        
        // Responsive positioning for right-side panels
        float ui_right_panel_x = screen_width - 320;  // 290px width + 20px margin
        float ui_right_panel_sm_x = screen_width - 270;  // 250px width + 20px margin
        
        //---------
        // Performance Metrics Panel
        
        float ui_metrics_hei = 20;
        float ui_metrics_wid = 300;
        float ui_metrics_hgt = 130;
        
        GuiGroupBox((Rectangle){ 490, ui_metrics_hei, ui_metrics_wid, ui_metrics_hgt }, "Performance metrics");
        
        // Frame time display
        GuiLabel((Rectangle){ 510, ui_metrics_hei + 15, 260, 20 },
            TextFormat("Frame Time:  %6.2f ms", frame_time_ms));
        
        // FPS display
        GuiLabel((Rectangle){ 510, ui_metrics_hei + 35, 260, 20 },
            TextFormat("FPS:         %6d fps", (int)fps_display));

#if defined(_WIN32)
        GuiLabel((Rectangle){ 510, ui_metrics_hei + 55, 260, 20 },
            TextFormat("CPU:         %6.2f %%", perf_metrics.cpu_percent));

        if (perf_metrics.gpu_percent >= 0.0f)
        {
            GuiLabel((Rectangle){ 510, ui_metrics_hei + 75, 260, 20 },
                TextFormat("GPU:         %6.2f %%", perf_metrics.gpu_percent));
        }
        else
        {
            GuiLabel((Rectangle){ 510, ui_metrics_hei + 75, 260, 20 },
                "GPU:         N/A");
        }

        GuiLabel((Rectangle){ 510, ui_metrics_hei + 95, 260, 20 },
            TextFormat("Memory:      %6.1f MB  (%5.1f%% sys)", perf_metrics.process_memory_mb, perf_metrics.system_memory_percent));
#elif defined(PLATFORM_WEB)
        GuiLabel((Rectangle){ 510, ui_metrics_hei + 55, 260, 20 },
            TextFormat("CPU(est):    %6.2f %%", perf_metrics.cpu_percent));

        GuiLabel((Rectangle){ 510, ui_metrics_hei + 75, 260, 20 },
            "GPU:         N/A");

        if (perf_metrics.system_memory_percent >= 0.0f)
        {
            GuiLabel((Rectangle){ 510, ui_metrics_hei + 95, 260, 20 },
                TextFormat("WASM Mem:    %6.1f MB  (JS %5.1f%%)", perf_metrics.process_memory_mb, perf_metrics.system_memory_percent));
        }
        else
        {
            GuiLabel((Rectangle){ 510, ui_metrics_hei + 95, 260, 20 },
                TextFormat("WASM Mem:    %6.1f MB", perf_metrics.process_memory_mb));
        }
#else
        GuiLabel((Rectangle){ 510, ui_metrics_hei + 55, 260, 20 },
            "CPU/GPU/Memory metrics are only implemented on Windows");
#endif
        
        //---------
        
        float ui_sim_hei = 20;
        
        GuiGroupBox((Rectangle){ ui_right_panel_x, ui_sim_hei, 290, 250 }, "simulation object");

        GuiSliderBar(
            (Rectangle){ ui_right_panel_x + 130, ui_sim_hei + 10, 120, 20 }, 
            "velocity halflife", 
            TextFormat("%5.3f", simulation_velocity_halflife), 
            &simulation_velocity_halflife, 0.0f, 0.5f);
            
        GuiSliderBar(
            (Rectangle){ ui_right_panel_x + 130, ui_sim_hei + 40, 120, 20 }, 
            "rotation halflife", 
            TextFormat("%5.3f", simulation_rotation_halflife), 
            &simulation_rotation_halflife, 0.0f, 0.5f);
            
        GuiSliderBar(
            (Rectangle){ ui_right_panel_x + 130, ui_sim_hei + 70, 120, 20 }, 
            "run forward speed", 
            TextFormat("%5.3f", simulation_run_fwrd_speed), 
            &simulation_run_fwrd_speed, 0.0f, 10.0f);
        
        GuiSliderBar(
            (Rectangle){ ui_right_panel_x + 130, ui_sim_hei + 100, 120, 20 }, 
            "run sideways speed", 
            TextFormat("%5.3f", simulation_run_side_speed), 
            &simulation_run_side_speed, 0.0f, 10.0f);
        
        GuiSliderBar(
            (Rectangle){ ui_right_panel_x + 130, ui_sim_hei + 130, 120, 20 }, 
            "run backwards speed", 
            TextFormat("%5.3f", simulation_run_back_speed), 
            &simulation_run_back_speed, 0.0f, 10.0f);
        
        GuiSliderBar(
            (Rectangle){ ui_right_panel_x + 130, ui_sim_hei + 160, 120, 20 }, 
            "walk forward speed", 
            TextFormat("%5.3f", simulation_walk_fwrd_speed), 
            &simulation_walk_fwrd_speed, 0.0f, 5.0f);
        
        GuiSliderBar(
            (Rectangle){ ui_right_panel_x + 130, ui_sim_hei + 190, 120, 20 }, 
            "walk sideways speed", 
            TextFormat("%5.3f", simulation_walk_side_speed), 
            &simulation_walk_side_speed, 0.0f, 5.0f);
        
        GuiSliderBar(
            (Rectangle){ ui_right_panel_x + 130, ui_sim_hei + 220, 120, 20 }, 
            "walk backwards speed", 
            TextFormat("%5.3f", simulation_walk_back_speed), 
            &simulation_walk_back_speed, 0.0f, 5.0f);
        
        //---------
        
        float ui_inert_hei = 280;
        
        GuiGroupBox((Rectangle){ ui_right_panel_x, ui_inert_hei, 290, 40 }, "inertiaization blending");
        
        GuiSliderBar(
            (Rectangle){ ui_right_panel_x + 130, ui_inert_hei + 10, 120, 20 }, 
            "halflife", 
            TextFormat("%5.3f", inertialize_blending_halflife), 
            &inertialize_blending_halflife, 0.0f, 0.3f);
        
        //---------
        
        float ui_visual_hei = 330;
        float ui_visual_height = joystick_playback_enabled ? 100.0f : 40.0f;
        
        GuiGroupBox((Rectangle){ ui_right_panel_x, ui_visual_hei, 290, ui_visual_height }, "visualization");
        
        GuiCheckBox(
            (Rectangle){ ui_right_panel_x + 30, ui_visual_hei + 10, 20, 20 }, 
            "stickman",
            &show_stickman);
        
        //---------
        
        float ui_lmm_hei = 380;
        
        GuiGroupBox((Rectangle){ ui_right_panel_x, ui_lmm_hei, 290, 40 }, "learned motion matching");
        
        GuiCheckBox(
            (Rectangle){ ui_right_panel_x + 30, ui_lmm_hei + 10, 20, 20 }, 
            "enabled",
            &lmm_enabled);
        
        //---------
        
        float ui_ctrl_hei = 430;
        
        GuiGroupBox((Rectangle){ ui_right_panel_sm_x, ui_ctrl_hei, 250, 190 }, "controls");
        
        GuiLabel((Rectangle){ ui_right_panel_sm_x + 20, ui_ctrl_hei +  10, 145, 20 }, "Move: Left Stick or WASD");
        GuiLabel((Rectangle){ ui_right_panel_sm_x + 20, ui_ctrl_hei +  30, 145, 20 }, "Camera/Facing: Right Stick");
        GuiLabel((Rectangle){ ui_right_panel_sm_x + 20, ui_ctrl_hei +  50, 145, 20 }, "Strafe: Left Trigger or H");
        GuiLabel((Rectangle){ ui_right_panel_sm_x + 20, ui_ctrl_hei +  70, 145, 20 }, "Walk: A Button or J");
        GuiLabel((Rectangle){ ui_right_panel_sm_x + 20, ui_ctrl_hei +  90, 145, 20 }, "Crouch: Y Button/K");
        GuiLabel((Rectangle){ ui_right_panel_sm_x + 20, ui_ctrl_hei + 110, 145, 20 }, "Cartwheel: X Button/L");
        GuiLabel((Rectangle){ ui_right_panel_sm_x + 20, ui_ctrl_hei + 130, 145, 20 }, "Zoom In: Left Shoulder/E");
        GuiLabel((Rectangle){ ui_right_panel_sm_x + 20, ui_ctrl_hei + 150, 145, 20 }, "Zoom Out: Right Shoulder/Q");
        GuiLabel((Rectangle){ ui_right_panel_sm_x + 20, ui_ctrl_hei + 170, 145, 20 }, "Pad + keyboard can mix");

        const int flag_x = ui_right_panel_sm_x + 168;
        GuiLabel((Rectangle){ (float)flag_x, ui_ctrl_hei + 14, 70, 16 }, "Gait Flags");

        auto draw_flag_chip = [&](int y, const char* label, bool on, Color on_color)
        {
            const Color fill = on ? on_color : Fade(LIGHTGRAY, 0.75f);
            const Color border = on ? DARKGRAY : GRAY;
            DrawRectangle(flag_x, y, 66, 20, fill);
            DrawRectangleLines(flag_x, y, 66, 20, border);
            DrawCircle(flag_x + 8, y + 10, 4.0f, on ? GREEN : GRAY);
            GuiLabel((Rectangle){ (float)flag_x + 14, (float)y + 2, 34, 16 }, label);
            GuiLabel((Rectangle){ (float)flag_x + 47, (float)y + 2, 18, 16 }, on ? "ON" : "OFF");
        };

        draw_flag_chip(ui_ctrl_hei + 40, "CR", desired_crouch, ORANGE);
        draw_flag_chip(ui_ctrl_hei + 66, "ID", desired_idle, SKYBLUE);
        draw_flag_chip(ui_ctrl_hei + 92, "JP", desired_jump, RED);
        draw_flag_chip(ui_ctrl_hei + 118, "CW", desired_cartwheel, GOLD);
        

        //---------

        float ui_record_hei = 640;
        GuiGroupBox((Rectangle){ ui_right_panel_sm_x, ui_record_hei, 250, 255 }, "joystick recording");

        const char* recording_button_label = joystick_recording_enabled ? "stop + save" : "start recording";
        if (GuiButton((Rectangle){ ui_right_panel_sm_x + 20, ui_record_hei + 15, 100, 26 }, recording_button_label))
        {
            if (!joystick_recording_enabled)
            {
                joystick_playback_enabled = false;
                joystick_recording_enabled = true;
                joystick_recording_samples.clear();
                joystick_recording_frame = 0;
                joystick_recording_time = 0.0f;
                std::string output_path = joystick_recording_make_output_path(joystick_recording_folder);
                snprintf(joystick_recording_output_file, sizeof(joystick_recording_output_file), "%s", output_path.c_str());
                reset_motion_to_recording_start();
            }
            else
            {
                joystick_recording_enabled = false;
                joystick_recording_last_saved_count = (int)joystick_recording_samples.size();
                joystick_recording_last_save_ok = save_joystick_recording_csv(
                    joystick_recording_output_file,
                    joystick_recording_samples);

                if (joystick_recording_last_save_ok)
                {
                    snprintf(joystick_recording_last_saved_file, sizeof(joystick_recording_last_saved_file), "%s", joystick_recording_output_file);
                    joystick_recording_refresh_csv_files(joystick_recording_folder, joystick_recording_csv_files);
                    joystick_recording_build_dropdown_text(
                        joystick_recording_csv_files,
                        joystick_recording_csv_dropdown_text,
                        sizeof(joystick_recording_csv_dropdown_text));
                }
            }
        }

        if (GuiButton((Rectangle){ ui_right_panel_sm_x + 20, ui_record_hei + 47, 100, 24 }, "refresh"))
        {
            joystick_recording_refresh_csv_files(joystick_recording_folder, joystick_recording_csv_files);
            if (joystick_recording_csv_selected_index >= (int)joystick_recording_csv_files.size())
            {
                joystick_recording_csv_selected_index = (int)joystick_recording_csv_files.size() - 1;
            }
            if (joystick_recording_csv_selected_index < 0)
            {
                joystick_recording_csv_selected_index = 0;
            }
            joystick_recording_build_dropdown_text(
                joystick_recording_csv_files,
                joystick_recording_csv_dropdown_text,
                sizeof(joystick_recording_csv_dropdown_text));
        }

        const char* playback_button_label = joystick_playback_enabled ? "stop playback" : "load + play selected";
        if (GuiButton((Rectangle){ ui_right_panel_sm_x + 20, ui_record_hei + 103, 210, 24 }, playback_button_label))
        {
            if (joystick_playback_enabled)
            {
                joystick_playback_enabled = false;
            }
            else if (!joystick_recording_csv_files.empty())
            {
                std::string selected_path = std::string(joystick_recording_folder) + "/" + joystick_recording_csv_files[joystick_recording_csv_selected_index];
                joystick_playback_last_load_ok = load_joystick_recording_csv(
                    selected_path.c_str(),
                    joystick_playback_samples);
                joystick_playback_last_loaded_count = (int)joystick_playback_samples.size();

                if (joystick_playback_last_load_ok)
                {
                    snprintf(joystick_recording_loaded_file, sizeof(joystick_recording_loaded_file), "%s", selected_path.c_str());
                    joystick_recording_enabled = false;
                    joystick_playback_enabled = true;
                    joystick_playback_index = 0;
                    reset_motion_to_recording_start();
                }
                else
                {
                    joystick_playback_enabled = false;
                    joystick_playback_index = 0;
                }
            }
            else
            {
                joystick_playback_last_load_ok = false;
                joystick_playback_last_loaded_count = 0;
                joystick_playback_enabled = false;
                joystick_playback_index = 0;
            }
        }

        GuiLabel(
            (Rectangle){ ui_right_panel_sm_x + 20, ui_record_hei + 129, 210, 16 },
            joystick_recording_enabled ? "Status: recording" : (joystick_playback_enabled ? "Status: playing" : "Status: idle"));
        GuiLabel(
            (Rectangle){ ui_right_panel_sm_x + 20, ui_record_hei + 143, 210, 16 },
            TextFormat("Samples: %d", (int)joystick_recording_samples.size()));
        GuiLabel(
            (Rectangle){ ui_right_panel_sm_x + 20, ui_record_hei + 157, 210, 16 },
            TextFormat("Last save: %s (%d)", joystick_recording_last_save_ok ? "ok" : "failed", joystick_recording_last_saved_count));
        GuiLabel(
            (Rectangle){ ui_right_panel_sm_x + 20, ui_record_hei + 171, 210, 16 },
            TextFormat("Load: %s (%d)", joystick_playback_last_load_ok ? "ok" : "failed", joystick_playback_last_loaded_count));
        int active_range_index = -1;
        int range_min = 0;
        int range_max = db.nframes() > 0 ? db.nframes() - 1 : 0;
        for (int ri = 0; ri < db.nranges(); ri++)
        {
            if (frame_index >= db.range_starts(ri) && frame_index < db.range_stops(ri))
            {
                active_range_index = ri;
                range_min = db.range_starts(ri);
                range_max = db.range_stops(ri) - 1;
                break;
            }
        }

        const char* bvh_name_display = "n/a";
        int source_start_display = 0;
        int source_stop_display = 0;
        for (size_t mi = 0; mi < range_metadata_entries.size(); mi++)
        {
            if (range_metadata_entries[mi].range_index == active_range_index)
            {
                bvh_name_display = range_metadata_entries[mi].bvh_name;
                source_start_display = range_metadata_entries[mi].source_start;
                source_stop_display = range_metadata_entries[mi].source_stop;
                break;
            }
        }

        int per_file_frame_index = 0;
        if (source_stop_display > source_start_display && range_max > range_min)
        {
            float frame_t = (float)(frame_index - range_min) / (float)(range_max - range_min);
            frame_t = clampf(frame_t, 0.0f, 1.0f);
            float source_span = (float)(source_stop_display - source_start_display - 1);
            per_file_frame_index = source_start_display + (int)roundf(frame_t * source_span);
        }
        else
        {
            per_file_frame_index = source_start_display;
        }

        GuiLabel(
            (Rectangle){ ui_right_panel_sm_x + 20, ui_record_hei + 185, 210, 16 },
            TextFormat("BVH: %s", bvh_name_display));
        GuiLabel(
            (Rectangle){ ui_right_panel_sm_x + 20, ui_record_hei + 199, 210, 16 },
            TextFormat("Source Range: %d - %d", source_start_display, source_stop_display));
        int playback_frame_curr = joystick_playback_index > 0 ? joystick_playback_index - 1 : 0;
        int playback_frame_max = joystick_playback_last_loaded_count > 0 ? joystick_playback_last_loaded_count - 1 : 0;
        if (joystick_playback_enabled)
        {
            GuiLabel(
                (Rectangle){ ui_right_panel_sm_x + 20, ui_record_hei + 213, 210, 16 },
                TextFormat("Frame G/P: %d / %d", playback_frame_curr, playback_frame_curr));
            GuiLabel(
                (Rectangle){ ui_right_panel_sm_x + 20, ui_record_hei + 227, 210, 16 },
                TextFormat("Range: %d - %d", 0, playback_frame_max));
        }
        else
        {
            GuiLabel(
                (Rectangle){ ui_right_panel_sm_x + 20, ui_record_hei + 213, 210, 16 },
                TextFormat("Frame G/P: %d / %d", frame_index, per_file_frame_index));
            GuiLabel(
                (Rectangle){ ui_right_panel_sm_x + 20, ui_record_hei + 227, 210, 16 },
                TextFormat("Range: %d - %d", range_min, range_max));
        }

        //---------
        
        float ui_input_x = 20;
        float ui_input_hei = 700;
        GuiGroupBox((Rectangle){ ui_input_x, ui_input_hei, 250, 115 }, "Gamepad Test");
        
        int center_x = (int)ui_input_x + 125;
        int start_y = ui_input_hei + 30;

        // Shoulder Buttons
        // L2
        DrawRectangle(center_x - 80, start_y - 20, 30, 10, IsGamepadButtonDown(GAMEPAD_PLAYER, GAMEPAD_BUTTON_LEFT_TRIGGER_2) ? RED : LIGHTGRAY);
        DrawRectangleLines(center_x - 80, start_y - 20, 30, 10, BLACK);
        // L1
        DrawRectangle(center_x - 80, start_y - 5, 30, 10, IsGamepadButtonDown(GAMEPAD_PLAYER, GAMEPAD_BUTTON_LEFT_TRIGGER_1) ? RED : LIGHTGRAY);
        DrawRectangleLines(center_x - 80, start_y - 5, 30, 10, BLACK);
        
        // R2
        DrawRectangle(center_x + 50, start_y - 20, 30, 10, IsGamepadButtonDown(GAMEPAD_PLAYER, GAMEPAD_BUTTON_RIGHT_TRIGGER_2) ? RED : LIGHTGRAY);
        DrawRectangleLines(center_x + 50, start_y - 20, 30, 10, BLACK);
        // R1
        DrawRectangle(center_x + 50, start_y - 5, 30, 10, IsGamepadButtonDown(GAMEPAD_PLAYER, GAMEPAD_BUTTON_RIGHT_TRIGGER_1) ? RED : LIGHTGRAY);
        DrawRectangleLines(center_x + 50, start_y - 5, 30, 10, BLACK);

        // Sticks
        // Left Stick
        int ls_x = center_x - 50;
        int ls_y = start_y + 60;
        DrawCircleLines(ls_x, ls_y, 20, BLACK);
        if (IsGamepadButtonDown(GAMEPAD_PLAYER, GAMEPAD_BUTTON_LEFT_THUMB)) DrawCircle(ls_x, ls_y, 20, Fade(RED, 0.3f));
        DrawCircle(ls_x + (int)(gamepadstick_left.x * 20), ls_y - (int)(gamepadstick_left.z * 20), 4, RED);
        
        // Right Stick
        int rs_x = center_x + 50;
        int rs_y = start_y + 60;
        DrawCircleLines(rs_x, rs_y, 20, BLACK);
        if (IsGamepadButtonDown(GAMEPAD_PLAYER, GAMEPAD_BUTTON_RIGHT_THUMB)) DrawCircle(rs_x, rs_y, 20, Fade(BLUE, 0.3f));
        DrawCircle(rs_x + (int)(gamepadstick_right.x * 20), rs_y - (int)(gamepadstick_right.z * 20), 4, BLUE);

        // D-Pad
        int dp_x = center_x - 90;
        int dp_y = start_y + 30;
        int dp_size = 10;
        DrawRectangle(dp_x, dp_y - dp_size, dp_size, dp_size, IsGamepadButtonDown(GAMEPAD_PLAYER, GAMEPAD_BUTTON_LEFT_FACE_UP) ? GRAY : LIGHTGRAY);
        DrawRectangleLines(dp_x, dp_y - dp_size, dp_size, dp_size, BLACK);
        DrawRectangle(dp_x, dp_y + dp_size, dp_size, dp_size, IsGamepadButtonDown(GAMEPAD_PLAYER, GAMEPAD_BUTTON_LEFT_FACE_DOWN) ? GRAY : LIGHTGRAY);
        DrawRectangleLines(dp_x, dp_y + dp_size, dp_size, dp_size, BLACK);
        DrawRectangle(dp_x - dp_size, dp_y, dp_size, dp_size, IsGamepadButtonDown(GAMEPAD_PLAYER, GAMEPAD_BUTTON_LEFT_FACE_LEFT) ? GRAY : LIGHTGRAY);
        DrawRectangleLines(dp_x - dp_size, dp_y, dp_size, dp_size, BLACK);
        DrawRectangle(dp_x + dp_size, dp_y, dp_size, dp_size, IsGamepadButtonDown(GAMEPAD_PLAYER, GAMEPAD_BUTTON_LEFT_FACE_RIGHT) ? GRAY : LIGHTGRAY);
        DrawRectangleLines(dp_x + dp_size, dp_y, dp_size, dp_size, BLACK);

        // Face Buttons
        int fb_x = center_x + 90;
        int fb_y = start_y + 30;
        int fb_rad = 6;
        DrawCircle(fb_x, fb_y - 12, fb_rad, IsGamepadButtonDown(GAMEPAD_PLAYER, GAMEPAD_BUTTON_RIGHT_FACE_UP) ? YELLOW : LIGHTGRAY);
        DrawCircleLines(fb_x, fb_y - 12, fb_rad, BLACK);
        DrawCircle(fb_x, fb_y + 12, fb_rad, IsGamepadButtonDown(GAMEPAD_PLAYER, GAMEPAD_BUTTON_RIGHT_FACE_DOWN) ? GREEN : LIGHTGRAY);
        DrawCircleLines(fb_x, fb_y + 12, fb_rad, BLACK);
        DrawCircle(fb_x - 12, fb_y, fb_rad, IsGamepadButtonDown(GAMEPAD_PLAYER, GAMEPAD_BUTTON_RIGHT_FACE_LEFT) ? BLUE : LIGHTGRAY);
        DrawCircleLines(fb_x - 12, fb_y, fb_rad, BLACK);
        DrawCircle(fb_x + 12, fb_y, fb_rad, IsGamepadButtonDown(GAMEPAD_PLAYER, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT) ? RED : LIGHTGRAY);
        DrawCircleLines(fb_x + 12, fb_y, fb_rad, BLACK);

        
        //---------
        
        GuiGroupBox((Rectangle){ 20, 20, 290, 220 }, "feature weights");
        
        GuiSliderBar(
            (Rectangle){ 150, 30, 120, 20 }, 
            "foot position", 
            TextFormat("%5.3f", feature_weight_foot_position), 
            &feature_weight_foot_position, 0.001f, 3.0f);
            
        GuiSliderBar(
            (Rectangle){ 150, 60, 120, 20 }, 
            "foot velocity", 
            TextFormat("%5.3f", feature_weight_foot_velocity), 
            &feature_weight_foot_velocity, 0.001f, 3.0f);
        
        GuiSliderBar(
            (Rectangle){ 150, 90, 120, 20 }, 
            "hip velocity", 
            TextFormat("%5.3f", feature_weight_hip_velocity), 
            &feature_weight_hip_velocity, 0.001f, 3.0f);
        
        GuiSliderBar(
            (Rectangle){ 150, 120, 120, 20 }, 
            "trajectory positions", 
            TextFormat("%5.3f", feature_weight_trajectory_positions), 
            &feature_weight_trajectory_positions, 0.001f, 3.0f);
        
        GuiSliderBar(
            (Rectangle){ 150, 150, 120, 20 }, 
            "trajectory directions", 
            TextFormat("%5.3f", feature_weight_trajectory_directions), 
            &feature_weight_trajectory_directions, 0.001f, 3.0f);
        
        GuiSliderBar(
            (Rectangle){ 150, 180, 120, 20 }, 
            "terrain heights", 
            TextFormat("%5.3f", feature_weight_terrain_heights), 
            &feature_weight_terrain_heights, 0.001f, 3.0f);

        if (GuiDropdownBox(
            (Rectangle){ 30, 210, 110, 20 },
            "mm hist off;mm hist on;mm hist both",
            &mm_history_mode,
            mm_history_mode_dropdown_edit))
        {
            mm_history_mode_dropdown_edit = !mm_history_mode_dropdown_edit;
        }

        if (mm_history_mode == MM_HISTORY_SEARCH_BOTH)
        {
            GuiLabel((Rectangle){ 30, 233, 260, 16 }, "Both mode: GREEN=history on, SKYBLUE=history off");
        }
            
        if (GuiButton((Rectangle){ 150, 210, 120, 20 }, "rebuild database"))
        {
            database_build_matching_features(
                db,
                feature_weight_foot_position,
                feature_weight_foot_velocity,
                feature_weight_hip_velocity,
                feature_weight_trajectory_positions,
                feature_weight_trajectory_directions,
                feature_weight_terrain_heights,
                feature_weight_idle,
                feature_weight_crouch,
                feature_weight_jump,
                feature_weight_cartwheel,
                feature_weight_history_foot_position,
                feature_weight_history_foot_velocity,
                feature_weight_history_hip_velocity,
                feature_weight_history_trajectory_positions,
                feature_weight_history_trajectory_directions,
                feature_weight_history_terrain_heights);
        }
        
        //---------
        
        float ui_sync_hei = 250;
        
        GuiGroupBox((Rectangle){ 20, ui_sync_hei, 290, 70 }, "synchronization");

        GuiCheckBox(
            (Rectangle){ 50, ui_sync_hei + 10, 20, 20 }, 
            "enabled",
            &synchronization_enabled);

        GuiSliderBar(
            (Rectangle){ 150, ui_sync_hei + 40, 120, 20 }, 
            "data-driven amount", 
            TextFormat("%5.3f", synchronization_data_factor), 
            &synchronization_data_factor, 0.0f, 1.0f);

        //---------
        
        float ui_adj_hei = 330;
        
        GuiGroupBox((Rectangle){ 20, ui_adj_hei, 290, 130 }, "adjustment");
        
        GuiCheckBox(
            (Rectangle){ 50, ui_adj_hei + 10, 20, 20 }, 
            "enabled",
            &adjustment_enabled);    
        
        GuiCheckBox(
            (Rectangle){ 50, ui_adj_hei + 40, 20, 20 }, 
            "clamp to max velocity",
            &adjustment_by_velocity_enabled);    
        
        GuiSliderBar(
            (Rectangle){ 150, ui_adj_hei + 70, 120, 20 }, 
            "position halflife", 
            TextFormat("%5.3f", adjustment_position_halflife), 
            &adjustment_position_halflife, 0.0f, 0.5f);
        
        GuiSliderBar(
            (Rectangle){ 150, ui_adj_hei + 100, 120, 20 }, 
            "rotation halflife", 
            TextFormat("%5.3f", adjustment_rotation_halflife), 
            &adjustment_rotation_halflife, 0.0f, 0.5f);
        
        //---------
        
        float ui_clamp_hei = 470;
        
        GuiGroupBox((Rectangle){ 20, ui_clamp_hei, 290, 100 }, "clamping");
        
        GuiCheckBox(
            (Rectangle){ 50, ui_clamp_hei + 10, 20, 20 }, 
            "enabled",
            &clamping_enabled);      
        
        GuiSliderBar(
            (Rectangle){ 150, ui_clamp_hei + 40, 120, 20 }, 
            "distance", 
            TextFormat("%5.3f", clamping_max_distance), 
            &clamping_max_distance, 0.0f, 0.5f);
        
        GuiSliderBar(
            (Rectangle){ 150, ui_clamp_hei + 70, 120, 20 }, 
            "angle", 
            TextFormat("%5.3f", clamping_max_angle), 
            &clamping_max_angle, 0.0f, PIf);
        
        //---------
        
        float ui_ik_hei = 580;
        
        GuiGroupBox((Rectangle){ 20, ui_ik_hei, 290, 100 }, "inverse kinematics");
        
        bool ik_enabled_prev = ik_enabled;
        
        GuiCheckBox(
            (Rectangle){ 50, ui_ik_hei + 10, 20, 20 }, 
            "enabled",
            &ik_enabled);      
        
        // Foot locking needs resetting when IK is toggled
        if (ik_enabled && !ik_enabled_prev)
        {
            for (int i = 0; i < contact_bones.size; i++)
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
                    bone_positions,
                    bone_velocities,
                    bone_rotations,
                    bone_angular_velocities,
                    db.bone_parents,
                    contact_bones(i));
                
                contact_reset(
                    contact_states(i),
                    contact_locks(i),
                    contact_positions(i),  
                    contact_velocities(i),
                    contact_points(i),
                    contact_targets(i),
                    contact_offset_positions(i),
                    contact_offset_velocities(i),
                    bone_position,
                    bone_velocity,
                    false);
            }
        }
        
        GuiSliderBar(
            (Rectangle){ 150, ui_ik_hei + 40, 120, 20 }, 
            "blending halflife", 
            TextFormat("%5.3f", ik_blending_halflife), 
            &ik_blending_halflife, 0.0f, 1.0f);
        
        GuiSliderBar(
            (Rectangle){ 150, ui_ik_hei + 70, 120, 20 }, 
            "unlock radius", 
            TextFormat("%5.3f", ik_unlock_radius), 
            &ik_unlock_radius, 0.0f, 0.5f);

        // Draw this last so expanded dropdown options stay above other GUI panels.
        if (GuiDropdownBox(
            (Rectangle){ ui_right_panel_sm_x + 20, ui_record_hei + 75, 210, 24 },
            joystick_recording_csv_dropdown_text,
            &joystick_recording_csv_selected_index,
            joystick_recording_csv_dropdown_edit))
        {
            joystick_recording_csv_dropdown_edit = !joystick_recording_csv_dropdown_edit;
        }
        
        //---------

            EndDrawing();
        }

    };

    // Initialize simulation/trajectory state to the configured spawn pose.
    reset_motion_to_recording_start();

#if defined(PLATFORM_WEB)
    std::function<void()> u{update_func};
    emscripten_set_main_loop_arg(update_callback, &u, 0, 1);
#else
    if (mode != APP_MODE_WINDOW)
    {
        std::vector<std::string> analysis_files;
        if (analyze_input_is_file)
        {
            if (FileExists(analyze_input_path))
            {
                analysis_files.push_back(GetFileName(analyze_input_path));
            }
        }
        else
        {
            joystick_recording_refresh_csv_files(analyze_input_path, analysis_files);
        }

        if (analysis_files.empty())
        {
            if (analyze_input_is_file)
            {
                std::cout << "Analyze: csv file not found at " << analyze_input_path << std::endl;
            }
            else
            {
                std::cout << "Analyze: no csv files found in " << analyze_input_path << std::endl;
            }
        }

        struct analyze_result
        {
            std::string file;
            int frame_count = 0;
            int joint_count = 0;
            double mpjpe = -1.0;
            double mm_mpjpe = -1.0;
            double lmm_mpjpe = -1.0;
            double mm_time_ms = -1.0;
            double lmm_time_ms = -1.0;
            float mm_mem_delta_mb = -1.0f;
            float lmm_mem_delta_mb = -1.0f;
            float mm_mem_peak_mb = -1.0f;
            float lmm_mem_peak_mb = -1.0f;
            float mm_mem_avg_mb = -1.0f;
            float lmm_mem_avg_mb = -1.0f;
            bool ok = false;
            std::string note;
        };

        struct capture_stats
        {
            double elapsed_ms = -1.0;
            float mem_delta_mb = -1.0f;
            float mem_peak_mb = -1.0f;
            float mem_avg_mb = -1.0f;
        };

        auto run_capture_for_mode = [&](const std::vector<joystick_record_sample>& samples,
                                        bool use_lmm,
                                        std::vector<array1d<vec3>>& output,
                                        capture_stats& stats) -> bool
        {
            reset_runtime_for_analysis();
            lmm_enabled = use_lmm;
            joystick_playback_samples = samples;
            joystick_playback_enabled = true;
            joystick_playback_index = 0;
            analysis_capture_bone_positions.clear();
            analysis_capture_enabled = true;

            auto start = std::chrono::high_resolution_clock::now();
#if defined(_WIN32)
            float mem_start = get_process_memory_mb();
            float mem_peak = mem_start;
            double mem_sum = 0.0;
            int mem_samples = 0;
            if (mem_start >= 0.0f)
            {
                mem_sum += mem_start;
                mem_samples++;
            }
#endif

            const int max_steps = (int)samples.size() + 8;
            for (int i = 0; i < max_steps && joystick_playback_enabled; i++)
            {
                update_func();
#if defined(_WIN32)
                float mem_now = get_process_memory_mb();
                if (mem_now >= 0.0f)
                {
                    if (mem_now > mem_peak)
                    {
                        mem_peak = mem_now;
                    }
                    mem_sum += mem_now;
                    mem_samples++;
                }
#endif
            }

            auto end = std::chrono::high_resolution_clock::now();
            stats.elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();

#if defined(_WIN32)
            float mem_end = get_process_memory_mb();
            stats.mem_delta_mb = (mem_start >= 0.0f && mem_end >= 0.0f) ? (mem_end - mem_start) : -1.0f;
            stats.mem_peak_mb = mem_peak;
            stats.mem_avg_mb = (mem_samples > 0) ? (float)(mem_sum / (double)mem_samples) : -1.0f;
#endif

            analysis_capture_enabled = false;
            output = analysis_capture_bone_positions;
            return !output.empty();
        };

        auto compute_mpjpe = [](const std::vector<array1d<vec3>>& mm,
                    const std::vector<array1d<vec3>>& lmm,
                                int& used_frames,
                                int& used_joints) -> double
        {
            used_frames = std::min((int)mm.size(), (int)lmm.size());
            used_joints = 0;
            if (used_frames <= 0)
            {
                return -1.0;
            }

            double error_sum = 0.0;
            long long sample_count = 0;

            for (int f = 0; f < used_frames; f++)
            {
                int joint_count = std::min(mm[f].size, lmm[f].size);
                if (joint_count <= 0)
                {
                    continue;
                }
                used_joints = joint_count;

                for (int j = 0; j < joint_count; j++)
                {
                    vec3 d = mm[f](j) - lmm[f](j);
                    error_sum += std::sqrt((double)d.x * (double)d.x + (double)d.y * (double)d.y + (double)d.z * (double)d.z);
                    sample_count++;
                }
            }

            if (sample_count == 0)
            {
                return -1.0;
            }

            return error_sum / (double)sample_count;
        };

        auto compute_reference_mpjpe = [](const array1d<vec3>& reference,
                                          const std::vector<array1d<vec3>>& capture,
                                          int& used_frames,
                                          int& used_joints) -> double
        {
            used_frames = (int)capture.size();
            used_joints = reference.size;
            if (used_frames <= 0 || used_joints <= 0)
            {
                return -1.0;
            }

            double error_sum = 0.0;
            long long sample_count = 0;

            for (int f = 0; f < used_frames; f++)
            {
                int joint_count = std::min(reference.size, capture[f].size);
                if (joint_count <= 0)
                {
                    continue;
                }

                used_joints = joint_count;

                for (int j = 0; j < joint_count; j++)
                {
                    vec3 d = reference(j) - capture[f](j);
                    error_sum += std::sqrt((double)d.x * (double)d.x + (double)d.y * (double)d.y + (double)d.z * (double)d.z);
                    sample_count++;
                }
            }

            if (sample_count == 0)
            {
                return -1.0;
            }

            return error_sum / (double)sample_count;
        };

        std::vector<analyze_result> results;
        for (const std::string& name : analysis_files)
        {
            analyze_result res;
            res.file = name;

            std::vector<joystick_record_sample> samples;
            std::string input_path = analyze_input_is_file ? std::string(analyze_input_path) : (std::string(analyze_input_path) + "/" + name);
            if (!load_joystick_recording_csv(input_path.c_str(), samples) || samples.empty())
            {
                res.ok = false;
                res.note = "failed to load or empty";
                results.push_back(res);
                continue;
            }

            std::vector<array1d<vec3>> mm_capture;
            std::vector<array1d<vec3>> lmm_capture;
            capture_stats mm_stats;
            capture_stats lmm_stats;

            const bool need_mm = mode == APP_MODE_ANALYZE_BOTH || mode == APP_MODE_ANALYZE_MM;
            const bool need_lmm = mode == APP_MODE_ANALYZE_BOTH || mode == APP_MODE_ANALYZE_LMM;

            bool mm_ok = true;
            bool lmm_ok = true;
            if (need_mm)
            {
                mm_ok = run_capture_for_mode(samples, false, mm_capture, mm_stats);
                res.mm_time_ms = mm_stats.elapsed_ms;
                res.mm_mem_delta_mb = mm_stats.mem_delta_mb;
                res.mm_mem_peak_mb = mm_stats.mem_peak_mb;
                res.mm_mem_avg_mb = mm_stats.mem_avg_mb;
            }
            if (need_lmm)
            {
                lmm_ok = run_capture_for_mode(samples, true, lmm_capture, lmm_stats);
                res.lmm_time_ms = lmm_stats.elapsed_ms;
                res.lmm_mem_delta_mb = lmm_stats.mem_delta_mb;
                res.lmm_mem_peak_mb = lmm_stats.mem_peak_mb;
                res.lmm_mem_avg_mb = lmm_stats.mem_avg_mb;
            }

            if ((need_mm && !mm_ok) || (need_lmm && !lmm_ok))
            {
                res.ok = false;
                res.note = "failed capture";
                results.push_back(res);
                continue;
            }

            int used_frames = 0;
            int used_joints = 0;
            if (mode == APP_MODE_ANALYZE_BOTH)
            {
                res.mpjpe = compute_mpjpe(mm_capture, lmm_capture, used_frames, used_joints);
                res.frame_count = used_frames;
                res.joint_count = used_joints;
                res.ok = res.mpjpe >= 0.0;
                if (!res.ok)
                {
                    res.note = "invalid score";
                }
                std::cout << "Analyze " << name << " -> MPJPE=" << res.mpjpe
                          << " (frames=" << res.frame_count << ", joints=" << res.joint_count << ")";
            }
            else if (mode == APP_MODE_ANALYZE_MM)
            {
                res.mm_mpjpe = compute_reference_mpjpe(base_bone_positions, mm_capture, used_frames, used_joints);
                res.frame_count = used_frames;
                res.joint_count = used_joints;
                res.ok = res.mm_mpjpe >= 0.0;
                if (!res.ok)
                {
                    res.note = "invalid score";
                }
                std::cout << "Analyze " << name << " -> MM MPJPE=" << res.mm_mpjpe
                          << " (frames=" << res.frame_count << ", joints=" << res.joint_count << ")";
            }
            else if (mode == APP_MODE_ANALYZE_LMM)
            {
                res.lmm_mpjpe = compute_reference_mpjpe(base_bone_positions, lmm_capture, used_frames, used_joints);
                res.frame_count = used_frames;
                res.joint_count = used_joints;
                res.ok = res.lmm_mpjpe >= 0.0;
                if (!res.ok)
                {
                    res.note = "invalid score";
                }
                std::cout << "Analyze " << name << " -> LMM MPJPE=" << res.lmm_mpjpe
                          << " (frames=" << res.frame_count << ", joints=" << res.joint_count << ")";
            }

            if (need_mm)
            {
                std::cout << " | time(ms) MM=" << res.mm_time_ms;
#if defined(_WIN32)
                std::cout << " | mem_delta(MB) MM=" << res.mm_mem_delta_mb
                          << " | mem_peak(MB) MM=" << res.mm_mem_peak_mb
                          << " | mem_avg(MB) MM=" << res.mm_mem_avg_mb;
#endif
            }
            if (need_lmm)
            {
                std::cout << " | time(ms) LMM=" << res.lmm_time_ms;
#if defined(_WIN32)
                std::cout << " | mem_delta(MB) LMM=" << res.lmm_mem_delta_mb
                          << " | mem_peak(MB) LMM=" << res.lmm_mem_peak_mb
                          << " | mem_avg(MB) LMM=" << res.lmm_mem_avg_mb;
#endif
            }
            std::cout << std::endl;
            results.push_back(res);
        }

    #if defined(_WIN32)
        _mkdir("./score");
    #else
        mkdir("./score", 0777);
    #endif
        std::string ts = joystick_recording_timestamp_string();
        const char* report_prefix =
            (mode == APP_MODE_ANALYZE_MM) ? "./score/mm_" :
            (mode == APP_MODE_ANALYZE_LMM) ? "./score/lmm_" :
            "./score/both_";
        std::string report_path = std::string(report_prefix) + ts + ".txt";
        FILE* report = fopen(report_path.c_str(), "w");
        if (report != nullptr)
        {
            const char* report_title =
                (mode == APP_MODE_ANALYZE_MM) ? "MM analysis" :
                (mode == APP_MODE_ANALYZE_LMM) ? "LMM analysis" :
                "MM vs LMM analysis (MPJPE)";

            fprintf(report, "%s\n", report_title);
            fprintf(report, "%s: %s\n", analyze_input_is_file ? "input file" : "input folder", analyze_input_path);
            fprintf(report, "generated: %s\n\n", ts.c_str());

            double avg_sum = 0.0;
            int avg_count = 0;
            double mm_time_sum_ms = 0.0;
            double lmm_time_sum_ms = 0.0;
            int time_count = 0;
#if defined(_WIN32)
            double mm_mem_delta_sum_mb = 0.0;
            double lmm_mem_delta_sum_mb = 0.0;
            double mm_mem_peak_sum_mb = 0.0;
            double lmm_mem_peak_sum_mb = 0.0;
            double mm_mem_avg_sum_mb = 0.0;
            double lmm_mem_avg_sum_mb = 0.0;
            int mem_count = 0;
#endif
            for (const analyze_result& r : results)
            {
                if (r.ok)
                {
                    if (mode == APP_MODE_ANALYZE_BOTH)
                    {
                        fprintf(report, "%s | MPJPE=%.6e | frames=%d | joints=%d | time_ms MM=%.3f LMM=%.3f",
                            r.file.c_str(), r.mpjpe, r.frame_count, r.joint_count, r.mm_time_ms, r.lmm_time_ms);
#if defined(_WIN32)
                        fprintf(report, " | mem_delta_mb MM=%.3f LMM=%.3f | mem_peak_mb MM=%.3f LMM=%.3f",
                            r.mm_mem_delta_mb, r.lmm_mem_delta_mb, r.mm_mem_peak_mb, r.lmm_mem_peak_mb);
                        fprintf(report, " | mem_avg_mb MM=%.3f LMM=%.3f",
                            r.mm_mem_avg_mb, r.lmm_mem_avg_mb);
#endif
                        fprintf(report, "\n");
                        avg_sum += r.mpjpe;
                    }
                    else if (mode == APP_MODE_ANALYZE_MM)
                    {
                        fprintf(report, "%s | MM_MPJPE=%.6e | frames=%d | joints=%d | time_ms=%.3f",
                            r.file.c_str(), r.mm_mpjpe, r.frame_count, r.joint_count, r.mm_time_ms);
#if defined(_WIN32)
                        fprintf(report, " | mem_delta_mb=%.3f | mem_peak_mb=%.3f",
                            r.mm_mem_delta_mb, r.mm_mem_peak_mb);
                        fprintf(report, " | mem_avg_mb=%.3f", r.mm_mem_avg_mb);
#endif
                        fprintf(report, "\n");
                        avg_sum += r.mm_mpjpe;
                    }
                    else if (mode == APP_MODE_ANALYZE_LMM)
                    {
                        fprintf(report, "%s | LMM_MPJPE=%.6e | frames=%d | joints=%d | time_ms=%.3f",
                            r.file.c_str(), r.lmm_mpjpe, r.frame_count, r.joint_count, r.lmm_time_ms);
#if defined(_WIN32)
                        fprintf(report, " | mem_delta_mb=%.3f | mem_peak_mb=%.3f",
                            r.lmm_mem_delta_mb, r.lmm_mem_peak_mb);
                        fprintf(report, " | mem_avg_mb=%.3f", r.lmm_mem_avg_mb);
#endif
                        fprintf(report, "\n");
                        avg_sum += r.lmm_mpjpe;
                    }
                    avg_count++;

                    if (mode == APP_MODE_ANALYZE_BOTH)
                    {
                        if (r.mm_time_ms >= 0.0 && r.lmm_time_ms >= 0.0)
                        {
                            mm_time_sum_ms += r.mm_time_ms;
                            lmm_time_sum_ms += r.lmm_time_ms;
                            time_count++;
                        }
                    }
                    else if (mode == APP_MODE_ANALYZE_MM && r.mm_time_ms >= 0.0)
                    {
                        mm_time_sum_ms += r.mm_time_ms;
                        time_count++;
                    }
                    else if (mode == APP_MODE_ANALYZE_LMM && r.lmm_time_ms >= 0.0)
                    {
                        lmm_time_sum_ms += r.lmm_time_ms;
                        time_count++;
                    }
#if defined(_WIN32)
                    if (mode == APP_MODE_ANALYZE_BOTH)
                    {
                        if (r.mm_mem_delta_mb >= 0.0f && r.lmm_mem_delta_mb >= 0.0f &&
                            r.mm_mem_peak_mb >= 0.0f && r.lmm_mem_peak_mb >= 0.0f &&
                            r.mm_mem_avg_mb >= 0.0f && r.lmm_mem_avg_mb >= 0.0f)
                        {
                            mm_mem_delta_sum_mb += r.mm_mem_delta_mb;
                            lmm_mem_delta_sum_mb += r.lmm_mem_delta_mb;
                            mm_mem_peak_sum_mb += r.mm_mem_peak_mb;
                            lmm_mem_peak_sum_mb += r.lmm_mem_peak_mb;
                            mm_mem_avg_sum_mb += r.mm_mem_avg_mb;
                            lmm_mem_avg_sum_mb += r.lmm_mem_avg_mb;
                            mem_count++;
                        }
                    }
                    else if (mode == APP_MODE_ANALYZE_MM)
                    {
                        if (r.mm_mem_delta_mb >= 0.0f && r.mm_mem_peak_mb >= 0.0f && r.mm_mem_avg_mb >= 0.0f)
                        {
                            mm_mem_delta_sum_mb += r.mm_mem_delta_mb;
                            mm_mem_peak_sum_mb += r.mm_mem_peak_mb;
                            mm_mem_avg_sum_mb += r.mm_mem_avg_mb;
                            mem_count++;
                        }
                    }
                    else if (mode == APP_MODE_ANALYZE_LMM)
                    {
                        if (r.lmm_mem_delta_mb >= 0.0f && r.lmm_mem_peak_mb >= 0.0f && r.lmm_mem_avg_mb >= 0.0f)
                        {
                            lmm_mem_delta_sum_mb += r.lmm_mem_delta_mb;
                            lmm_mem_peak_sum_mb += r.lmm_mem_peak_mb;
                            lmm_mem_avg_sum_mb += r.lmm_mem_avg_mb;
                            mem_count++;
                        }
                    }
#endif
                }
                else
                {
                    fprintf(report, "%s | FAILED | %s\n", r.file.c_str(), r.note.c_str());
                }
            }

            if (avg_count > 0)
            {
                if (mode == APP_MODE_ANALYZE_BOTH)
                {
                    fprintf(report, "\nAverage MPJPE: %.6e (across %d files)\n", avg_sum / (double)avg_count, avg_count);
                }
                else if (mode == APP_MODE_ANALYZE_MM)
                {
                    fprintf(report, "\nAverage MM MPJPE: %.6e (across %d files)\n", avg_sum / (double)avg_count, avg_count);
                }
                else if (mode == APP_MODE_ANALYZE_LMM)
                {
                    fprintf(report, "\nAverage LMM MPJPE: %.6e (across %d files)\n", avg_sum / (double)avg_count, avg_count);
                }
            }
            else
            {
                if (mode == APP_MODE_ANALYZE_BOTH)
                {
                    fprintf(report, "\nAverage MPJPE: N/A\n");
                }
                else if (mode == APP_MODE_ANALYZE_MM)
                {
                    fprintf(report, "\nAverage MM MPJPE: N/A\n");
                }
                else if (mode == APP_MODE_ANALYZE_LMM)
                {
                    fprintf(report, "\nAverage LMM MPJPE: N/A\n");
                }
            }

            if (time_count > 0)
            {
                if (mode == APP_MODE_ANALYZE_BOTH)
                {
                    fprintf(report, "Average Time (ms): MM=%.3f LMM=%.3f\n",
                        mm_time_sum_ms / (double)time_count,
                        lmm_time_sum_ms / (double)time_count);
                    fprintf(report, "Total Time (ms): MM=%.3f LMM=%.3f\n",
                        mm_time_sum_ms,
                        lmm_time_sum_ms);
                }
                else if (mode == APP_MODE_ANALYZE_MM)
                {
                    fprintf(report, "Average Time (ms): MM=%.3f\n",
                        mm_time_sum_ms / (double)time_count);
                    fprintf(report, "Total Time (ms): MM=%.3f\n",
                        mm_time_sum_ms);
                }
                else if (mode == APP_MODE_ANALYZE_LMM)
                {
                    fprintf(report, "Average Time (ms): LMM=%.3f\n",
                        lmm_time_sum_ms / (double)time_count);
                    fprintf(report, "Total Time (ms): LMM=%.3f\n",
                        lmm_time_sum_ms);
                }
            }
            else
            {
                fprintf(report, "Average Time (ms): N/A\n");
            }

#if defined(_WIN32)
            if (mem_count > 0)
            {
                if (mode == APP_MODE_ANALYZE_BOTH)
                {
                    fprintf(report, "Average Memory Delta (MB): MM=%.3f LMM=%.3f\n",
                        mm_mem_delta_sum_mb / (double)mem_count,
                        lmm_mem_delta_sum_mb / (double)mem_count);
                    fprintf(report, "Average Memory Peak (MB): MM=%.3f LMM=%.3f\n",
                        mm_mem_peak_sum_mb / (double)mem_count,
                        lmm_mem_peak_sum_mb / (double)mem_count);
                    fprintf(report, "Average Memory Usage (MB): MM=%.3f LMM=%.3f\n",
                        mm_mem_avg_sum_mb / (double)mem_count,
                        lmm_mem_avg_sum_mb / (double)mem_count);
                }
                else if (mode == APP_MODE_ANALYZE_MM)
                {
                    fprintf(report, "Average Memory Delta (MB): MM=%.3f\n",
                        mm_mem_delta_sum_mb / (double)mem_count);
                    fprintf(report, "Average Memory Peak (MB): MM=%.3f\n",
                        mm_mem_peak_sum_mb / (double)mem_count);
                    fprintf(report, "Average Memory Usage (MB): MM=%.3f\n",
                        mm_mem_avg_sum_mb / (double)mem_count);
                }
                else if (mode == APP_MODE_ANALYZE_LMM)
                {
                    fprintf(report, "Average Memory Delta (MB): LMM=%.3f\n",
                        lmm_mem_delta_sum_mb / (double)mem_count);
                    fprintf(report, "Average Memory Peak (MB): LMM=%.3f\n",
                        lmm_mem_peak_sum_mb / (double)mem_count);
                    fprintf(report, "Average Memory Usage (MB): LMM=%.3f\n",
                        lmm_mem_avg_sum_mb / (double)mem_count);
                }
            }
            else
            {
                fprintf(report, "Average Memory Delta (MB): N/A\n");
                fprintf(report, "Average Memory Peak (MB): N/A\n");
                fprintf(report, "Average Memory Usage (MB): N/A\n");
            }
#else
            fprintf(report, "Memory comparison: only available on Windows\n");
#endif

            fclose(report);
            std::cout << "Analysis report exported to: " << report_path << std::endl;
        }
        else
        {
            std::cout << "Failed to write analysis report at: " << report_path << std::endl;
        }
    }
    else while (!WindowShouldClose())
    {
        update_func();
    }
#endif

    if (joystick_recording_enabled && !joystick_recording_samples.empty())
    {
        joystick_recording_last_saved_count = (int)joystick_recording_samples.size();
        joystick_recording_last_save_ok = save_joystick_recording_csv(
            joystick_recording_output_file,
            joystick_recording_samples);

        if (joystick_recording_last_save_ok)
        {
            snprintf(joystick_recording_last_saved_file, sizeof(joystick_recording_last_saved_file), "%s", joystick_recording_output_file);
        }
    }

    // Unload stuff and finish
    UnloadModel(character_model);
    UnloadModel(ground_plane_model);
    UnloadShader(character_shader);
    if (!using_glb_ground)
    {
        UnloadShader(ground_plane_shader);
    }

    CloseWindow();

    return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Exception error: " << e.what() << std::endl;
        std::cout << "EXCEPTION: " << e.what() << std::endl;
        return 1;
    }
    catch (...)
    {
        std::cerr << "Unknown error occurred" << std::endl;
        std::cout << "UNKNOWN ERROR during initialization" << std::endl;
        return 1;
    }
}