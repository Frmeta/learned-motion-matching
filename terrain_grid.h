#pragma once

#include "raylib.h"
#include "raymath.h"
#include <vector>
#include <float.h>
#include <math.h>

struct TerrainTriangle {
    Vector3 p1, p2, p3;
};

struct TerrainGrid {
    float min_x, min_z;
    float max_x, max_z;
    float cell_size;
    int width, height;
    std::vector<std::vector<TerrainTriangle>> cells;

    TerrainGrid() : min_x(0), min_z(0), max_x(0), max_z(0), cell_size(1.0f), width(0), height(0) {}

    void build(const Model& model, float desired_cell_size = 1.0f) {
        cleanup();
        cell_size = desired_cell_size;

        // 1. Calculate Bounding Box
        min_x = FLT_MAX; min_z = FLT_MAX;
        max_x = -FLT_MAX; max_z = -FLT_MAX;

        for (int i = 0; i < model.meshCount; i++) {
            Mesh& mesh = model.meshes[i];
            for (int v = 0; v < mesh.vertexCount; v++) {
                Vector3 pos = { mesh.vertices[v * 3], mesh.vertices[v * 3 + 1], mesh.vertices[v * 3 + 2] };
                pos = Vector3Transform(pos, model.transform);
                if (pos.x < min_x) min_x = pos.x;
                if (pos.x > max_x) max_x = pos.x;
                if (pos.z < min_z) min_z = pos.z;
                if (pos.z > max_z) max_z = pos.z;
            }
        }

        // Add some padding
        min_x -= 1.0f; min_z -= 1.0f;
        max_x += 1.0f; max_z += 1.0f;

        width = (int)ceilf((max_x - min_x) / cell_size);
        height = (int)ceilf((max_z - min_z) / cell_size);
        
        if (width <= 0) width = 1;
        if (height <= 0) height = 1;
        
        cells.resize(width * height);

        // 2. Bin Triangles
        for (int i = 0; i < model.meshCount; i++) {
            Mesh& mesh = model.meshes[i];
            for (int j = 0; j < mesh.triangleCount; j++) {
                int i1, i2, i3;
                if (mesh.indices) {
                    i1 = mesh.indices[j * 3];
                    i2 = mesh.indices[j * 3 + 1];
                    i3 = mesh.indices[j * 3 + 2];
                } else {
                    i1 = j * 3;
                    i2 = j * 3 + 1;
                    i3 = j * 3 + 2;
                }

                Vector3 v1 = { mesh.vertices[i1 * 3], mesh.vertices[i1 * 3 + 1], mesh.vertices[i1 * 3 + 2] };
                Vector3 v2 = { mesh.vertices[i2 * 3], mesh.vertices[i2 * 3 + 1], mesh.vertices[i2 * 3 + 2] };
                Vector3 v3 = { mesh.vertices[i3 * 3], mesh.vertices[i3 * 3 + 1], mesh.vertices[i3 * 3 + 2] };

                v1 = Vector3Transform(v1, model.transform);
                v2 = Vector3Transform(v2, model.transform);
                v3 = Vector3Transform(v3, model.transform);

                TerrainTriangle tri = { v1, v2, v3 };

                // Find overlapping cells
                float tmin_x = fminf(v1.x, fminf(v2.x, v3.x));
                float tmax_x = fmaxf(v1.x, fmaxf(v2.x, v3.x));
                float tmin_z = fminf(v1.z, fminf(v2.z, v3.z));
                float tmax_z = fmaxf(v1.z, fmaxf(v2.z, v3.z));

                int start_x = (int)floorf((tmin_x - min_x) / cell_size);
                int end_x = (int)floorf((tmax_x - min_x) / cell_size);
                int start_z = (int)floorf((tmin_z - min_z) / cell_size);
                int end_z = (int)floorf((tmax_z - min_z) / cell_size);

                // Helper to clamp values
                auto clamp_int = [](int v, int mn, int mx) { return v < mn ? mn : v > mx ? mx : v; };

                start_x = clamp_int(start_x, 0, width - 1);
                end_x = clamp_int(end_x, 0, width - 1);
                start_z = clamp_int(start_z, 0, height - 1);
                end_z = clamp_int(end_z, 0, height - 1);

                for (int z = start_z; z <= end_z; z++) {
                    for (int x = start_x; x <= end_x; x++) {
                        cells[x + z * width].push_back(tri);
                    }
                }
            }
        }
    }

    bool get_height(Vector3 position, float& height_out) const {
        if (position.x < min_x || position.x > max_x || position.z < min_z || position.z > max_z) return false;

        int cx = (int)floorf((position.x - min_x) / cell_size);
        int cz = (int)floorf((position.z - min_z) / cell_size);

        if (cx < 0 || cx >= width || cz < 0 || cz >= height) return false;

        const std::vector<TerrainTriangle>& cell_triangles = cells[cx + cz * width];
        if (cell_triangles.empty()) return false;

        Ray ray = { {position.x, 1000.0f, position.z}, {0, -1, 0} };
        float max_h = -FLT_MAX;
        bool hit = false;

        for (const auto& tri : cell_triangles) {
            RayCollision col = GetRayCollisionTriangle(ray, tri.p1, tri.p2, tri.p3);
            if (col.hit) {
                if (col.point.y > max_h) {
                    max_h = col.point.y;
                    hit = true;
                }
            }
        }

        if (hit) {
            height_out = max_h;
            return true;
        }
        return false;
    }

    void cleanup() {
        cells.clear();
        width = 0;
        height = 0;
    }
};
