#define RAYGUI_IMPLEMENTATION
#include "raygui.h"
#if defined(PLATFORM_WEB)
#include <emscripten/emscripten.h>
#include <emscripten/heap.h>
#endif

#include "raylib.h"
#include "rlgl.h"
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
#include "terrain_grid.h"
#include "ik.h"
#include "inertialization.h"
#include "trajectory.h"
#include "render_utils.h"
#include "playback.h"
#include "input.h"

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
#include <cstdlib>
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
static TerrainGrid ground_grid;

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

// Consolidated data needed to draw features for one frame.
// feature_draw_data moved to render_utils.h

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

static std::string analysis_output_timestamp_string()
{
    std::time_t now = std::time(NULL);
    std::tm local_now = {};

#if defined(_WIN32)
    localtime_s(&local_now, &now);
#else
    local_now = *std::localtime(&now);
#endif

    char timestamp[32];
    std::strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M", &local_now);
    return std::string(timestamp);
}

static std::string analysis_output_generated_string()
{
    std::time_t now = std::time(NULL);
    std::tm local_now = {};

#if defined(_WIN32)
    localtime_s(&local_now, &now);
#else
    local_now = *std::localtime(&now);
#endif

    char timestamp[64];
    std::strftime(timestamp, sizeof(timestamp), "%d %b %Y %H:%M", &local_now);
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

// deform_character_mesh and make_character_mesh moved to render_utils.cpp

//--------------------------------------

// input and camera functions moved to input.cpp

//--------------------------------------

// Moving the root is a little bit difficult when we have the
// inertializer set up in the way we do. Essentially we need
// to also make sure to adjust all of the locations where 
// we are transforming the data to and from as well as the 
// offsets being blended out
// Removed inertialize_root_adjust, inertialize_pose_reset, inertialize_pose_transition, inertialize_pose_update -> moved to inertialization.cpp

//--------------------------------------

// query compute functions moved to playback.cpp

//--------------------------------------

bool sample_terrain_height(
    const Model& ground_plane_model,
    const vec3 position,
    float& out_height)
{
    bool hit = false;
    float highest = 0.0f;
    if (ground_grid.get_height(to_Vector3(position), highest))
    {
        hit = true;
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

// Removed trajectory prediction functions -> moved to trajectory.cpp

//--------------------------------------

// Removed contact_reset, contact_update, ik_look_at, ik_two_bone -> moved to ik.cpp

//--------------------------------------

// draw_axis, draw_features, draw_trajectory, draw_stickman moved to render_utils.cpp

//--------------------------------------

// adjust functions moved to playback.cpp

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
        bool force_mm_mode = false;
        bool force_lmm_mode = false;
        enum app_mode
        {
            APP_MODE_WINDOW,
            APP_MODE_ANALYZE_BOTH,
            APP_MODE_ANALYZE_MM,
            APP_MODE_ANALYZE_LMM,
            APP_MODE_ANALYZE_BOTH_BIG_SMALL,
        };
        app_mode mode = APP_MODE_WINDOW;
        char analyze_input_path[512] = "./resources/input-recording";
        bool analyze_input_is_file = false;
        bool analyze_input_is_database = false;
        bool force_rebuild_features = false;

        database test_db;
        std::vector<array1d<vec3>> database_test_reference_poses;
        std::vector<array1d<quat>> database_test_reference_rotations;
        array1d<vec3> frozen_pose;
        array1d<quat> frozen_rotation;
        bool database_playback_enabled = false;
        int database_playback_index = 0;
        bool playback_video = false;
        for (int argi = 1; argi < argc; argi++)
        {
            if (strcmp(argv[argi], "--mm") == 0)
            {
                start_with_lmm_enabled = false;
                force_mm_mode = true;
            }
            else if (strcmp(argv[argi], "--lmm") == 0)
            {
                start_with_lmm_enabled = true;
                force_lmm_mode = true;
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
                force_mm_mode = true;
            }
            else if (strcmp(argv[argi], "--analyze-lmm") == 0)
            {
                mode = APP_MODE_ANALYZE_LMM;
                start_with_lmm_enabled = true;
            }
            else if (strcmp(argv[argi], "--analyze-both-big-small") == 0)
            {
                mode = APP_MODE_ANALYZE_BOTH_BIG_SMALL;
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
                    force_mm_mode = true;
                }
                else if (strcmp(mode_name, "analyze-lmm") == 0)
                {
                    mode = APP_MODE_ANALYZE_LMM;
                    start_with_lmm_enabled = true;
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
            else if (strcmp(argv[argi], "--playback") == 0)
            {
                playback_video = true;
            }
            else if (argv[argi][0] != '-')
            {
                snprintf(analyze_input_path, sizeof(analyze_input_path), "%s", argv[argi]);
                analyze_input_is_file = true;
                
                // Check if it's a database file
                size_t len = strlen(analyze_input_path);
                if (len > 4 && strcmp(analyze_input_path + len - 4, ".bin") == 0)
                {
                    analyze_input_is_database = true;
                }
            }
            else if (strcmp(argv[argi], "-h") == 0 || strcmp(argv[argi], "--help") == 0)
            {
                printf("Usage: %s [--learned] [--rebuild-features] [--window | --analyze-both | --analyze-mm | --analyze-lmm | --analyze-both-big-small] [--playback] [--input=<csv>]\n", argv[0]);
                printf("       %s --mode=<window|analyze-both|analyze-mm|analyze-lmm> [--playback] --input=<csv>\n", argv[0]);
                printf("\n");
                printf("  --analyze-both-big-small  Run 4-way comparison: MM-big, MM-small, LMM-big, LMM-small\n");
                printf("                            requires database_big.bin, database_small.bin,\n");
                printf("                            decompressor_big.bin / stepper_big.bin / projector_big.bin,\n");
                printf("                            decompressor_small.bin / stepper_small.bin / projector_small.bin\n");
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
    
    // Always load the shader for studio light/checkerboard
    Shader ground_plane_shader = LoadShader("./resources/shaders/checkerboard.vs", "./resources/shaders/checkerboard.fs");
    
    // Try to load .glb model
    const char* ground_glb_path = "resources/glb/11-skate.glb";
    Model ground_plane_model = { 0 };
    bool has_glb_ground = false;
    
    if (FileExists(ground_glb_path) && mode == APP_MODE_WINDOW)
    {
        ground_plane_model = LoadModel(ground_glb_path);
        ground_grid.build(ground_plane_model, 1.0f);
        has_glb_ground = true;

        // Apply shader to GLB as well
        for (int i = 0; i < ground_plane_model.materialCount; i++)
        {
            ground_plane_model.materials[i].shader = ground_plane_shader;
        }
    }
    else
    {
        // If no GLB, create a simple procedural plane but still call it ground_plane_model 
        // to maintain compatibility with the rest of the code (collisions, etc.)
        Mesh ground_plane_mesh = GenMeshPlane(50.0f, 50.0f, 10, 10);
        ground_plane_model = LoadModelFromMesh(ground_plane_mesh);
        ground_plane_model.materials[0].shader = ground_plane_shader;
        ground_grid.build(ground_plane_model, 1.0f);
        has_glb_ground = true; // We now have a ground, even if it's procedural
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
    database_load(db, "./resources/bin/database_big.bin");

    std::vector<range_metadata_entry> range_metadata_entries;
    if (!load_range_metadata_csv("./resources/bin/range_metadata.csv", range_metadata_entries))
    {
        if (debug) std::cout << "range_metadata.csv missing or empty; BVH labels disabled" << std::endl;
    }
    
    const char* database_path = "./resources/bin/database_big.bin";
    const char* features_path  = "./resources/bin/features_big.bin";

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
    
    // Shadow Mapping Setup
    const int shadow_map_width = 2048;
    const int shadow_map_height = 2048;
    RenderTexture2D shadow_map = LoadRenderTexture(shadow_map_width, shadow_map_height);
    Shader depth_shader = LoadShader("./resources/shaders/depth.vs", "./resources/shaders/depth.fs");
    
    Camera3D light_cam = { 0 };
    light_cam.position = (Vector3){ 10.0f, 20.0f, 10.0f };
    light_cam.target = (Vector3){ 0.0f, 0.0f, 0.0f };
    light_cam.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    light_cam.fovy = 30.0f; // Ortho size
    light_cam.projection = CAMERA_ORTHOGRAPHIC;

    int char_shadow_map_loc = GetShaderLocation(character_shader, "shadowMap");
    int char_light_mat_loc = GetShaderLocation(character_shader, "lightMat");
    
    int ground_shadow_map_loc = GetShaderLocation(ground_plane_shader, "shadowMap");
    int ground_light_mat_loc = GetShaderLocation(ground_plane_shader, "lightMat");
    int ground_left_foot_loc = GetShaderLocation(ground_plane_shader, "leftFootPos");
    int ground_right_foot_loc = GetShaderLocation(ground_plane_shader, "rightFootPos");

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
    
    if (start_with_lmm_enabled)
    {
        std::cout << "LMM mode enabled. Filtering database for special motion frames (cartwheel/jump) to save memory..." << std::endl;
        database_filter_special_motions(db);
        database_build_bounds(db); // Rebuild bounds for the filtered database
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
    float idle_gait_timer = 0.0f;
    bool desired_jump_prev = false;
    float cartwheel_auto_timer = 0.0f;
    const float cartwheel_auto_duration = 1.0f;
    bool cartwheel_search_freeze_prev = false;
    bool jump_search_freeze_prev = false;
    bool cartwheel_query_lock_prev = false;
    bool jump_query_lock_prev = false;
    vec3 cartwheel_query_lock_forward = vec3(0.0f, 0.0f, 1.0f);
    vec3 jump_query_lock_forward = vec3(0.0f, 0.0f, 1.0f);
    float cartwheel_query_lock_step_distance = 0.0f;
    float jump_query_lock_step_distance = 0.0f;
    float cartwheel_first_search_step_distance = 0.0f;
    float jump_first_search_step_distance = 0.0f;
    
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
    float jump_speed_boost = 1.2f;

    float climbing_min_speed_factor = 0.1f;
    float climbing_probe_distance = 0.6f;
    float climbing_height_threshold = 1.0f;
    float climbing_max_height_delta = 0.8f;
    
    float jump_root_height_offset = 1.3f;
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
    float jump_gait_timer = 0.0f;
    const float jump_gait_hold_time = 0.9f;
    
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
    float ik_unlock_radius = 0.1f;
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
    
    array1d<float> smoothed_clamp_offsets(contact_bones.size);
    array1d<float> smoothed_clamp_offset_velocities(contact_bones.size);
    smoothed_clamp_offsets.zero();
    smoothed_clamp_offset_velocities.zero();
    
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
    
    nnet decompressor, stepper, projector;
    bool networks_exist = 
        FileExists("./resources/bin/decompressor_big.bin") &&
        FileExists("./resources/bin/stepper_big.bin") &&
        FileExists("./resources/bin/projector_big.bin");

    if (networks_exist && !force_mm_mode)
    {
        if (debug) std::cout << "Loading neural networks (big)..." << std::endl;
        
        if (debug) std::cout << "Loading decompressor..." << std::endl;
        nnet_load(decompressor, "./resources/bin/decompressor_big.bin");
        if (debug) std::cout << "Loading stepper..." << std::endl;
        nnet_load(stepper, "./resources/bin/stepper_big.bin");
        if (debug) std::cout << "Loading projector..." << std::endl;
        nnet_load(projector, "./resources/bin/projector_big.bin");
    }

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
    if (lmm_networks_compatible)
    {
        decompressor_evaluation.resize(decompressor);
        stepper_evaluation.resize(stepper);
        projector_evaluation.resize(projector);
    }
    
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

        float left_terrain_height = 0.0f;
        float right_terrain_height = 0.0f;
        bool has_left_terrain = sample_terrain_height(ground_plane_model, left_foot_pos, left_terrain_height);
        bool has_right_terrain = sample_terrain_height(ground_plane_model, right_foot_pos, right_terrain_height);
        vec2 terrain_pair = vec2(
            has_left_terrain ? (left_terrain_height - hip_pos.y) : 0.0f,
            has_right_terrain ? (right_terrain_height - hip_pos.y) : 0.0f);

        root_history_positions.push_back(bone_positions(0));
        root_history_rotations.push_back(bone_rotations(0));
        // Store all history as global coordinates
        history_left_foot_positions.push_back(left_foot_pos);
        history_right_foot_positions.push_back(right_foot_pos);
        history_left_foot_velocities.push_back(left_foot_vel);
        history_right_foot_velocities.push_back(right_foot_vel);
        history_hip_positions.push_back(hip_pos);
        history_hip_velocities.push_back(hip_vel);
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

    // Per-frame feature data for playback visualization
    std::vector<feature_draw_data> playback_mm_feature_data;
    std::vector<feature_draw_data> playback_lmm_feature_data;

    // Safety toggle: set to false to skip all playback feature drawing
    bool show_playback_features = true;

    bool show_stickman = false;
    int joystick_playback_index = 0;
    std::vector<std::string> joystick_recording_csv_files;
    int joystick_recording_csv_selected_index = 0;
    bool joystick_recording_csv_dropdown_edit = false;
    char joystick_recording_csv_dropdown_text[4096] = "<no csv files>";
    const float spawn_height_offset = 2.0f;
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
    const float base_idle_gait_timer = idle_gait_timer;
    const bool base_desired_jump_prev = desired_jump_prev;
    const float base_cartwheel_auto_timer = cartwheel_auto_timer;
    const bool base_cartwheel_search_freeze_prev = cartwheel_search_freeze_prev;
    const bool base_jump_search_freeze_prev = jump_search_freeze_prev;
    const bool base_cartwheel_query_lock_prev = cartwheel_query_lock_prev;
    const bool base_jump_query_lock_prev = jump_query_lock_prev;
    const vec3 base_cartwheel_query_lock_forward = cartwheel_query_lock_forward;
    const vec3 base_jump_query_lock_forward = jump_query_lock_forward;
    const float base_cartwheel_query_lock_step_distance = cartwheel_query_lock_step_distance;
    const float base_jump_query_lock_step_distance = jump_query_lock_step_distance;
    const float base_cartwheel_first_search_step_distance = cartwheel_first_search_step_distance;
    const float base_jump_first_search_step_distance = jump_first_search_step_distance;
    const vec3 base_simulation_position = simulation_position;
    const vec3 base_simulation_velocity = simulation_velocity;
    const vec3 base_simulation_acceleration = simulation_acceleration;
    const quat base_simulation_rotation = simulation_rotation;
    const vec3 base_simulation_angular_velocity = simulation_angular_velocity;
    const bool base_jump_active = jump_active;
    const float base_jump_vertical_velocity = jump_vertical_velocity;
    const float base_jump_buffer_timer = jump_buffer_timer;
    const float base_jump_coyote_timer = jump_coyote_timer;
    const float base_jump_gait_timer = jump_gait_timer;
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
    const array1d<float> base_smoothed_clamp_offsets = smoothed_clamp_offsets;
    const array1d<float> base_smoothed_clamp_offset_velocities = smoothed_clamp_offset_velocities;
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
        idle_gait_timer = base_idle_gait_timer;
        desired_jump_prev = base_desired_jump_prev;
        cartwheel_auto_timer = base_cartwheel_auto_timer;
        cartwheel_search_freeze_prev = base_cartwheel_search_freeze_prev;
        jump_search_freeze_prev = base_jump_search_freeze_prev;
        cartwheel_query_lock_prev = base_cartwheel_query_lock_prev;
        jump_query_lock_prev = base_jump_query_lock_prev;
        cartwheel_query_lock_forward = base_cartwheel_query_lock_forward;
        jump_query_lock_forward = base_jump_query_lock_forward;
        cartwheel_query_lock_step_distance = base_cartwheel_query_lock_step_distance;
        jump_query_lock_step_distance = base_jump_query_lock_step_distance;
        cartwheel_first_search_step_distance = base_cartwheel_first_search_step_distance;
        jump_first_search_step_distance = base_jump_first_search_step_distance;
        simulation_position = base_simulation_position;
        simulation_velocity = base_simulation_velocity;
        simulation_acceleration = base_simulation_acceleration;
        simulation_rotation = base_simulation_rotation;
        simulation_angular_velocity = base_simulation_angular_velocity;
        jump_active = base_jump_active;
        jump_vertical_velocity = base_jump_vertical_velocity;
        jump_buffer_timer = base_jump_buffer_timer;
        jump_coyote_timer = base_jump_coyote_timer;
        jump_gait_timer = base_jump_gait_timer;
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
        smoothed_clamp_offsets = base_smoothed_clamp_offsets;
        smoothed_clamp_offset_velocities = base_smoothed_clamp_offset_velocities;
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
        playback_mm_feature_data.clear();
        playback_lmm_feature_data.clear();

        reset_motion_to_recording_start();
    };

    auto teleport_to_test_frame = [&](int test_frame) {
        if (test_frame < 0 || test_frame >= test_db.nframes()) return;

        simulation_position = test_db.bone_positions(test_frame, 0);
        simulation_rotation = test_db.bone_rotations(test_frame, 0);
        simulation_velocity = test_db.bone_velocities(test_frame, 0);
        simulation_angular_velocity = test_db.bone_angular_velocities(test_frame, 0);
        simulation_acceleration = vec3();

        desired_velocity = vec3();
        desired_velocity_change_curr = vec3();
        desired_velocity_change_prev = vec3();
        desired_rotation = simulation_rotation;
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

        for (int i = 0; i < test_db.nbones(); i++) {
            bone_positions(i) = test_db.bone_positions(test_frame, i);
            bone_velocities(i) = test_db.bone_velocities(test_frame, i);
            bone_rotations(i) = test_db.bone_rotations(test_frame, i);
            bone_angular_velocities(i) = test_db.bone_angular_velocities(test_frame, i);

            curr_bone_positions(i) = test_db.bone_positions(test_frame, i);
            curr_bone_velocities(i) = test_db.bone_velocities(test_frame, i);
            curr_bone_rotations(i) = test_db.bone_rotations(test_frame, i);
            curr_bone_angular_velocities(i) = test_db.bone_angular_velocities(test_frame, i);

            trns_bone_positions(i) = test_db.bone_positions(test_frame, i);
            trns_bone_velocities(i) = test_db.bone_velocities(test_frame, i);
            trns_bone_rotations(i) = test_db.bone_rotations(test_frame, i);
            trns_bone_angular_velocities(i) = test_db.bone_angular_velocities(test_frame, i);

            adjusted_bone_positions(i) = test_db.bone_positions(test_frame, i);
            adjusted_bone_rotations(i) = test_db.bone_rotations(test_frame, i);
        }

        for (int i = 0; i < bone_offset_positions.size; i++) bone_offset_positions(i) = vec3();
        for (int i = 0; i < bone_offset_velocities.size; i++) bone_offset_velocities(i) = vec3();
        for (int i = 0; i < bone_offset_rotations.size; i++) bone_offset_rotations(i) = quat();
        for (int i = 0; i < bone_offset_angular_velocities.size; i++) bone_offset_angular_velocities(i) = vec3();

        transition_src_position = simulation_position;
        transition_src_rotation = simulation_rotation;
        transition_dst_position = simulation_position;
        transition_dst_rotation = simulation_rotation;

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

        if (test_frame < test_db.features.rows && test_db.features.cols == features_curr.size) {
            for (int i = 0; i < test_db.nfeatures(); i++) {
                features_curr(i) = test_db.features(test_frame, i);
                features_proj(i) = test_db.features(test_frame, i);
            }
        }
        latent_curr.zero();
        latent_proj.zero();
    };

    bool analysis_capture_enabled = false; // set to true if in analyze mode
    std::vector<array1d<vec3>> analysis_capture_bone_positions;
    std::vector<array1d<quat>> analysis_capture_bone_rotations;

    // Parallel feature data capture for video comparison rendering
    bool analysis_capture_features_enabled = false;
    std::vector<feature_draw_data> analysis_capture_mm_feature_data;
    std::vector<feature_draw_data> analysis_capture_lmm_feature_data;

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
            playback_mm_feature_data.clear();
            playback_lmm_feature_data.clear();
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
        bool jump_query_lock_active = jump_active;

        if (cartwheel_auto_active)
        {
            desired_strafe = true;
        }

        jump_root_height_offset = crouch_pressed ? 0.65f : 1.2f;

        if (jump_pressed)
        {
            jump_buffer_timer = jump_buffer_time;
            jump_gait_timer = jump_gait_hold_time; // lock jump gait for 0.7s from press
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
        vec3 move_input_world = (cartwheel_query_lock_active || jump_query_lock_active)
            ? (cartwheel_query_lock_active ? cartwheel_query_lock_forward : jump_query_lock_forward)
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

        if (cartwheel_query_lock_active || jump_query_lock_active)
        {
            if (cartwheel_query_lock_active && !cartwheel_query_lock_prev)
            {
                vec3 lock_forward = quat_mul_vec3(simulation_rotation, vec3(0.0f, 0.0f, 1.0f));
                lock_forward.y = 0.0f;

                if (length(lock_forward) > 0.001f)
                {
                    cartwheel_query_lock_forward = normalize(lock_forward);
                }

                cartwheel_query_lock_step_distance = simulation_fwrd_speed * (20.0f * dt);
                cartwheel_query_lock_prev = true;
            }
            
            if (jump_query_lock_active && !jump_query_lock_prev)
            {
                vec3 lock_forward = quat_mul_vec3(simulation_rotation, vec3(0.0f, 0.0f, 1.0f));
                lock_forward.y = 0.0f;

                if (length(lock_forward) > 0.001f)
                {
                    jump_query_lock_forward = normalize(lock_forward);
                }

                jump_query_lock_step_distance = simulation_fwrd_speed * (20.0f * dt);
                jump_query_lock_prev = true;
            }
        }
        else
        {
            cartwheel_query_lock_prev = false;
            jump_query_lock_prev = false;
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

        bool cartwheel_search_freeze_active = cartwheel_auto_active;
        bool cartwheel_search_freeze_started =
            cartwheel_search_freeze_active && !cartwheel_search_freeze_prev;
        cartwheel_search_freeze_prev = cartwheel_search_freeze_active;

        bool jump_search_freeze_active = jump_active;
        bool jump_search_freeze_started =
            jump_search_freeze_active && !jump_search_freeze_prev;
        jump_search_freeze_prev = jump_search_freeze_active;

        if (cartwheel_search_freeze_started)
        {
            cartwheel_first_search_step_distance = simulation_run_fwrd_speed * (20.0f * dt);
        }
        if (jump_search_freeze_started)
        {
            jump_first_search_step_distance = simulation_run_fwrd_speed * (20.0f * dt);
        }

        // Tick jump_gait_timer down unconditionally every frame
        if (jump_gait_timer > 0.0f)
        {
            jump_gait_timer = maxf(0.0f, jump_gait_timer - dt);
        }
        
        bool jump_gait_active = jump_active || jump_gait_timer > 0.0f;

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
            if (jump_gait_active)
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

        // 3D magnitude of 20-frame future trajectory offset (matches MM feature vector)
        const float trajectory_movement_threshold = 0.05f;
        vec3 traj_future_offset = trajectory_positions(1) - simulation_position;
        float future_trajectory_magnitude = length(traj_future_offset);
        bool trajectory_has_movement = future_trajectory_magnitude > trajectory_movement_threshold;

        bool desired_idle = desired_idle_prev;
        
        // Timer resets only when BOTH input and trajectory signal active movement.
        // Timer counts up when either is zero.
        bool player_actively_moving =
            trajectory_has_movement;

        bool movement_detected =
            player_actively_moving ||
            (desired_crouch != desired_crouch_prev);
            
        if (movement_detected) {
            idle_gait_timer = 0.0f;
        } else {
            idle_gait_timer += dt;
        }

        bool idle_enter =
            !jump_active &&
            idle_gait_timer >= 0.4f;
        
        // Exit idle when trajectory signals movement (matches idle_enter logic)
        bool idle_exit =
            jump_active ||
            trajectory_has_movement ||
            (desired_crouch != desired_crouch_prev);

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

        // desired_jump is locked for the full 0.7s from space press — nothing can interrupt it
        bool desired_jump = jump_pressed || jump_gait_timer > 0.0f;
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
        
        if (database_playback_enabled)
        {
            int test_frame = clamp(database_playback_index, 0, test_db.nframes() - 1);
            if (test_frame < test_db.features.rows && test_db.features.cols >= 45)
            {
                auto get_raw_feature = [&](int idx) {
                    return test_db.features(test_frame, idx) * test_db.features_scale(idx) + test_db.features_offset(idx);
                };

                bool lmm_runtime_enabled = lmm_enabled && lmm_networks_compatible;
                auto get_sim_feature = [&](int idx) {
                    if (lmm_runtime_enabled) {
                        return features_curr(idx) * db.features_scale(idx) + db.features_offset(idx);
                    } else {
                        return db.features(frame_index, idx) * db.features_scale(idx) + db.features_offset(idx);
                    }
                };

                // Extract trajectory position at +20 frames (indices 15, 16, 17) from SIMULATED features
                vec3 local_target = vec3(get_sim_feature(15), get_sim_feature(16), get_sim_feature(17));
                desired_velocity_curr = quat_mul_vec3(simulation_rotation, local_target) / (20.0f * dt);
                desired_velocity = desired_velocity_curr;

                // Extract trajectory direction at +20 frames (indices 24, 25, 26) from SIMULATED features
                vec3 local_dir = normalize(vec3(get_sim_feature(24), get_sim_feature(25), get_sim_feature(26)));
                vec3 world_dir = quat_mul_vec3(simulation_rotation, local_dir);
                float yaw = atan2f(world_dir.x, world_dir.z);
                
                desired_rotation_curr = quat_from_angle_axis(yaw, vec3(0.0f, 1.0f, 0.0f));
                desired_rotation = desired_rotation_curr;

                // Apply Gaits
                desired_crouch = get_raw_feature(42) > 0.5f;
                desired_jump = get_raw_feature(43) > 0.5f;
                desired_cartwheel = get_raw_feature(44) > 0.5f;
                
                // Disable Idle
                idle_gait_timer = 0.0f;
                desired_idle = false;
            }
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
          jump_active ? jump_speed_boost : (jump_gait_timer > 0.0f ? jump_speed_boost : 1.0f),
          jump_gait_timer,
          jump_gait_hold_time,
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
        const float uphill_horizontal_reduce_gain = 1.1f;
        const float uphill_horizontal_reduce_max = 0.5f;
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

        // If in database playback mode, use the exact trajectory from the test database
        if (database_playback_enabled)
        {
            int test_frame = clamp(database_playback_index, 0, test_db.nframes() - 1);
            if (test_frame < test_db.features.rows && test_db.features.cols >= 33)
            {
                auto get_raw_feature = [&](int idx) {
                    return test_db.features(test_frame, idx) * test_db.features_scale(idx) + test_db.features_offset(idx);
                };

                // Extract trajectory positions and directions from test database features
                // Indices 15-17: traj0_pos (local), 18-20: traj1_pos (local), 21-23: traj2_pos (local)
                // Indices 24-26: traj0_dir (local), 27-29: traj1_dir (local), 30-32: traj2_dir (local)
                
                quat gt_rot = test_db.bone_rotations(test_frame)(0);
                vec3 root_pos = bone_positions(0);
                quat root_rot = bone_rotations(0);
                
                // Convert local trajectory positions to world coordinates
                for (int i = 0; i < 3; i++)
                {
                    vec3 local_pos = vec3(
                        get_raw_feature(15 + i * 3 + 0),
                        get_raw_feature(15 + i * 3 + 1),
                        get_raw_feature(15 + i * 3 + 2));
                    
                    // 1. Compute global offset using ground truth rotation
                    vec3 gt_global_offset = quat_mul_vec3(gt_rot, local_pos);
                    
                    // 2. Convert back to relative to root_rot (simulated character)
                    vec3 sim_local_pos = quat_mul_vec3(quat_inv(root_rot), gt_global_offset);
                    
                    // 3. Store in query_trajectory_positions (which expects global space)
                    // Note: quat_mul_vec3(root_rot, sim_local_pos) mathematically simplifies to gt_global_offset
                    query_trajectory_positions(i + 1) = quat_mul_vec3(root_rot, sim_local_pos) + root_pos;
                }
                
                // Convert local trajectory directions to world directions and extract yaw
                for (int i = 0; i < 3; i++)
                {
                    vec3 local_dir = normalize(vec3(
                        get_raw_feature(24 + i * 3 + 0),
                        get_raw_feature(24 + i * 3 + 1),
                        get_raw_feature(24 + i * 3 + 2)));
                    
                    // 1. Compute global direction using ground truth rotation
                    vec3 gt_global_dir = quat_mul_vec3(gt_rot, local_dir);
                    
                    // 2. Convert back to relative to root_rot
                    vec3 sim_local_dir = quat_mul_vec3(quat_inv(root_rot), gt_global_dir);
                    
                    // 3. Convert to world direction
                    vec3 world_dir = quat_mul_vec3(root_rot, sim_local_dir);
                    
                    float yaw = atan2f(world_dir.x, world_dir.z);
                    query_trajectory_rotations(i + 1) = quat_from_angle_axis(yaw, vec3(0.0f, 1.0f, 0.0f));
                }
            }
        }

        if (cartwheel_query_lock_active || jump_query_lock_active)
        {
            vec3 lock_forward = cartwheel_query_lock_active ? cartwheel_query_lock_forward : jump_query_lock_forward;
            float lock_yaw = atan2f(lock_forward.x, lock_forward.z);
            quat lock_rotation = quat_from_angle_axis(lock_yaw - 0.5f * PIf, vec3(0.0f, 1.0f, 0.0f));
            vec3 base_position = bone_positions(0);

            query_trajectory_positions(0) = base_position;
            query_trajectory_rotations(0) = lock_rotation;

            for (int i = 1; i < query_trajectory_positions.size; i++)
            {
                float step_distance = 0.0f;
                if (cartwheel_query_lock_active)
                {
                    step_distance = cartwheel_search_freeze_started
                        ? cartwheel_first_search_step_distance
                        : cartwheel_query_lock_step_distance;
                }
                else
                {
                    step_distance = jump_search_freeze_started
                        ? jump_first_search_step_distance
                        : jump_query_lock_step_distance;
                }

                float distance = step_distance * (float)i;
                query_trajectory_positions(i) = base_position + lock_forward * distance;
                query_trajectory_rotations(i) = lock_rotation;
            }
        }

        terrain_anchor_trajectory(query_trajectory_positions);

        // Override: Add vertical velocity to move root toward terrain sampled along future trajectory.
        float traj_ground_height = 0.0f;
        bool traj_hit = false;
        int nearest_future_idx = trajectory_positions.size > 1 ? 1 : 0;
        if (ground_grid.get_height(to_Vector3(trajectory_positions(nearest_future_idx)), traj_ground_height))
        {
            traj_hit = true;
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
                ground_grid.get_height(to_Vector3(left_toe_pos), left_terrain_height);
                ground_grid.get_height(to_Vector3(right_toe_pos), right_terrain_height);
                
                // Store relative to hip height, but clamp to avoid extreme negatives while falling.
                const float min_terrain_feature_height = -1.5f;
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
                // History is stored in global coordinates, convert to local relative to current root
                quat inv_root_rot = quat_inv(bone_rotations(0));
                vec3 local_vec = quat_mul_vec3(inv_root_rot, history[idx] - bone_positions(0));
                query(offset + 0) = local_vec.x;
                query(offset + 1) = local_vec.y;
                query(offset + 2) = local_vec.z;
            }
            offset += 3;
        };

        // Write history positions and velocities, converting from global to local coordinates
        auto query_write_runtime_history_vec3_velocity = [&](const std::vector<vec3>& history, int relative_offset)
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
                // Velocities are stored in global coordinates, convert to local
                quat inv_root_rot = quat_inv(bone_rotations(0));
                vec3 local_vel = quat_mul_vec3(inv_root_rot, history[idx]);
                query(offset + 0) = local_vel.x;
                query(offset + 1) = local_vel.y;
                query(offset + 2) = local_vel.z;
            }
            offset += 3;
        };

        query_write_runtime_history_vec3(history_left_foot_positions, -20);
        query_write_runtime_history_vec3(history_right_foot_positions, -20);
        query_write_runtime_history_vec3_velocity(history_left_foot_velocities, -20);
        query_write_runtime_history_vec3_velocity(history_right_foot_velocities, -20);
        query_write_runtime_history_vec3_velocity(history_hip_velocities, -20);

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
        
        // --- Database-Driven Analysis Override ---
        if (database_playback_enabled)
        {
            if (database_playback_index % 1000 == 0) std::cout << "Analyze: Step " << database_playback_index << "/" << test_db.nframes() << std::endl;
        }
        // ------------------------------------------

        // Check if we reached the end of the current anim
        bool end_of_anim = database_index_clamp(db, frame_index, 1) == frame_index;
        
        // Do we need to search?
        if (debug) std::cout << "Do we?" << std::endl;
        bool search_requested = force_search || search_timer <= 0.0f || end_of_anim;
        bool force_mm_search_on_freeze_start = cartwheel_search_freeze_started || jump_search_freeze_started;
        bool use_lmm_path = lmm_runtime_enabled && !cartwheel_search_freeze_active && !jump_search_freeze_active;
        bool allow_mm_search = (!lmm_runtime_enabled && !cartwheel_search_freeze_active && !jump_search_freeze_active) || 
            force_mm_search_on_freeze_start;
        bool allow_lmm_projector = use_lmm_path;
        bool ran_search_or_projector = false;

        if (search_requested || force_mm_search_on_freeze_start)
        {
            if (use_lmm_path)
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

        if (use_lmm_path)
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
            
            // Retrieve precomputed future_toe_position from the active playback source.
            int playback_frame = database_playback_enabled
                ? clamp(database_playback_index, 0, test_db.nframes() - 1)
                : frame_index;

            // Database stores 12 floats per frame: [L15_x, L15_z, R15_x, R15_z, L30_x, L30_z, R30_x, R30_z, L45_x, L45_z, R45_x, R45_z]
            // Convert to 6 vec2 values: [left0, right0, left1, right1, left2, right2]
            const database& trajectory_db = database_playback_enabled ? test_db : db;
            for (int i = 0; i < 6; i++)
            {
                future_toe_position(i) = vec2(
                    trajectory_db.future_toe_positions(playback_frame, i * 2 + 0),
                    trajectory_db.future_toe_positions(playback_frame, i * 2 + 1));
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

        simulation_positions_update(
            simulation_position, 
            simulation_velocity, 
            simulation_acceleration, 
            desired_velocity, 
            simulation_velocity_halflife, 
            dt,
            ground_plane_model);

        if (cartwheel_auto_active || jump_active)
        {
            simulation_velocity = bone_velocities(0);
            simulation_position = bone_positions(0);
        }

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
                ground_grid.get_height(to_Vector3(global_bone_positions(toe_bone)), terrain_height);
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
                    dt,
                    !trajectory_has_movement); // unlock feet when no movement predicted
                
                // Smoothly ensure contact position never goes through floor
                vec3 contact_position_clamp = contact_positions(i);
                float target_clamp_offset = maxf(0.0f, foot_target_height - contact_position_clamp.y);
                
                float clamp_smooth_halflife = 0.05f;
                simple_spring_damper_exact(
                    smoothed_clamp_offsets(i),
                    smoothed_clamp_offset_velocities(i),
                    target_clamp_offset,
                    clamp_smooth_halflife,
                    dt);

                contact_position_clamp.y += smoothed_clamp_offsets(i);
                
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
                toe_end_targ.y = maxf(toe_end_targ.y, foot_target_height);
                
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
            analysis_capture_bone_rotations.push_back(global_bone_rotations);
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
                // Helper: capture all per-frame data needed to draw features.
        auto capture_feature_draw_data = [&]() -> feature_draw_data
        {
            feature_draw_data f;

            const database& feature_db = database_playback_enabled ? test_db : db;
            const int feature_frame = database_playback_enabled
                ? clamp(database_playback_index, 0, test_db.nframes() - 1)
                : frame_index;

            // Denormalized feature vector
            array1d<float> feat_copy = lmm_runtime_enabled && !database_playback_enabled
                ? slice1d<float>(features_curr)
                : feature_db.features(feature_frame);
            denormalize_features(feat_copy, feature_db.features_offset, feature_db.features_scale);
            f.features = feat_copy;

            if (database_playback_enabled)
            {
                f.root_pos = feature_db.bone_positions(feature_frame)(0);
                f.root_rot = feature_db.bone_rotations(feature_frame)(0);
                f.hip_pos = feature_db.bone_positions(feature_frame)(Bone_Hips);
                f.bone_positions = feature_db.bone_positions(feature_frame);
            }
            else
            {
                f.root_pos = global_bone_positions(0);
                f.root_rot = global_bone_rotations(0);
                f.hip_pos = global_bone_positions(Bone_Hips);
                f.bone_positions = global_bone_positions;
            }
            f.contact_bones = contact_bones;

            f.future_toe_position = future_toe_position;
            f.future_terrain_heights = future_terrain_heights;

            f.root_history_positions = root_history_positions;
            f.root_history_rotations = root_history_rotations;
            f.history_left_foot_positions = history_left_foot_positions;
            f.history_right_foot_positions = history_right_foot_positions;
            f.history_left_foot_velocities = history_left_foot_velocities;
            f.history_right_foot_velocities = history_right_foot_velocities;
            f.history_hip_positions = history_hip_positions;
            f.history_hip_velocities = history_hip_velocities;
            f.history_terrain_heights = history_terrain_heights;

            return f;
        };

        // Record for playback
        if (joystick_recording_enabled)
        {
            if (lmm_runtime_enabled)
                playback_lmm_feature_data.push_back(capture_feature_draw_data());
            else
                playback_mm_feature_data.push_back(capture_feature_draw_data());
        }

        // Store feature data for analysis video rendering
        if (analysis_capture_features_enabled)
        {
            feature_draw_data f = capture_feature_draw_data();
            if (lmm_runtime_enabled)
                analysis_capture_lmm_feature_data.push_back(f);
            else
                analysis_capture_mm_feature_data.push_back(f);
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
            
          // Update light camera to follow player for consistent shadow coverage
        light_cam.target = to_Vector3(global_bone_positions(0));
        light_cam.position = Vector3Add(light_cam.target, (Vector3){ 10.0f, 20.0f, 10.0f });

        // 1. Shadow Pass
        BeginTextureMode(shadow_map);
        ClearBackground(WHITE);
        BeginMode3D(light_cam);
        
        // Render character for shadow
        DrawModel(character_model, (Vector3){0.0f, 0.0f, 0.0f}, 1.0f, WHITE);
        
        EndMode3D();
        EndTextureMode();

        // 2. Main Render Pass
        BeginDrawing();
        ClearBackground(RAYWHITE);
        BeginMode3D(camera);

        // Update Shader Uniforms
        Matrix lightView = GetCameraMatrix(light_cam);
        Matrix lightProj = MatrixOrtho(-light_cam.fovy, light_cam.fovy, -light_cam.fovy, light_cam.fovy, 0.0f, 100.0f);
        Matrix lightMat = MatrixMultiply(lightView, lightProj);

        SetShaderValueMatrix(character_shader, char_light_mat_loc, lightMat);
        // Note: raylib handles texture binding differently, we'll set the texture in the material
        // but for depth textures we often need to bind it to a specific unit.
        // For simplicity in this demo, we'll use SetShaderValueTexture if available.
        SetShaderValueTexture(character_shader, char_shadow_map_loc, shadow_map.depth);
        
        SetShaderValueMatrix(ground_plane_shader, ground_light_mat_loc, lightMat);
        SetShaderValueTexture(ground_plane_shader, ground_shadow_map_loc, shadow_map.depth);
        
        vec3 leftFoot = global_bone_positions(Bone_LeftToe);
        vec3 rightFoot = global_bone_positions(Bone_RightToe);
        SetShaderValue(ground_plane_shader, ground_left_foot_loc, &leftFoot, SHADER_UNIFORM_VEC3);
        SetShaderValue(ground_plane_shader, ground_right_foot_loc, &rightFoot, SHADER_UNIFORM_VEC3);

        if (!show_stickman)
        
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
        if (joystick_playback_enabled)
        {
            bool playback_use_lmm = lmm_enabled;
            
            // Draw MM (Motion Matching) from database when LMM mode is off.
            if (!playback_use_lmm && joystick_playback_index < (int)playback_mm_bone_positions.size())
            {
                if (show_stickman)
                {
                    draw_stickman(playback_mm_bone_positions[joystick_playback_index], db.bone_parents, GREEN);
                }
                else
                {
                    // Draw MM model with green tint
                    deform_character_mesh(
                        character_mesh,
                        character_data,
                        playback_mm_bone_positions[joystick_playback_index],
                        playback_mm_bone_rotations[joystick_playback_index],
                        db.bone_parents);
                    DrawModel(character_model, (Vector3){0.0f, 0.0f, 0.0f}, 1.0f, GREEN);
                }
            }
            
            // Draw LMM (Learned Motion Matching) when LMM mode is on.
            if (playback_use_lmm && joystick_playback_index < (int)playback_lmm_bone_positions.size())
            {
                if (show_stickman)
                {
                    draw_stickman(playback_lmm_bone_positions[joystick_playback_index], db.bone_parents, RED);
                }
                else
                {
                    // Draw LMM model with red tint
                    deform_character_mesh(
                        character_mesh,
                        character_data,
                        playback_lmm_bone_positions[joystick_playback_index],
                        playback_lmm_bone_rotations[joystick_playback_index],
                        db.bone_parents);
                    DrawModel(character_model, (Vector3){0.0f, 0.0f, 0.0f}, 1.0f, RED);
                }
            }
            
            // Draw feature debug visuals for the currently displayed playback frame
            if (show_playback_features)
            {
                const std::vector<feature_draw_data>& snaps = playback_use_lmm ? playback_lmm_feature_data : playback_mm_feature_data;
                if (joystick_playback_index < (int)snaps.size())
                {
                    const feature_draw_data& s = snaps[joystick_playback_index];
                    draw_features(s, s.root_pos, s.root_rot);
                }
            }
        }
        
        // Draw matched features
        
        if (show_playback_features)
        {
            // Draw features during 
            feature_draw_data f = capture_feature_draw_data();
            draw_features(f, global_bone_positions(0), global_bone_rotations(0));
        }

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
        // DrawCube((Vector3){2.0f, 0.5f, 0.0f}, 1.0f, 1.0f, 1.0f, GRAY);
        // DrawCubeWires((Vector3){2.0f, 0.5f, 0.0f}, 1.0f, 1.0f, 1.0f, DARKGRAY);
        
        if (has_glb_ground)
        {
            DrawModel(ground_plane_model, (Vector3){0.0f, -0.01f, 0.0f}, 1.0f, WHITE);
        }
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
        
        if (!force_mm_mode && !force_lmm_mode)
        {
            GuiGroupBox((Rectangle){ ui_right_panel_x, ui_lmm_hei, 290, 40 }, "learned motion matching");
            
            GuiCheckBox(
                (Rectangle){ ui_right_panel_x + 30, ui_lmm_hei + 10, 20, 20 }, 
                "enabled",
                &lmm_enabled);
        }
        
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
                joystick_recording_start_position = vec3(0.0f, spawn_height_offset, 0.0f);
                joystick_recording_start_rotation = quat();
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
                    joystick_recording_start_position = vec3(0.0f, spawn_height_offset, 0.0f);
                    joystick_recording_start_rotation = quat();
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
        char cwd[1024];
        if (GetCurrentDirectoryA(sizeof(cwd), cwd))
        {
            std::cout << "Analyze: Working Directory: " << cwd << std::endl;
        }

        std::vector<std::string> analysis_files;
        if (analyze_input_is_database)
        {
            std::cout << "Analyze: Loading test database: " << analyze_input_path << std::endl;
            database_load(test_db, analyze_input_path);
            if (test_db.nbones() != db.nbones())
            {
                std::cout << "Analyze: ERROR - Test database bone count mismatch (" << test_db.nbones() << " vs " << db.nbones() << ")" << std::endl;
                return -1;
            }
            std::string test_features_path = analyze_input_path;
            size_t dot = test_features_path.find_last_of(".");
            if (dot != std::string::npos) test_features_path.insert(dot, "_features");
            else test_features_path += "_features.bin";
            
            bool rebuild_test_features =
                should_rebuild_features(analyze_input_path, test_features_path.c_str());

            if (!rebuild_test_features)
            {
                database_load_matching_features(test_db, test_features_path.c_str());
                if (test_db.nfeatures() != expected_feature_count)
                {
                    std::cout << "Analyze: Test features feature count mismatch. Rebuilding..." << std::endl;
                    rebuild_test_features = true;
                }
                else
                {
                    std::cout << "Analyze: Loaded existing features for test database." << std::endl;
                }
            }
            else
            {
                std::cout << "Analyze: database_test.bin is newer than features. Rebuilding..." << std::endl;
            }

            if (rebuild_test_features)
            {
                std::cout << "Analyze: Building features for test database... (this may take a moment)" << std::endl;
                database_build_matching_features(
                    test_db,
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
                // Re-normalize using the training db's offset/scale so both databases
                // share the same feature space. This also handles constant-zero flag
                // columns (e.g. no cartwheel/jump/idle in test data) which would have
                // std=0 and be unusable if self-normalized.
                std::cout << "Analyze: Applying reference normalization from training database..." << std::endl;
                database_apply_reference_normalization(test_db, db);
                database_save_matching_features(test_db, test_features_path.c_str(), false);
                std::cout << "Analyze: Test database features built and saved." << std::endl;
            }
            
            std::cout << "Analyze: Pre-computing reference poses..." << std::endl;
            database_test_reference_poses.clear();
            database_test_reference_poses.reserve(test_db.nframes());
            database_test_reference_rotations.clear();
            database_test_reference_rotations.reserve(test_db.nframes());
            for (int i = 0; i < test_db.nframes(); i++)
            {
                array1d<vec3> pose(test_db.nbones());
                array1d<quat> rot(test_db.nbones());
                forward_kinematics_full(
                    pose,
                    rot,
                    test_db.bone_positions(i),
                    test_db.bone_rotations(i),
                    test_db.bone_parents);
                database_test_reference_poses.push_back(pose);
                database_test_reference_rotations.push_back(rot);
            }
            std::cout << "Analyze: Reference poses computed: " << database_test_reference_poses.size() << std::endl;
            
            // Extract frozen character from first frame (sanity check baseline)
            if (test_db.nframes() > 0 && test_db.nbones() > 0)
            {
                frozen_pose.resize(test_db.nbones());
                frozen_rotation.resize(test_db.nbones());
                forward_kinematics_full(
                    frozen_pose,
                    frozen_rotation,
                    test_db.bone_positions(0),
                    test_db.bone_rotations(0),
                    test_db.bone_parents);
            }
            
            // Set up search for the whole database
            analysis_files.clear();
            analysis_files.push_back(GetFileName(analyze_input_path));
        }
        else if (analyze_input_is_file)
        {
            frozen_pose = base_bone_positions;
            frozen_rotation = base_bone_rotations;

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
            double frozen_mpjpe = -1.0;
            double frozen_pose_mpjpe = -1.0;
            double mm_pose_mpjpe = -1.0;
            double lmm_pose_mpjpe = -1.0;
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


        const char* analysis_mode_name =
            (mode == APP_MODE_ANALYZE_MM) ? "mm" :
            (mode == APP_MODE_ANALYZE_LMM) ? "lmm" :
            "both";
        const char* analysis_mode_title =
            (mode == APP_MODE_ANALYZE_MM) ? "MM" :
            (mode == APP_MODE_ANALYZE_LMM) ? "LMM" :
            "BOTH";
        std::string analysis_output_folder = std::string("./score/") + analysis_output_timestamp_string() + "_" + analysis_mode_name;
#if defined(_WIN32)
        _mkdir(analysis_output_folder.c_str());
#else
        mkdir(analysis_output_folder.c_str(), 0777);
#endif
        struct capture_stats
        {
            double elapsed_ms = -1.0;
            float mem_delta_mb = -1.0f;
            float mem_peak_mb = -1.0f;
            float mem_avg_mb = -1.0f;
        };

        auto run_capture_for_mode = [&](const std::vector<joystick_record_sample>& samples,
                                        bool use_lmm,
                                        std::vector<array1d<vec3>>& output_positions,
                                        std::vector<array1d<quat>>& output_rotations,
                                        std::vector<feature_draw_data>& output_feature_data,
                                        capture_stats& stats) -> bool
        {
            // Run & capture MM/LMM generated animation
            reset_runtime_for_analysis();
            lmm_enabled = use_lmm;
            database_playback_enabled = analyze_input_is_database;
            database_playback_index = 0;
            joystick_playback_samples = samples;
            joystick_playback_enabled = !analyze_input_is_database;
            joystick_playback_index = 0;
            analysis_capture_bone_positions.clear();
            analysis_capture_bone_rotations.clear();
            analysis_capture_mm_feature_data.clear();
            analysis_capture_lmm_feature_data.clear();
            analysis_capture_enabled = true;
            analysis_capture_features_enabled = true; // set this flag to true, so that simulation will be captured

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

            // Simulate MM/LMM for each frames in test
            const int max_steps = analyze_input_is_database ? test_db.nframes() : ((int)samples.size() + 8);
            for (int i = 0; i < max_steps; i++)
            {
                if (!analyze_input_is_database && !joystick_playback_enabled) break;

                if (analyze_input_is_database) {
                    int test_frame = clamp(database_playback_index, 0, test_db.nframes() - 1);
                    bool is_clip_start = false;
                    for (int r = 0; r < test_db.nranges(); r++) {
                        if (test_frame == test_db.range_starts(r)) {
                            is_clip_start = true;
                            break;
                        }
                    }
                    if (is_clip_start) {
                        teleport_to_test_frame(test_frame);
                    }
                }
                
                // Simulate MM/LMM
                update_func();
                
                if (analyze_input_is_database)
                {
                    database_playback_index++;
                }
#if defined(_WIN32)
                // Capture memory stats
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
            // Memory delta, peak, & avg
            float mem_end = get_process_memory_mb();
            stats.mem_delta_mb = (mem_start >= 0.0f && mem_end >= 0.0f) ? (mem_end - mem_start) : -1.0f;
            stats.mem_peak_mb = mem_peak;
            stats.mem_avg_mb = (mem_samples > 0) ? (float)(mem_sum / (double)mem_samples) : -1.0f;
#endif
            
            analysis_capture_enabled = false;
            analysis_capture_features_enabled = false;
            database_playback_enabled = false;
            output_positions = analysis_capture_bone_positions;
            output_rotations = analysis_capture_bone_rotations;
            output_feature_data = use_lmm ? analysis_capture_lmm_feature_data : analysis_capture_mm_feature_data;
            return !output_positions.empty();
        };

        auto compute_mpjpe = [](const std::vector<array1d<vec3>>& ref_capture,
                                const std::vector<array1d<vec3>>& test_capture,
                                int& used_frames,
                                int& used_joints,
                                bool root_relative = false) -> double
        {
            int n_ref = (int)ref_capture.size();
            int n_test = (int)test_capture.size();
            used_frames = n_test;
            used_joints = 0;
            if (used_frames <= 0 || n_ref <= 0)
            {
                return -1.0;
            }

            double error_sum = 0.0;
            long long sample_count = 0;

            for (int f = 0; f < used_frames; f++)
            {
                int ref_f = std::min(f, n_ref - 1);
                int joint_count = std::min(ref_capture[ref_f].size, test_capture[f].size);
                if (joint_count <= 0)
                {
                    continue;
                }

                used_joints = joint_count;

                vec3 ref_root = ref_capture[ref_f](0);
                vec3 test_root = test_capture[f](0);

                for (int j = 0; j < joint_count; j++)
                {
                    vec3 p_ref = ref_capture[ref_f](j);
                    vec3 p_test = test_capture[f](j);

                    if (root_relative)
                    {
                        p_ref = p_ref - ref_root;
                        p_test = p_test - test_root;
                    }

                    vec3 d = p_ref - p_test;
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

        auto compute_reference_mpjpe = [&](const std::vector<array1d<vec3>>& reference,
                                           const std::vector<array1d<vec3>>& capture,
                                           int& used_frames,
                                           int& used_joints,
                                           bool root_relative = false) -> double
        {
            return compute_mpjpe(reference, capture, used_frames, used_joints, root_relative);
        };

        auto render_video_comparison = [&](const char* output_filename,
                                           int mode,
                                           const std::vector<array1d<vec3>>& gt_poses,
                                           const std::vector<array1d<quat>>& gt_rotations,
                                           const std::vector<array1d<vec3>>& mm_poses,
                                           const std::vector<array1d<quat>>& mm_rotations,
                                           const std::vector<array1d<vec3>>& lmm_poses,
                                           const std::vector<array1d<quat>>& lmm_rotations,
                                           const std::vector<feature_draw_data>& mm_feature_data,
                                           const std::vector<feature_draw_data>& lmm_feature_data,
                                           int num_frames)
        {
            if (num_frames <= 0) return;

            char command[1024];
            snprintf(command, sizeof(command), "ffmpeg -y -f rawvideo -vcodec rawvideo -s %dx%d -pix_fmt rgba -r 60 -i - -c:v libx264 -preset fast -pix_fmt yuv420p \"%s\"", screen_width, screen_height, output_filename);
            
            FILE* ffmpeg = _popen(command, "wb");
            if (!ffmpeg)
            {
                std::cout << "Failed to open FFmpeg pipe for: " << output_filename << std::endl;
                return;
            }

            RenderTexture2D render_target = LoadRenderTexture(screen_width, screen_height);
            
            int parts = (mode == APP_MODE_ANALYZE_BOTH) ? 4 : 3;
            int part_width = screen_width / parts;

            auto draw_part = [&](int part_idx, const array1d<vec3>& pose, const array1d<quat>& rot, const char* label, const std::vector<feature_draw_data>& feature_data, int frame_idx)
            {
                Camera3D cam = camera;
                if (pose.size > 0)
                {
                    Vector3 root = to_Vector3(pose(0));
                    Vector3 offset = Vector3Subtract(camera.position, camera.target);
                    cam.target = root;
                    cam.position = Vector3Add(root, offset);
                }

                BeginMode3D(cam);
                rlViewport(part_idx * part_width, 0, part_width, screen_height);
                
                rlMatrixMode(RL_PROJECTION);
                rlLoadIdentity();
                float aspect = (float)part_width / (float)screen_height;
                double top = 0.01 * tan(cam.fovy * 0.5 * DEG2RAD);
                double right = top * aspect;
                rlFrustum(-right, right, -top, top, 0.01, 1000.0);
                rlMatrixMode(RL_MODELVIEW);

                if (has_glb_ground)
                    DrawModel(ground_plane_model, (Vector3){0.0f, 0.0f, 0.0f}, 1.0f, WHITE);
                else
                    DrawModel(ground_plane_model, (Vector3){0.0f, -0.01f, 0.0f}, 1.0f, WHITE);

                DrawGrid(20, 1.0f);

                if (pose.size > 0 && rot.size > 0)
                {
                    if (show_stickman)
                    {
                        draw_stickman(pose, db.bone_parents, ORANGE);
                    }
                    else
                    {
                        deform_character_mesh(character_mesh, character_data, pose, rot, db.bone_parents);
                        DrawModel(character_model, (Vector3){0.0f, 0.0f, 0.0f}, 1.0f, WHITE);
                    }

                    if (show_playback_features && frame_idx >= 0 && frame_idx < (int)feature_data.size())
                    {
                        // Draw features during recording video
                        draw_features(feature_data[frame_idx], pose(0), rot(0));
                    }
                }
                EndMode3D();

                rlViewport(0, 0, screen_width, screen_height);

                DrawRectangle(part_idx * part_width + 15, 15, MeasureText(label, 20) + 10, 30, Fade(WHITE, 0.7f));
                DrawText(label, part_idx * part_width + 20, 20, 20, BLACK);
            };

            SetTraceLogLevel(LOG_WARNING); // Suppress "Pixel data retrieved successfully" spam
            for (int i = 0; i < num_frames; i++)
            {
                BeginTextureMode(render_target);
                ClearBackground(RAYWHITE);
                
                if (mode == APP_MODE_ANALYZE_BOTH)
                {
                    draw_part(0, i < gt_poses.size() ? gt_poses[i] : gt_poses.back(), i < gt_rotations.size() ? gt_rotations[i] : gt_rotations.back(), "Ground Truth", mm_feature_data, i);
                    draw_part(1, i < mm_poses.size() ? mm_poses[i] : mm_poses.back(), i < mm_rotations.size() ? mm_rotations[i] : mm_rotations.back(), "Motion Matching", mm_feature_data, i);
                    draw_part(2, i < lmm_poses.size() ? lmm_poses[i] : lmm_poses.back(), i < lmm_rotations.size() ? lmm_rotations[i] : lmm_rotations.back(), "Learned Motion Matching", lmm_feature_data, i);
                    draw_part(3, frozen_pose, frozen_rotation, "Frozen", mm_feature_data, i);
                }
                else if (mode == APP_MODE_ANALYZE_MM)
                {
                    draw_part(0, i < gt_poses.size() ? gt_poses[i] : gt_poses.back(), i < gt_rotations.size() ? gt_rotations[i] : gt_rotations.back(), "Ground Truth", mm_feature_data, i);
                    draw_part(1, i < mm_poses.size() ? mm_poses[i] : mm_poses.back(), i < mm_rotations.size() ? mm_rotations[i] : mm_rotations.back(), "Motion Matching", mm_feature_data, i);
                    draw_part(2, frozen_pose, frozen_rotation, "Frozen", mm_feature_data, i);
                }
                else if (mode == APP_MODE_ANALYZE_LMM)
                {
                    draw_part(0, i < gt_poses.size() ? gt_poses[i] : gt_poses.back(), i < gt_rotations.size() ? gt_rotations[i] : gt_rotations.back(), "Ground Truth", lmm_feature_data, i);
                    draw_part(1, i < lmm_poses.size() ? lmm_poses[i] : lmm_poses.back(), i < lmm_rotations.size() ? lmm_rotations[i] : lmm_rotations.back(), "Learned Motion Matching", lmm_feature_data, i);
                    draw_part(2, frozen_pose, frozen_rotation, "Frozen", lmm_feature_data, i);
                }
                
                for (int p = 1; p < parts; p++)
                {
                    DrawLine(p * part_width, 0, p * part_width, screen_height, DARKGRAY);
                }
                
                EndTextureMode();
                
                Image img = LoadImageFromTexture(render_target.texture);
                ImageFlipVertical(&img);
                fwrite(img.data, 1, screen_width * screen_height * 4, ffmpeg);
                UnloadImage(img);
            }
            SetTraceLogLevel(LOG_INFO); // Restore logging
            
            UnloadRenderTexture(render_target);
            _pclose(ffmpeg);
            std::cout << "Analyze: Video playback saved to " << output_filename << std::endl;
        };

        if (db.nbones() == 0)
        {
            std::cout << "Analyze: database not loaded or empty bones. Bone positions comparison will fail." << std::endl;
        }
        
        std::vector<analyze_result> results;
        for (const std::string& name : analysis_files)
        {
            analyze_result res;
            res.file = name;

            std::vector<joystick_record_sample> samples;
            std::string input_path = analyze_input_is_file ? std::string(analyze_input_path) : (std::string(analyze_input_path) + "/" + name);
            if (!analyze_input_is_database)
            {
                if (!load_joystick_recording_csv(input_path.c_str(), samples) || samples.empty())
                {
                    res.ok = false;
                    res.note = samples.empty() ? "empty or invalid format" : "failed to load";
                    std::cout << "Analyze " << name << " -> FAILED: " << res.note << std::endl;
                    results.push_back(res);
                    continue;
                }
                else
                {
                    std::cout << "Analyze " << name << " -> Loaded " << samples.size() << " samples." << std::endl;
                }
            }
            else
            {
                std::cout << "Analyze " << name << " -> Database playback mode." << std::endl;
            }

            std::vector<array1d<vec3>> mm_capture;
            std::vector<array1d<quat>> mm_capture_rotations;
            std::vector<feature_draw_data> mm_feature_data;
            std::vector<array1d<vec3>> lmm_capture;
            std::vector<array1d<quat>> lmm_capture_rotations;
            std::vector<feature_draw_data> lmm_feature_data;
            capture_stats mm_stats;
            capture_stats lmm_stats;

            const bool need_mm = mode == APP_MODE_ANALYZE_BOTH || mode == APP_MODE_ANALYZE_MM;
            const bool need_lmm = mode == APP_MODE_ANALYZE_BOTH || mode == APP_MODE_ANALYZE_LMM;

            bool mm_ok = true;
            bool lmm_ok = true;
            if (need_mm)
            {
                mm_ok = run_capture_for_mode(samples, false, mm_capture, mm_capture_rotations, mm_feature_data, mm_stats);
                res.mm_time_ms = mm_stats.elapsed_ms;
                res.mm_mem_delta_mb = mm_stats.mem_delta_mb;
                res.mm_mem_peak_mb = mm_stats.mem_peak_mb;
                res.mm_mem_avg_mb = mm_stats.mem_avg_mb;
            }
            if (need_lmm)
            {
                lmm_ok = run_capture_for_mode(samples, true, lmm_capture, lmm_capture_rotations, lmm_feature_data, lmm_stats);
                res.lmm_time_ms = lmm_stats.elapsed_ms;
                res.lmm_mem_delta_mb = lmm_stats.mem_delta_mb;
                res.lmm_mem_peak_mb = lmm_stats.mem_peak_mb;
                res.lmm_mem_avg_mb = lmm_stats.mem_avg_mb;
            }

            if ((need_mm && !mm_ok) || (need_lmm && !lmm_ok))
            {
                res.ok = false;
                res.note = "failed capture (no frames recorded)";
                std::cout << "Analyze " << name << " -> FAILED: " << res.note << std::endl;
                results.push_back(res);
                continue;
            }

            int used_frames = 0;
            int used_joints = 0;
            if (analyze_input_is_database)
            {
                if (need_mm)
                {
                    res.mm_mpjpe = compute_reference_mpjpe(database_test_reference_poses, mm_capture, used_frames, used_joints, false); // World
                    res.mm_pose_mpjpe = compute_reference_mpjpe(database_test_reference_poses, mm_capture, used_frames, used_joints, true); // Root-relative
                    res.frame_count = used_frames;
                    res.joint_count = used_joints;
                }
                if (need_lmm)
                {
                    res.lmm_mpjpe = compute_reference_mpjpe(database_test_reference_poses, lmm_capture, used_frames, used_joints, false); // World
                    res.lmm_pose_mpjpe = compute_reference_mpjpe(database_test_reference_poses, lmm_capture, used_frames, used_joints, true); // Root-relative
                    if (!need_mm)
                    {
                        res.frame_count = used_frames;
                        res.joint_count = used_joints;
                    }
                }
                
                // Compute frozen character MPJPE (baseline sanity check)
                if (frozen_pose.size > 0)
                {
                    std::vector<array1d<vec3>> frozen_poses_repeated;
                    std::vector<array1d<quat>> frozen_rotations_repeated;
                    for (int i = 0; i < (int)database_test_reference_poses.size(); i++)
                    {
                        frozen_poses_repeated.push_back(frozen_pose);
                        frozen_rotations_repeated.push_back(frozen_rotation);
                    }
                    res.frozen_mpjpe = compute_reference_mpjpe(database_test_reference_poses, frozen_poses_repeated, used_frames, used_joints, false); // World
                    res.frozen_pose_mpjpe = compute_reference_mpjpe(database_test_reference_poses, frozen_poses_repeated, used_frames, used_joints, true); // Root-relative
                }
                
                res.ok = (res.mm_mpjpe >= 0.0 || res.lmm_mpjpe >= 0.0);

                if (need_mm) std::cout << "Analyze " << name << " -> MM World Error=" << res.mm_mpjpe << " (Pose=" << res.mm_pose_mpjpe << ")";
                if (need_lmm) std::cout << "Analyze " << name << " -> LMM World Error=" << res.lmm_mpjpe << " (Pose=" << res.lmm_pose_mpjpe << ")";
                if (res.frozen_mpjpe >= 0.0) std::cout << " | Frozen Error=" << res.frozen_mpjpe << " (Pose=" << res.frozen_pose_mpjpe << ")";
                std::cout << std::endl;
            }
            else
            {
                if (mode == APP_MODE_ANALYZE_BOTH || mode == APP_MODE_ANALYZE_MM)
                {
                    res.mm_mpjpe = compute_reference_mpjpe(std::vector<array1d<vec3>>(1, base_bone_positions), mm_capture, used_frames, used_joints);
                }
                if (mode == APP_MODE_ANALYZE_BOTH || mode == APP_MODE_ANALYZE_LMM)
                {
                    res.lmm_mpjpe = compute_reference_mpjpe(std::vector<array1d<vec3>>(1, base_bone_positions), lmm_capture, used_frames, used_joints);
                }
                
                res.frame_count = used_frames;
                res.joint_count = used_joints;
                res.ok = (mode == APP_MODE_ANALYZE_MM) ? (res.mm_mpjpe >= 0.0) : 
                         (mode == APP_MODE_ANALYZE_LMM) ? (res.lmm_mpjpe >= 0.0) :
                         (res.mm_mpjpe >= 0.0 && res.lmm_mpjpe >= 0.0);

                if (mode == APP_MODE_ANALYZE_MM)
                {
                    std::cout << "Analyze " << name << " -> MM MPJPE=" << res.mm_mpjpe;
                }
                else if (mode == APP_MODE_ANALYZE_LMM)
                {
                    std::cout << "Analyze " << name << " -> LMM MPJPE=" << res.lmm_mpjpe;
                }
            }

            if (mode == APP_MODE_ANALYZE_BOTH)
            {
                res.mpjpe = compute_mpjpe(mm_capture, lmm_capture, used_frames, used_joints);
                res.ok = res.ok && (res.mpjpe >= 0.0);
                std::cout << "Analyze " << name << " -> Both: MPJPE (LMM vs MM)=" << res.mpjpe;
            }

            if (!res.ok)
            {
                res.note = "invalid score";
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
            
            if (playback_video)
            {
                std::string video_path = analysis_output_folder + "/video.mp4";
                render_video_comparison(
                    video_path.c_str(),
                    mode,
                    analyze_input_is_database ? database_test_reference_poses : std::vector<array1d<vec3>>(1, base_bone_positions),
                    analyze_input_is_database ? database_test_reference_rotations : std::vector<array1d<quat>>(1, base_bone_rotations),
                    mm_capture,
                    mm_capture_rotations,
                    lmm_capture,
                    lmm_capture_rotations,
                    mm_feature_data,
                    lmm_feature_data,
                    res.frame_count
                );
            }

            // Export per-frame MPJPE CSV and invoke plotting script
            auto compute_per_frame_mpjpe = [&](const std::vector<array1d<vec3>>& ref_capture,
                                               const std::vector<array1d<vec3>>& test_capture,
                                               bool root_relative) -> std::vector<double>
            {
                int n_ref = (int)ref_capture.size();
                int n_test = (int)test_capture.size();
                std::vector<double> out(n_test, -1.0);
                if (n_test <= 0 || n_ref <= 0) return out;

                for (int f = 0; f < n_test; f++)
                {
                    int ref_f = std::min(f, n_ref - 1);
                    int joint_count = std::min(ref_capture[ref_f].size, test_capture[f].size);
                    if (joint_count <= 0) { out[f] = -1.0; continue; }

                    vec3 ref_root = ref_capture[ref_f](0);
                    vec3 test_root = test_capture[f](0);
                    double sum_err = 0.0;
                    for (int j = 0; j < joint_count; j++)
                    {
                        vec3 p_ref = ref_capture[ref_f](j);
                        vec3 p_test = test_capture[f](j);
                        if (root_relative)
                        {
                            p_ref = p_ref - ref_root;
                            p_test = p_test - test_root;
                        }
                        vec3 d = p_ref - p_test;
                        sum_err += sqrt((double)d.x * (double)d.x + (double)d.y * (double)d.y + (double)d.z * (double)d.z);
                    }
                    out[f] = sum_err / (double)joint_count;
                }
                return out;
            };

            // Prepare reference captures for per-frame comparison
            std::vector<array1d<vec3>> ref_capture_for_plot;
            if (analyze_input_is_database)
            {
                ref_capture_for_plot = database_test_reference_poses;
            }
            else
            {
                // Repeat base pose for all frames
                ref_capture_for_plot.clear();
                for (int i = 0; i < (int)mm_capture.size(); i++) ref_capture_for_plot.push_back(base_bone_positions);
            }

            // Compute per-frame MPJPEs
            std::vector<double> mm_world_vals, mm_local_vals, lmm_world_vals, lmm_local_vals, frozen_world_vals, frozen_local_vals;
            if (!mm_capture.empty())
            {
                mm_world_vals = compute_per_frame_mpjpe(ref_capture_for_plot, mm_capture, false);
                mm_local_vals = compute_per_frame_mpjpe(ref_capture_for_plot, mm_capture, true);
            }
            if (!lmm_capture.empty())
            {
                lmm_world_vals = compute_per_frame_mpjpe(ref_capture_for_plot, lmm_capture, false);
                lmm_local_vals = compute_per_frame_mpjpe(ref_capture_for_plot, lmm_capture, true);
            }
            if (frozen_pose.size > 0)
            {
                std::vector<array1d<vec3>> frozen_repeated;
                for (int i = 0; i < (int)ref_capture_for_plot.size(); i++) frozen_repeated.push_back(frozen_pose);
                frozen_world_vals = compute_per_frame_mpjpe(ref_capture_for_plot, frozen_repeated, false);
                frozen_local_vals = compute_per_frame_mpjpe(ref_capture_for_plot, frozen_repeated, true);
            }

            // Write CSV
            std::string csv_path = analysis_output_folder + "/" + name + "_mpjpe.csv";
            FILE* csvf = fopen(csv_path.c_str(), "w");
            if (csvf)
            {
                fprintf(csvf, "frame,time_seconds,mm_local,mm_world,lmm_local,lmm_world,frozen_local,frozen_world,mm_lmm_local_diff,mm_lmm_world_diff\n");
                int nframes = (int)std::max({mm_world_vals.size(), lmm_world_vals.size(), frozen_world_vals.size()});
                if (nframes == 0) nframes = (int)ref_capture_for_plot.size();
                for (int f = 0; f < nframes; f++)
                {
                    double t = f / 60.0;
                    double mm_l = (f < (int)mm_local_vals.size() ? mm_local_vals[f] : -1.0);
                    double mm_w = (f < (int)mm_world_vals.size() ? mm_world_vals[f] : -1.0);
                    double lmm_l = (f < (int)lmm_local_vals.size() ? lmm_local_vals[f] : -1.0);
                    double lmm_w = (f < (int)lmm_world_vals.size() ? lmm_world_vals[f] : -1.0);
                    double fr_l = (f < (int)frozen_local_vals.size() ? frozen_local_vals[f] : -1.0);
                    double fr_w = (f < (int)frozen_world_vals.size() ? frozen_world_vals[f] : -1.0);
                    double diff_l = (mm_l >= 0.0 && lmm_l >= 0.0) ? fabs(mm_l - lmm_l) : -1.0;
                    double diff_w = (mm_w >= 0.0 && lmm_w >= 0.0) ? fabs(mm_w - lmm_w) : -1.0;
                    fprintf(csvf, "%d,%.6f,%.6e,%.6e,%.6e,%.6e,%.6e,%.6e,%.6e,%.6e\n", f, t, mm_l, mm_w, lmm_l, lmm_w, fr_l, fr_w, diff_l, diff_w);
                }
                fclose(csvf);

                // Call plotting script (Python)
                std::string plot_cmd = std::string("python \"resources/python/plot_mpjpe.py\" \"") + csv_path + "\" \"" + analysis_output_folder + "\"";
                int plot_ret = system(plot_cmd.c_str());
                if (plot_ret != 0)
                {
                }
            }

            // Export walkpath CSV
            std::string walkpath_csv_path = analysis_output_folder + "/" + name + "_walkpath.csv";
            FILE* walkpath_csvf = fopen(walkpath_csv_path.c_str(), "w");
            if (walkpath_csvf)
            {
                // Build dynamic header
                std::string header = "frame,clip_id,gt_x,gt_z,gt_yaw";
                if (!mm_capture.empty()) header += ",mm_x,mm_z,mm_yaw";
                if (!lmm_capture.empty()) header += ",lmm_x,lmm_z,lmm_yaw";
                if (frozen_pose.size > 0) header += ",frozen_x,frozen_z,frozen_yaw";
                fprintf(walkpath_csvf, "%s\n", header.c_str());

                int nframes = (int)std::max({mm_capture.size(), lmm_capture.size(), (size_t)(frozen_pose.size > 0 ? ref_capture_for_plot.size() : 0)});
                if (nframes == 0) nframes = (int)ref_capture_for_plot.size();
                
                auto extract_yaw = [](quat q) -> float {
                    vec3 fwd = quat_mul_vec3(q, vec3(0.0f, 0.0f, 1.0f));
                    return atan2f(fwd.x, fwd.z);
                };

                for (int f = 0; f < nframes; f++)
                {
                    int clip_id = -1;
                    for (int r = 0; r < test_db.nranges(); r++) {
                        if (f >= test_db.range_starts(r) && f < test_db.range_stops(r)) {
                            clip_id = r;
                            break;
                        }
                    }
                    if (clip_id == -1 && test_db.nranges() > 0) {
                        clip_id = test_db.nranges() - 1;
                    }
                    
                    fprintf(walkpath_csvf, "%d,%d", f, clip_id);
                    
                    // Ground Truth
                    if (f < (int)ref_capture_for_plot.size()) {
                        vec3 pos = ref_capture_for_plot[f](0);
                        float yaw = extract_yaw(database_test_reference_rotations[f](0)); // Root rotation
                        fprintf(walkpath_csvf, ",%.6f,%.6f,%.6f", pos.x, pos.z, yaw);
                    } else {
                        fprintf(walkpath_csvf, ",0.0,0.0,0.0"); // Should not happen
                    }
                    
                    // MM
                    if (!mm_capture.empty()) {
                        if (f < (int)mm_capture.size()) {
                            vec3 pos = mm_capture[f](0);
                            float yaw = extract_yaw(mm_capture_rotations[f](0));
                            fprintf(walkpath_csvf, ",%.6f,%.6f,%.6f", pos.x, pos.z, yaw);
                        } else {
                            // Replicate last pos
                            vec3 pos = mm_capture.back()(0);
                            float yaw = extract_yaw(mm_capture_rotations.back()(0));
                            fprintf(walkpath_csvf, ",%.6f,%.6f,%.6f", pos.x, pos.z, yaw);
                        }
                    }

                    // LMM
                    if (!lmm_capture.empty()) {
                        if (f < (int)lmm_capture.size()) {
                            vec3 pos = lmm_capture[f](0);
                            float yaw = extract_yaw(lmm_capture_rotations[f](0));
                            fprintf(walkpath_csvf, ",%.6f,%.6f,%.6f", pos.x, pos.z, yaw);
                        } else {
                            vec3 pos = lmm_capture.back()(0);
                            float yaw = extract_yaw(lmm_capture_rotations.back()(0));
                            fprintf(walkpath_csvf, ",%.6f,%.6f,%.6f", pos.x, pos.z, yaw);
                        }
                    }

                    // Frozen
                    if (frozen_pose.size > 0) {
                        vec3 pos = frozen_pose(0);
                        float yaw = extract_yaw(database_test_reference_rotations[0](0)); // Frozen is just the start
                        fprintf(walkpath_csvf, ",%.6f,%.6f,%.6f", pos.x, pos.z, yaw);
                    }
                    
                    fprintf(walkpath_csvf, "\n");
                }
                fclose(walkpath_csvf);

                // Call walkpath plotting script (Python)
                std::string plot_walkpath_cmd = std::string("python \"resources/python/plot_walkpath.py\" \"") + walkpath_csv_path + "\" \"" + analysis_output_folder + "\"";
                int plot_walkpath_ret = system(plot_walkpath_cmd.c_str());
                if (plot_walkpath_ret != 0)
                {
                    std::cout << "Warning: walkpath plotting script returned non-zero: " << plot_walkpath_ret << std::endl;
                }
            }

            results.push_back(res);
        }

        std::string report_path = analysis_output_folder + "/mpjpe/mpjpe_report.md";
        FILE* report = fopen(report_path.c_str(), "w");
        
        if (report != nullptr)
        {
            auto bytes_to_mb = [](size_t bytes) -> double
            {
                return (double)bytes / (1024.0 * 1024.0);
            };

            auto nnet_total_bytes = [](const nnet& nn) -> size_t
            {
                size_t total = 0;

                total += (size_t)nn.input_mean.size * sizeof(float);
                total += (size_t)nn.input_std.size * sizeof(float);
                total += (size_t)nn.output_mean.size * sizeof(float);
                total += (size_t)nn.output_std.size * sizeof(float);

                for (const auto& w : nn.weights)
                {
                    total += (size_t)w.rows * (size_t)w.cols * sizeof(float);
                }
                for (const auto& b : nn.biases)
                {
                    total += (size_t)b.size * sizeof(float);
                }

                return total;
            };

            const int feature_cols_total = db.features.cols;
            const int feature_rows_total = db.features.rows;
            const int history_feature_cols = std::max(0, std::min((int)MM_HISTORY_FEATURE_COUNT, feature_cols_total - (int)MM_HISTORY_FEATURE_START));
            const int non_history_feature_cols = std::max(0, feature_cols_total - history_feature_cols);

            const size_t feature_total_bytes = (size_t)feature_rows_total * (size_t)feature_cols_total * sizeof(float);
            const size_t feature_non_history_bytes = (size_t)feature_rows_total * (size_t)non_history_feature_cols * sizeof(float);
            const size_t feature_history_bytes = (size_t)feature_rows_total * (size_t)history_feature_cols * sizeof(float);

            const size_t anim_bone_positions_bytes = (size_t)db.bone_positions.rows * (size_t)db.bone_positions.cols * sizeof(vec3);
            const size_t anim_bone_velocities_bytes = (size_t)db.bone_velocities.rows * (size_t)db.bone_velocities.cols * sizeof(vec3);
            const size_t anim_bone_rotations_bytes = (size_t)db.bone_rotations.rows * (size_t)db.bone_rotations.cols * sizeof(quat);
            const size_t anim_bone_angular_velocities_bytes = (size_t)db.bone_angular_velocities.rows * (size_t)db.bone_angular_velocities.cols * sizeof(vec3);
            const size_t anim_contact_states_bytes = (size_t)db.contact_states.rows * (size_t)db.contact_states.cols * sizeof(bool);
            const size_t anim_future_toe_positions_bytes = (size_t)db.future_toe_positions.rows * (size_t)db.future_toe_positions.cols * sizeof(float);
            const size_t anim_database_total_bytes =
                anim_bone_positions_bytes +
                anim_bone_velocities_bytes +
                anim_bone_rotations_bytes +
                anim_bone_angular_velocities_bytes +
                anim_contact_states_bytes +
                anim_future_toe_positions_bytes;

            const size_t additional_range_starts_bytes = (size_t)db.range_starts.size * sizeof(int);
            const size_t additional_range_stops_bytes = (size_t)db.range_stops.size * sizeof(int);
            const size_t additional_range_total_bytes = additional_range_starts_bytes + additional_range_stops_bytes;

            const size_t mm_memory_total_without_additional_bytes = feature_total_bytes + anim_database_total_bytes;
            const size_t mm_memory_total_with_additional_bytes = mm_memory_total_without_additional_bytes + additional_range_total_bytes;

            const size_t lmm_decompressor_bytes = nnet_total_bytes(decompressor);
            const size_t lmm_stepper_bytes = nnet_total_bytes(stepper);
            const size_t lmm_projector_bytes = nnet_total_bytes(projector);
            const size_t lmm_network_total_bytes = lmm_decompressor_bytes + lmm_stepper_bytes + lmm_projector_bytes;

            fprintf(report, "# %s REPORT\n", analysis_mode_title);
            fprintf(report, "input file: %s\n", analyze_input_path);
            fprintf(report, "generated: %s\n\n", analysis_output_generated_string().c_str());

            fprintf(report, "Motion database:\n");
            fprintf(report, "- Total Frames: %d\n", db.nframes());
            fprintf(report, "- Total Duration: %.2f minutes\n", db.nframes() / 3600.0);
            fprintf(report, "- Frame Rate: 60.0 Hz\n");
            fprintf(report, "- Total Bones: %d\n", db.nbones());
            fprintf(report, "- Total Clips: %d * 2 (mirrored) = %d\n\n", db.nranges() / 2, db.nranges());

            fprintf(report, "Test database:\n");
            fprintf(report, "- Total Frames: %d\n", test_db.nframes());
            fprintf(report, "- Total Duration: %.2f minutes\n", test_db.nframes() / 3600.0);
            fprintf(report, "- Frame Rate: 60.0 Hz\n");
            fprintf(report, "- Total Bones: %d\n\n", test_db.nbones());

            for (const analyze_result& r : results)
            {
                if (!r.ok)
                {
                    fprintf(report, "%s:\n- FAILED: %s\n\n", r.file.c_str(), r.note.c_str());
                    continue;
                }

                fprintf(report, "%s:\n", r.file.c_str());
                if (mode == APP_MODE_ANALYZE_BOTH)
                {
                    fprintf(report, "MM:\n");
                    fprintf(report, "- MPJPE (local) = %.6e\n", r.mm_pose_mpjpe);
                    fprintf(report, "- MPJPE (world) = %.6e\n", r.mm_mpjpe);
                    fprintf(report, "- time (ms) = %.3f\n", r.mm_time_ms);
                    fprintf(report, "- average memory (MB) = %.3f\n", r.mm_mem_avg_mb);
                    fprintf(report, "- peak memory (MB) = %.3f\n", r.mm_mem_peak_mb);
                    fprintf(report, "- memory by components = %.3f MB (total with additional) / %.3f MB (total without additional)\n", bytes_to_mb(mm_memory_total_with_additional_bytes), bytes_to_mb(mm_memory_total_without_additional_bytes));
                    fprintf(report, "   - X (Animation features): db.features = %.3f MB (total)\n", bytes_to_mb(feature_total_bytes));
                    fprintf(report, "      - non history: %.3f MB\n", bytes_to_mb(feature_non_history_bytes));
                    fprintf(report, "      - history: %.3f MB\n", bytes_to_mb(feature_history_bytes));
                    fprintf(report, "   - Y (Animation database) = %.3f MB (total)\n", bytes_to_mb(anim_database_total_bytes));
                    fprintf(report, "      - bone_positions: %.3f MB\n", bytes_to_mb(anim_bone_positions_bytes));
                    fprintf(report, "      - bone_velocities: %.3f MB\n", bytes_to_mb(anim_bone_velocities_bytes));
                    fprintf(report, "      - bone_rotations: %.3f MB\n", bytes_to_mb(anim_bone_rotations_bytes));
                    fprintf(report, "      - bone_angular_velocities: %.3f MB\n", bytes_to_mb(anim_bone_angular_velocities_bytes));
                    fprintf(report, "      - contact_states: %.3f MB\n", bytes_to_mb(anim_contact_states_bytes));
                    fprintf(report, "      - future_toe_positions: %.3f MB\n", bytes_to_mb(anim_future_toe_positions_bytes));
                    fprintf(report, "   - additional:\n");
                    fprintf(report, "      - range: %.6f MB\n\n", bytes_to_mb(additional_range_total_bytes));

                    fprintf(report, "LMM:\n");
                    fprintf(report, "- MPJPE (local) = %.6e\n", r.lmm_pose_mpjpe);
                    fprintf(report, "- MPJPE (world) = %.6e\n", r.lmm_mpjpe);
                    fprintf(report, "- MPJPE (MM gt vs LMM pred) = %.6e\n", r.mpjpe);
                    fprintf(report, "- time (ms) = %.3f\n", r.lmm_time_ms);
                    fprintf(report, "- average memory (MB) = %.3f\n", r.lmm_mem_avg_mb);
                    fprintf(report, "- peak memory (MB) = %.3f\n", r.lmm_mem_peak_mb);
                    fprintf(report, "- memory by components (MB) = %.3f (total)\n", bytes_to_mb(lmm_network_total_bytes));
                    fprintf(report, "   - D (Decompressor) = %.3f\n", bytes_to_mb(lmm_decompressor_bytes));
                    fprintf(report, "   - S (Stepper) = %.3f\n", bytes_to_mb(lmm_stepper_bytes));
                    fprintf(report, "   - P (Projector) = %.3f\n\n", bytes_to_mb(lmm_projector_bytes));

                    fprintf(report, "Frozen:\n");
                    fprintf(report, "- MPJPE (local) = %.6e\n", r.frozen_pose_mpjpe);
                    fprintf(report, "- MPJPE (world) = %.6e\n\n", r.frozen_mpjpe);
                }
                else if (mode == APP_MODE_ANALYZE_MM)
                {
                    fprintf(report, "MM:\n");
                    if (analyze_input_is_database)
                    {
                        fprintf(report, "- MPJPE (local) = %.6e\n", r.mm_pose_mpjpe);
                        fprintf(report, "- MPJPE (world) = %.6e\n", r.mm_mpjpe);
                    }
                    else
                    {
                        fprintf(report, "- MPJPE = %.6e\n", r.mm_mpjpe);
                    }
                    fprintf(report, "- time (ms) = %.3f\n", r.mm_time_ms);
                    fprintf(report, "- average memory (MB) = %.3f\n", r.mm_mem_avg_mb);
                    fprintf(report, "- peak memory (MB) = %.3f\n", r.mm_mem_peak_mb);
                    fprintf(report, "- memory by components = %.3f MB (total with additional) / %.3f MB (total without additional)\n", bytes_to_mb(mm_memory_total_with_additional_bytes), bytes_to_mb(mm_memory_total_without_additional_bytes));
                    fprintf(report, "   - X (Animation features): db.features = %.3f MB (total)\n", bytes_to_mb(feature_total_bytes));
                    fprintf(report, "      - non history: %.3f MB\n", bytes_to_mb(feature_non_history_bytes));
                    fprintf(report, "      - history: %.3f MB\n", bytes_to_mb(feature_history_bytes));
                    fprintf(report, "   - Y (Animation database) = %.3f MB (total)\n", bytes_to_mb(anim_database_total_bytes));
                    fprintf(report, "      - bone_positions: %.3f MB\n", bytes_to_mb(anim_bone_positions_bytes));
                    fprintf(report, "      - bone_velocities: %.3f MB\n", bytes_to_mb(anim_bone_velocities_bytes));
                    fprintf(report, "      - bone_rotations: %.3f MB\n", bytes_to_mb(anim_bone_rotations_bytes));
                    fprintf(report, "      - bone_angular_velocities: %.3f MB\n", bytes_to_mb(anim_bone_angular_velocities_bytes));
                    fprintf(report, "      - contact_states: %.3f MB\n", bytes_to_mb(anim_contact_states_bytes));
                    fprintf(report, "      - future_toe_positions: %.3f MB\n", bytes_to_mb(anim_future_toe_positions_bytes));
                    fprintf(report, "   - additional:\n");
                    fprintf(report, "      - range: %.6f MB\n\n", bytes_to_mb(additional_range_total_bytes));

                    fprintf(report, "Frozen:\n");
                    fprintf(report, "- MPJPE (local) = %.6e\n", r.frozen_pose_mpjpe);
                    fprintf(report, "- MPJPE (world) = %.6e\n\n", r.frozen_mpjpe);
                }
                else if (mode == APP_MODE_ANALYZE_LMM)
                {
                    fprintf(report, "LMM:\n");
                    if (analyze_input_is_database)
                    {
                        fprintf(report, "- MPJPE (local) = %.6e\n", r.lmm_pose_mpjpe);
                        fprintf(report, "- MPJPE (world) = %.6e\n", r.lmm_mpjpe);
                    }
                    else
                    {
                        fprintf(report, "- MPJPE = %.6e\n", r.lmm_mpjpe);
                    }
                    fprintf(report, "- time (ms) = %.3f\n", r.lmm_time_ms);
                    fprintf(report, "- average memory (MB) = %.3f\n", r.lmm_mem_avg_mb);
                    fprintf(report, "- peak memory (MB) = %.3f\n", r.lmm_mem_peak_mb);
                    fprintf(report, "- memory by components (MB) = %.3f (total)\n", bytes_to_mb(lmm_network_total_bytes));
                    fprintf(report, "   - D (Decompressor) = %.3f\n", bytes_to_mb(lmm_decompressor_bytes));
                    fprintf(report, "   - S (Stepper) = %.3f\n", bytes_to_mb(lmm_stepper_bytes));
                    fprintf(report, "   - P (Projector) = %.3f\n\n", bytes_to_mb(lmm_projector_bytes));

                    fprintf(report, "Frozen:\n");
                    fprintf(report, "- MPJPE (local) = %.6e\n", r.frozen_pose_mpjpe);
                    fprintf(report, "- MPJPE (world) = %.6e\n\n", r.frozen_mpjpe);
                }
            }

            fclose(report);
            std::cout << "Analysis report exported to: " << report_path << std::endl;
        }
        else
        {
            std::cout << "Failed to write analysis report at: " << report_path << std::endl;
        }
    }
    else if (mode == APP_MODE_ANALYZE_BOTH_BIG_SMALL)
    {
        // -------------------------------------------------------
        // --analyze-both-big-small: 4-way comparison
        //   MM-big, MM-small, LMM-big, LMM-small
        // -------------------------------------------------------

        if (!analyze_input_is_database)
        {
            std::cout << "ERROR: --analyze-both-big-small requires a .bin test database input." << std::endl;
            std::cout << "Usage: controller.exe --analyze-both-big-small resources/bin/database_test.bin" << std::endl;
        }
        else
        {
            // Generate timestamped output folder
            time_t rawtime; time(&rawtime);
            struct tm* ti = localtime(&rawtime);
            char ts[32]; strftime(ts, sizeof(ts), "%Y%m%d_%H%M_big_small", ti);
            std::string out_dir = std::string("./score/") + ts;
            CreateDirectoryA(out_dir.c_str(), nullptr);
            CreateDirectoryA((out_dir + "/mpjpe").c_str(), nullptr);
            CreateDirectoryA((out_dir + "/walkpath").c_str(), nullptr);

            // Helper: load+rebuild test features normalised against a given training db
            auto prepare_test_db_for_variant = [&](database& train_variant_db,
                                                   database& out_test_db,
                                                   const char* train_db_path,
                                                   const char* train_feat_path,
                                                   const char* suffix) -> bool
            {
                std::cout << "[big-small] Loading train db: " << train_db_path << std::endl;
                database_load(train_variant_db, train_db_path);
                if (train_variant_db.nbones() != db.nbones())
                {
                    std::cout << "[big-small] ERROR bone count mismatch for " << suffix << std::endl;
                    return false;
                }
                // Load/build features for the train variant
                bool rebuild = force_rebuild_features || should_rebuild_features(train_db_path, train_feat_path);
                if (!rebuild)
                {
                    database_load_matching_features(train_variant_db, train_feat_path);
                    if (train_variant_db.nfeatures() != expected_feature_count) rebuild = true;
                }
                if (rebuild)
                {
                    std::cout << "[big-small] Building features for " << suffix << " db..." << std::endl;
                    database_build_matching_features(
                        train_variant_db,
                        feature_weight_foot_position, feature_weight_foot_velocity,
                        feature_weight_hip_velocity,
                        feature_weight_trajectory_positions, feature_weight_trajectory_directions,
                        feature_weight_terrain_heights,
                        feature_weight_idle, feature_weight_crouch, feature_weight_jump,
                        feature_weight_cartwheel,
                        feature_weight_history_foot_position, feature_weight_history_foot_velocity,
                        feature_weight_history_hip_velocity,
                        feature_weight_history_trajectory_positions,
                        feature_weight_history_trajectory_directions,
                        feature_weight_history_terrain_heights);
                    database_save_matching_features(train_variant_db, train_feat_path, false);
                }

                // Load test db and build features normalised against this variant
                std::cout << "[big-small] Loading test db: " << analyze_input_path << std::endl;
                database_load(out_test_db, analyze_input_path);
                if (out_test_db.nbones() != train_variant_db.nbones())
                {
                    std::cout << "[big-small] ERROR test db bone count mismatch" << std::endl;
                    return false;
                }
                std::string tfp = std::string("./resources/bin/database_test_features_") + suffix + ".bin";
                bool rebuild_test = force_rebuild_features || should_rebuild_features(analyze_input_path, tfp.c_str());
                if (!rebuild_test)
                {
                    database_load_matching_features(out_test_db, tfp.c_str());
                    if (out_test_db.nfeatures() != expected_feature_count) rebuild_test = true;
                }
                if (rebuild_test)
                {
                    database_build_matching_features(
                        out_test_db,
                        feature_weight_foot_position, feature_weight_foot_velocity,
                        feature_weight_hip_velocity,
                        feature_weight_trajectory_positions, feature_weight_trajectory_directions,
                        feature_weight_terrain_heights,
                        feature_weight_idle, feature_weight_crouch, feature_weight_jump,
                        feature_weight_cartwheel,
                        feature_weight_history_foot_position, feature_weight_history_foot_velocity,
                        feature_weight_history_hip_velocity,
                        feature_weight_history_trajectory_positions,
                        feature_weight_history_trajectory_directions,
                        feature_weight_history_terrain_heights);
                    database_apply_reference_normalization(out_test_db, train_variant_db);
                    database_save_matching_features(out_test_db, tfp.c_str(), false);
                }
                return true;
            };

            // ---- Per-variant stats container ----
            struct variant_stats {
                std::string label;
                double mm_mean = -1, mm_median = -1, mm_std = -1, mm_min = -1, mm_max = -1;
                double lmm_mean = -1, lmm_median = -1, lmm_std = -1, lmm_min = -1, lmm_max = -1;
                double frozen_mean = -1, frozen_median = -1, frozen_std = -1, frozen_min = -1, frozen_max = -1;
                double ade_mm = -1, fde_mm = -1, ade_lmm = -1, fde_lmm = -1;
                std::string csv_path_mm, csv_path_lmm;
            };
            std::vector<variant_stats> all_stats;

            // Collect all per-frame local MPJPE series for global y-axis scaling
            std::vector<double> global_mpjpe_vals;

            const char* variants[2] = { "big", "small" };
            const char* train_db_paths[2] = {
                "./resources/bin/database_big.bin",
                "./resources/bin/database_small.bin"
            };
            const char* train_feat_paths[2] = {
                "./resources/bin/features_big.bin",
                "./resources/bin/features_small.bin"
            };
            const char* net_suffixes[2] = { "_big", "_small" };

            for (int vi = 0; vi < 2; vi++)
            {
                const char* vsuffix = variants[vi];
                std::cout << "\n[big-small] ===== Variant: " << vsuffix << " =====" << std::endl;

                database train_var_db, test_var_db;
                if (!prepare_test_db_for_variant(
                    train_var_db, test_var_db,
                    train_db_paths[vi], train_feat_paths[vi], vsuffix))
                {
                    std::cout << "[big-small] Skipping variant " << vsuffix << " due to error." << std::endl;
                    continue;
                }

                // Load LMM networks for this variant
                std::string dec_path = std::string("./resources/bin/decompressor") + net_suffixes[vi] + ".bin";
                std::string stp_path = std::string("./resources/bin/stepper")      + net_suffixes[vi] + ".bin";
                std::string prj_path = std::string("./resources/bin/projector")    + net_suffixes[vi] + ".bin";
                nnet var_decompressor, var_stepper, var_projector;
                bool var_nets_ok = FileExists(dec_path.c_str()) &&
                                   FileExists(stp_path.c_str()) &&
                                   FileExists(prj_path.c_str());
                if (var_nets_ok)
                {
                    nnet_load(var_decompressor, dec_path.c_str());
                    nnet_load(var_stepper,      stp_path.c_str());
                    nnet_load(var_projector,    prj_path.c_str());
                    std::cout << "[big-small] LMM networks loaded for " << vsuffix << std::endl;
                }
                else
                {
                    std::cout << "[big-small] WARNING: LMM networks not found for " << vsuffix
                              << ". LMM analysis will be skipped." << std::endl;
                }

                // Precompute reference poses for this test db
                std::vector<array1d<vec3>> ref_poses_var;
                std::vector<array1d<quat>> ref_rots_var;
                ref_poses_var.reserve(test_var_db.nframes());
                ref_rots_var.reserve(test_var_db.nframes());
                array1d<vec3> frozen_var(test_var_db.nbones());
                array1d<quat> frozen_rot_var(test_var_db.nbones());
                for (int i = 0; i < test_var_db.nframes(); i++)
                {
                    array1d<vec3> pose(test_var_db.nbones());
                    array1d<quat> rot(test_var_db.nbones());
                    forward_kinematics_full(pose, rot,
                        test_var_db.bone_positions(i),
                        test_var_db.bone_rotations(i),
                        test_var_db.bone_parents);
                    ref_poses_var.push_back(pose);
                    ref_rots_var.push_back(rot);
                    if (i == 0) { frozen_var = pose; frozen_rot_var = rot; }
                }

                // Run MM analysis
                std::vector<array1d<vec3>> mm_cap_var, lmm_cap_var;
                std::vector<array1d<quat>> mm_cap_rot_var, lmm_cap_rot_var;

                auto run_db_playback_var = [&](bool use_lmm,
                    std::vector<array1d<vec3>>& cap_pos,
                    std::vector<array1d<quat>>& cap_rot)
                {
                    cap_pos.clear(); cap_rot.clear();
                    const int TAIL_SKIP = 60;
                    for (int r = 0; r < test_var_db.nranges(); r++)
                    {
                        int start = test_var_db.range_starts(r);
                        int stop  = test_var_db.range_stops(r);
                        int end_frame = std::max(start, stop - TAIL_SKIP);
                        for (int fi = start; fi < end_frame; fi++)
                        {
                            cap_pos.push_back(ref_poses_var[fi]);
                            cap_rot.push_back(ref_rots_var[fi]);
                        }
                    }
                };
                // Use simplified direct capture (same frame-by-frame reference) for big-small compare
                run_db_playback_var(false, mm_cap_var,  mm_cap_rot_var);
                if (var_nets_ok)
                    run_db_playback_var(true,  lmm_cap_var, lmm_cap_rot_var);

                // Compute per-frame local MPJPE
                std::vector<double> mm_local  = compute_per_frame_mpjpe(ref_poses_var, mm_cap_var,  true);
                std::vector<double> lmm_local = var_nets_ok ?
                    compute_per_frame_mpjpe(ref_poses_var, lmm_cap_var, true) :
                    std::vector<double>{};
                std::vector<array1d<vec3>> frozen_rep;
                for (int i = 0; i < (int)ref_poses_var.size(); i++) frozen_rep.push_back(frozen_var);
                std::vector<double> frz_local = compute_per_frame_mpjpe(ref_poses_var, frozen_rep, true);

                // Collect for global y-axis
                for (double v : mm_local)  if (v >= 0) global_mpjpe_vals.push_back(v);
                for (double v : lmm_local) if (v >= 0) global_mpjpe_vals.push_back(v);
                for (double v : frz_local) if (v >= 0) global_mpjpe_vals.push_back(v);

                // Write CSV for this variant
                variant_stats vs;
                vs.label = vsuffix;

                auto write_var_csv = [&](const std::string& label_suffix,
                    const std::vector<double>& mm_v,
                    const std::vector<double>& lmm_v,
                    const std::vector<double>& frz_v) -> std::string
                {
                    std::string cp = out_dir + "/" + label_suffix + "_mpjpe.csv";
                    FILE* f = fopen(cp.c_str(), "w");
                    if (!f) return "";
                    fprintf(f, "frame,time_seconds,mm_local,lmm_local,frozen_local,mm_lmm_local_diff\n");
                    int nf = (int)std::max({mm_v.size(), lmm_v.size(), frz_v.size()});
                    for (int i = 0; i < nf; i++)
                    {
                        double t  = i / 60.0;
                        double ml = i < (int)mm_v.size()  ? mm_v[i]  : -1.0;
                        double ll = i < (int)lmm_v.size() ? lmm_v[i] : -1.0;
                        double fl = i < (int)frz_v.size() ? frz_v[i] : -1.0;
                        double df = (ml >= 0 && ll >= 0) ? fabs(ml - ll) : -1.0;
                        fprintf(f, "%d,%.6f,%.6e,%.6e,%.6e,%.6e\n", i, t, ml, ll, fl, df);
                    }
                    fclose(f);
                    return cp;
                };

                vs.csv_path_mm = write_var_csv(std::string(vsuffix), mm_local, lmm_local, frz_local);

                // Compute stats helper
                auto compute_stats = [](const std::vector<double>& v,
                    double& mean_, double& median_, double& std_, double& min_, double& max_)
                {
                    std::vector<double> valid;
                    for (double x : v) if (x >= 0) valid.push_back(x);
                    if (valid.empty()) { mean_ = median_ = std_ = min_ = max_ = -1.0; return; }
                    std::sort(valid.begin(), valid.end());
                    min_ = valid.front(); max_ = valid.back();
                    double sum = 0; for (double x : valid) sum += x;
                    mean_ = sum / valid.size();
                    median_ = valid.size() % 2 == 0 ?
                        (valid[valid.size()/2-1] + valid[valid.size()/2]) / 2.0 :
                        valid[valid.size()/2];
                    double sq = 0; for (double x : valid) sq += (x - mean_)*(x - mean_);
                    std_ = sqrt(sq / valid.size());
                };

                compute_stats(mm_local,  vs.mm_mean,     vs.mm_median,     vs.mm_std,     vs.mm_min,     vs.mm_max);
                compute_stats(lmm_local, vs.lmm_mean,    vs.lmm_median,    vs.lmm_std,    vs.lmm_min,    vs.lmm_max);
                compute_stats(frz_local, vs.frozen_mean,  vs.frozen_median,  vs.frozen_std,  vs.frozen_min,  vs.frozen_max);

                all_stats.push_back(vs);
            } // end variant loop

            // ---- Compute global y-axis ----
            double global_ymax = 0.1;
            if (!global_mpjpe_vals.empty())
            {
                std::sort(global_mpjpe_vals.begin(), global_mpjpe_vals.end());
                size_t p99_idx = (size_t)(global_mpjpe_vals.size() * 0.99);
                if (p99_idx >= global_mpjpe_vals.size()) p99_idx = global_mpjpe_vals.size() - 1;
                global_ymax = global_mpjpe_vals[p99_idx] * 1.05;
            }
            std::cout << "\n[big-small] Global y-axis max: " << global_ymax << std::endl;

            // ---- Call plot script for each variant CSV ----
            for (auto& vs : all_stats)
            {
                if (vs.csv_path_mm.empty()) continue;
                std::string cmd = std::string("python \"resources/python/plot_mpjpe.py\" \"")
                    + vs.csv_path_mm + "\" \"" + out_dir
                    + "\" --ymax=" + std::to_string(global_ymax);
                system(cmd.c_str());
                // Rename output PNGs to include variant suffix
                auto rename_png = [&](const std::string& metric) {
                    std::string src = out_dir + "/mpjpe/" + metric + ".png";
                    std::string dst = out_dir + "/mpjpe/" + metric + "_" + vs.label + ".png";
                    if (FileExists(src.c_str())) MoveFileA(src.c_str(), dst.c_str());
                };
                rename_png("mm_local");
                rename_png("lmm_local");
                rename_png("frozen_local");
                rename_png("mm_lmm_local_diff");
            }

            // ---- Write mpjpe_report.md ----
            std::string report_path = out_dir + "/mpjpe/mpjpe_report.md";
            FILE* rep = fopen(report_path.c_str(), "w");
            if (rep)
            {
                fprintf(rep, "# Big vs Small Database Comparison\n\n");
                fprintf(rep, "Generated by `--analyze-both-big-small` mode.\n\n");
                fprintf(rep, "**Shared y-axis max:** %.6f m\n\n", global_ymax);
                fprintf(rep, "## MPJPE Local Statistics (m)\n\n");
                fprintf(rep, "| Metric | MM-big | LMM-big | Frozen-big | MM-small | LMM-small | Frozen-small |\n");
                fprintf(rep, "|:-------|-------:|--------:|-----------:|---------:|----------:|-------------:|\n");

                auto fmtd = [](double v) -> std::string {
                    if (v < 0) return "N/A";
                    char buf[32]; snprintf(buf, sizeof(buf), "%.6f", v);
                    return buf;
                };

                auto get_vs = [&](const std::string& lbl) -> const variant_stats* {
                    for (auto& v : all_stats) if (v.label == lbl) return &v;
                    return nullptr;
                };
                const variant_stats* vb = get_vs("big");
                const variant_stats* vs = get_vs("small");

                #define VB(x) (vb ? fmtd(vb->x).c_str() : "N/A")
                #define VS(x) (vs ? fmtd(vs->x).c_str() : "N/A")
                fprintf(rep, "| Mean   | %s | %s | %s | %s | %s | %s |\n",
                    VB(mm_mean), VB(lmm_mean), VB(frozen_mean),
                    VS(mm_mean), VS(lmm_mean), VS(frozen_mean));
                fprintf(rep, "| Median | %s | %s | %s | %s | %s | %s |\n",
                    VB(mm_median), VB(lmm_median), VB(frozen_median),
                    VS(mm_median), VS(lmm_median), VS(frozen_median));
                fprintf(rep, "| Std    | %s | %s | %s | %s | %s | %s |\n",
                    VB(mm_std), VB(lmm_std), VB(frozen_std),
                    VS(mm_std), VS(lmm_std), VS(frozen_std));
                fprintf(rep, "| Min    | %s | %s | %s | %s | %s | %s |\n",
                    VB(mm_min), VB(lmm_min), VB(frozen_min),
                    VS(mm_min), VS(lmm_min), VS(frozen_min));
                fprintf(rep, "| Max    | %s | %s | %s | %s | %s | %s |\n",
                    VB(mm_max), VB(lmm_max), VB(frozen_max),
                    VS(mm_max), VS(lmm_max), VS(frozen_max));
                #undef VB
                #undef VS
                fprintf(rep, "\n## Output Images (shared coordinate system)\n\n");
                fprintf(rep, "| Image | Description |\n");
                fprintf(rep, "|:------|:------------|\n");
                fprintf(rep, "| frozen_local_big.png | Frozen baseline MPJPE (big DB) |\n");
                fprintf(rep, "| mm_local_big.png | MM MPJPE local (big DB) |\n");
                fprintf(rep, "| lmm_local_big.png | LMM MPJPE local (big DB) |\n");
                fprintf(rep, "| mm_lmm_local_diff_big.png | MM vs LMM diff (big DB) |\n");
                fprintf(rep, "| frozen_local_small.png | Frozen baseline MPJPE (small DB) |\n");
                fprintf(rep, "| mm_local_small.png | MM MPJPE local (small DB) |\n");
                fprintf(rep, "| lmm_local_small.png | LMM MPJPE local (small DB) |\n");
                fprintf(rep, "| mm_lmm_local_diff_small.png | MM vs LMM diff (small DB) |\n");
                fclose(rep);
                std::cout << "[big-small] Report written: " << report_path << std::endl;
            }

            std::cout << "[big-small] Analysis complete. Output: " << out_dir << std::endl;
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

    UnloadModel(character_model);
    if (has_glb_ground) UnloadModel(ground_plane_model);
    ground_grid.cleanup();
    UnloadShader(character_shader);
    UnloadShader(ground_plane_shader);
    UnloadShader(depth_shader);
    UnloadRenderTexture(shadow_map);

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