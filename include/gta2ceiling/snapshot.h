#pragma once

#include "gta2ceiling/types.h"
#include <vector>
#include <mutex>
#include <chrono>
#include <atomic>
#include <cstdint>

namespace gta2ceiling {

enum class EntityType : uint8_t {
    PlayerPed = 0,
    Pedestrian = 1,
    Car = 2,
    Projectile = 3,
    Explosion = 4,
    Particle = 5
};

struct DynamicEntitySnapshot {
    uint32_t entity_id = 0;
    EntityType type = EntityType::Pedestrian;
    Vec3 position;
    float heading_rad = 0.0f;
    uint32_t sprite_index = 0;
    float alpha = 1.0f;
    bool is_discrete_event = false; // true for teleport/explosion/death to disable interpolation
};

struct WorldSnapshot {
    uint64_t sequence_id = 0;
    double timestamp_seconds = 0.0;
    Vec3 player_position;
    float player_heading_rad = 0.0f;
    bool in_vehicle = false;
    uint32_t vehicle_id = 0;
    Vec3 camera_center;
    std::vector<DynamicEntitySnapshot> entities;
};

class SnapshotRingBuffer {
public:
    static constexpr size_t BUFFER_CAPACITY = 8;
    static constexpr double STALL_THRESHOLD_SECONDS = 0.400; // 400ms stall threshold

    SnapshotRingBuffer() {
        m_buffer.resize(BUFFER_CAPACITY);
    }

    void push_snapshot(const WorldSnapshot& snapshot) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_buffer[m_write_index % BUFFER_CAPACITY] = snapshot;
        m_write_index++;
        m_total_snapshots_produced++;
        m_last_produce_time = std::chrono::steady_clock::now();
    }

    bool get_latest_pair(WorldSnapshot& older, WorldSnapshot& newer) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_write_index < 2) {
            if (m_write_index == 1) {
                older = m_buffer[0];
                newer = m_buffer[0];
                return true;
            }
            return false;
        }

        uint64_t newest_idx = m_write_index - 1;
        uint64_t older_idx = m_write_index - 2;

        newer = m_buffer[newest_idx % BUFFER_CAPACITY];
        older = m_buffer[older_idx % BUFFER_CAPACITY];
        return true;
    }

    bool is_simulation_stalled() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_write_index == 0) return true;
        auto now = std::chrono::steady_clock::now();
        std::chrono::duration<double> diff = now - m_last_produce_time;
        return diff.count() > STALL_THRESHOLD_SECONDS;
    }

    uint64_t total_produced() const { return m_total_snapshots_produced.load(); }

    // Interpolates player and entities between older and newer snapshots for a given alpha [0, 1]
    static WorldSnapshot interpolate(const WorldSnapshot& a, const WorldSnapshot& b, float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        WorldSnapshot result;
        result.sequence_id = b.sequence_id;
        result.timestamp_seconds = a.timestamp_seconds + (b.timestamp_seconds - a.timestamp_seconds) * t;

        // Interpolate player position
        result.player_position = a.player_position + (b.player_position - a.player_position) * t;

        // Angle interpolation (shortest path)
        float diff = b.player_heading_rad - a.player_heading_rad;
        while (diff < -3.14159265f) diff += 6.2831853f;
        while (diff > 3.14159265f) diff -= 6.2831853f;
        result.player_heading_rad = a.player_heading_rad + diff * t;

        result.in_vehicle = b.in_vehicle;
        result.vehicle_id = b.vehicle_id;
        result.camera_center = a.camera_center + (b.camera_center - a.camera_center) * t;

        // Interpolate matching entities
        result.entities.reserve(b.entities.size());
        for (const auto& ent_b : b.entities) {
            if (ent_b.is_discrete_event) {
                result.entities.push_back(ent_b);
                continue;
            }

            // Find matching entity in older snapshot
            auto it = std::find_if(a.entities.begin(), a.entities.end(),
                [&ent_b](const DynamicEntitySnapshot& e) { return e.entity_id == ent_b.entity_id; });

            if (it != a.entities.end()) {
                DynamicEntitySnapshot ent_interp = ent_b;
                ent_interp.position = it->position + (ent_b.position - it->position) * t;
                float h_diff = ent_b.heading_rad - it->heading_rad;
                while (h_diff < -3.14159265f) h_diff += 6.2831853f;
                while (h_diff > 3.14159265f) h_diff -= 6.2831853f;
                ent_interp.heading_rad = it->heading_rad + h_diff * t;
                result.entities.push_back(ent_interp);
            } else {
                result.entities.push_back(ent_b);
            }
        }

        return result;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<WorldSnapshot> m_buffer;
    uint64_t m_write_index = 0;
    std::atomic<uint64_t> m_total_snapshots_produced{0};
    std::chrono::steady_clock::time_point m_last_produce_time{std::chrono::steady_clock::now()};
};

} // namespace gta2ceiling

