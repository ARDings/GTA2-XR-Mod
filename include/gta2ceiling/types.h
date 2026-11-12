#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <array>
#include <algorithm>

namespace gta2ceiling {

// Standard vector structures
struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    constexpr Vec2() = default;
    constexpr Vec2(float in_x, float in_y) : x(in_x), y(in_y) {}

    float length() const { return std::sqrt(x * x + y * y); }
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr Vec3() = default;
    constexpr Vec3(float in_x, float in_y, float in_z) : x(in_x), y(in_y), z(in_z) {}

    Vec3 operator+(const Vec3& other) const { return {x + other.x, y + other.y, z + other.z}; }
    Vec3 operator-(const Vec3& other) const { return {x - other.x, y - other.y, z - other.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }

    float length() const { return std::sqrt(x * x + y * y + z * z); }
    Vec3 normalized() const {
        float l = length();
        return (l > 1e-6f) ? Vec3(x / l, y / l, z / l) : Vec3(0, 0, 0);
    }

    static float dot(const Vec3& a, const Vec3& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    static Vec3 cross(const Vec3& a, const Vec3& b) {
        return {
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x
        };
    }
};

struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;

    constexpr Vec4() = default;
    constexpr Vec4(float in_x, float in_y, float in_z, float in_w = 1.0f)
        : x(in_x), y(in_y), z(in_z), w(in_w) {}
};

struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;

    constexpr Quat() = default;
    constexpr Quat(float in_x, float in_y, float in_z, float in_w)
        : x(in_x), y(in_y), z(in_z), w(in_w) {}

    static Quat identity() { return {0.0f, 0.0f, 0.0f, 1.0f}; }

    static Quat from_euler(float pitch_rad, float yaw_rad, float roll_rad) {
        float cp = std::cos(pitch_rad * 0.5f);
        float sp = std::sin(pitch_rad * 0.5f);
        float cy = std::cos(yaw_rad * 0.5f);
        float sy = std::sin(yaw_rad * 0.5f);
        float cr = std::cos(roll_rad * 0.5f);
        float sr = std::sin(roll_rad * 0.5f);

        return {
            sr * cp * cy - cr * sp * sy,
            cr * sp * cy + sr * cp * sy,
            cr * cp * sy - sr * sp * cy,
            cr * cp * cy + sr * sp * sy
        };
    }
};

// 4x4 Row-major Matrix for D3D9 / XR rendering
struct Mat4 {
    float m[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };

    static Mat4 identity() {
        return Mat4();
    }

    static Mat4 translation(const Vec3& t) {
        Mat4 res = identity();
        res.m[12] = t.x;
        res.m[13] = t.y;
        res.m[14] = t.z;
        return res;
    }

    static Mat4 scale(const Vec3& s) {
        Mat4 res = identity();
        res.m[0] = s.x;
        res.m[5] = s.y;
        res.m[10] = s.z;
        return res;
    }

    static Mat4 look_at(const Vec3& eye, const Vec3& target, const Vec3& up) {
        Vec3 z_axis = (target - eye).normalized();
        Vec3 x_axis = Vec3::cross(up, z_axis).normalized();
        Vec3 y_axis = Vec3::cross(z_axis, x_axis);

        Mat4 res;
        res.m[0] = x_axis.x; res.m[1] = y_axis.x; res.m[2] = z_axis.x; res.m[3] = 0.0f;
        res.m[4] = x_axis.y; res.m[5] = y_axis.y; res.m[6] = z_axis.y; res.m[7] = 0.0f;
        res.m[8] = x_axis.z; res.m[9] = y_axis.z; res.m[10] = z_axis.z; res.m[11] = 0.0f;
        res.m[12] = -Vec3::dot(x_axis, eye);
        res.m[13] = -Vec3::dot(y_axis, eye);
        res.m[14] = -Vec3::dot(z_axis, eye);
        res.m[15] = 1.0f;
        return res;
    }

    static Mat4 perspective_fov(float fov_rad, float aspect, float z_near, float z_far) {
        float tan_half_fov = std::tan(fov_rad * 0.5f);
        Mat4 res;
        std::fill(std::begin(res.m), std::end(res.m), 0.0f);
        res.m[0] = 1.0f / (aspect * tan_half_fov);
        res.m[5] = 1.0f / tan_half_fov;
        res.m[10] = z_far / (z_far - z_near);
        res.m[11] = 1.0f;
        res.m[14] = -(z_near * z_far) / (z_far - z_near);
        return res;
    }

    static Mat4 multiply(const Mat4& a, const Mat4& b) {
        Mat4 out;
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                out.m[r * 4 + c] =
                    a.m[r * 4 + 0] * b.m[0 * 4 + c] +
                    a.m[r * 4 + 1] * b.m[1 * 4 + c] +
                    a.m[r * 4 + 2] * b.m[2 * 4 + c] +
                    a.m[r * 4 + 3] * b.m[3 * 4 + c];
            }
        }
        return out;
    }
};

// Spatial placement configuration for the Ceiling Diorama
struct CeilingConfig {
    float distance_meters = 2.1f;    // Distance from player eyes up to the ceiling plane
    float scale = 0.08f;             // World scale (1 GTA2 block = 0.08m in Diorama space)
    float tilt_degrees = 15.0f;      // Downward tilt towards the resting player's natural gaze
    float ipd_meters = 0.063f;       // Default Interpupillary distance (63 mm)
    float fov_horizontal_rad = 1.57f;// ~90 degrees FOV
    float diorama_depth_relief = 1.5f;// Depth relief scale multiplier for building height
};

enum class Eye {
    Left = 0,
    Right = 1
};

} // namespace gta2ceiling

