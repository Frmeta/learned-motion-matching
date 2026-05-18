#include "../common.h"
#include "../vec.h"
#include "../quat.h"
#include "../array.h"
#include "../character.h"
#include "../database.h"

#include <iostream>
#include <chrono>
#include <string>

int main()
{
    std::cout << "============================================================" << std::endl;
    std::cout << "Starting Headless C++ Feature Builder..." << std::endl;
    std::cout << "============================================================" << std::endl;

    // Default feature weights matching controller.cpp exactly
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

    // 1. Process BIG Database
    {
        std::cout << "\n[1/2] Loading BIG Database: ./resources/bin/database_big.bin" << std::endl;
        database db_big;
        auto start = std::chrono::high_resolution_clock::now();
        database_load(db_big, "./resources/bin/database_big.bin");
        auto loaded = std::chrono::high_resolution_clock::now();
        std::cout << "  - Loaded successfully (" << db_big.nframes() << " frames, " << db_big.nbones() << " bones)." << std::endl;

        std::cout << "  - Building matching features (Big)..." << std::endl;
        database_build_matching_features(
            db_big,
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

        auto built = std::chrono::high_resolution_clock::now();
        std::cout << "  - Saving features to: ./resources/bin/features_big.bin" << std::endl;
        database_save_matching_features(db_big, "./resources/bin/features_big.bin", false);
        
        auto saved = std::chrono::high_resolution_clock::now();
        double t_load = std::chrono::duration<double, std::milli>(loaded - start).count();
        double t_build = std::chrono::duration<double, std::milli>(built - loaded).count();
        double t_save = std::chrono::duration<double, std::milli>(saved - built).count();
        std::cout << "  - Completed in: " << (t_load + t_build + t_save) << " ms" << std::endl;
        std::cout << "    * Load: " << t_load << " ms" << std::endl;
        std::cout << "    * Build: " << t_build << " ms" << std::endl;
        std::cout << "    * Save: " << t_save << " ms" << std::endl;
    }

    // 2. Process SMALL Database
    {
        std::cout << "\n[2/2] Loading SMALL Database: ./resources/bin/database_small.bin" << std::endl;
        database db_small;
        auto start = std::chrono::high_resolution_clock::now();
        database_load(db_small, "./resources/bin/database_small.bin");
        auto loaded = std::chrono::high_resolution_clock::now();
        std::cout << "  - Loaded successfully (" << db_small.nframes() << " frames, " << db_small.nbones() << " bones)." << std::endl;

        std::cout << "  - Building matching features (Small)..." << std::endl;
        database_build_matching_features(
            db_small,
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

        auto built = std::chrono::high_resolution_clock::now();
        std::cout << "  - Saving features to: ./resources/bin/features_small.bin" << std::endl;
        database_save_matching_features(db_small, "./resources/bin/features_small.bin", false);

        auto saved = std::chrono::high_resolution_clock::now();
        double t_load = std::chrono::duration<double, std::milli>(loaded - start).count();
        double t_build = std::chrono::duration<double, std::milli>(built - loaded).count();
        double t_save = std::chrono::duration<double, std::milli>(saved - built).count();
        std::cout << "  - Completed in: " << (t_load + t_build + t_save) << " ms" << std::endl;
        std::cout << "    * Load: " << t_load << " ms" << std::endl;
        std::cout << "    * Build: " << t_build << " ms" << std::endl;
        std::cout << "    * Save: " << t_save << " ms" << std::endl;
    }

    std::cout << "\n============================================================" << std::endl;
    std::cout << "All features generated and saved successfully!" << std::endl;
    std::cout << "============================================================" << std::endl;

    return 0;
}
