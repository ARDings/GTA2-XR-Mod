// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "json.hpp"
#include <cmath>
#include <stdexcept>
#include <string>

namespace gcc_diagnostic {
using Json = nlohmann::json;

// Explicit output allowlist: no anchor IDs, positions, polygons, auth or raw
// diagnostics can escape into the report, including through exception text.
inline std::string Status(const Json& source, const char* field) {
    const auto value = source.value(field, std::string());
    constexpr const char* allowed[] = {
        "OK", "UNAUTHORIZED", "STALE_SESSION", "STALE_REFERENCE_EPOCH",
        "SCENE_UNSUPPORTED", "PERMISSION_REQUIRED", "PERMISSION_DENIED",
        "PERMISSION_REQUEST_FAILED", "SUCCESS_CEILING_DATA", "READY", "IDLE",
        "UNINITIALIZED", "REFRESH_REQUESTED", "QUERYING_ROOMS", "QUERYING_CEILINGS",
        "WAITING_FOR_PREVIOUS_SCENE_QUERIES", "NO_EXISTING_SCENE_DATA",
        "ROOMS_WITHOUT_CEILING_UUID", "CEILING_UUIDS_NOT_LOADED",
        "WAITING_FOR_VALID_CEILING_POSE", "INVALID_BASE_SPACE_OR_TIME",
        "OUTPUT_LIMIT_EXCEEDED", "GRANTED", "DENIED", "UNKNOWN",
        "BRIDGE_JAVA_UNAVAILABLE", "PERMISSION_PENDING"
    };
    for (const auto* candidate : allowed) if (value == candidate) return value;
    return "OTHER_STATUS_REDACTED";
}

inline double PolygonArea(const Json& polygon) {
    if (!polygon.is_array() || polygon.size() < 3 || polygon.size() > 512)
        throw std::runtime_error("Invalid polygon");
    double sum[3]{};
    for (size_t i = 0; i < polygon.size(); ++i) {
        const auto& a = polygon.at(i);
        const auto& b = polygon.at((i + 1) % polygon.size());
        if (!a.is_array() || !b.is_array() || a.size() != 3 || b.size() != 3)
            throw std::runtime_error("Invalid vertex");
        double av[3], bv[3];
        for (size_t j = 0; j < 3; ++j) {
            av[j] = a.at(j).get<double>(); bv[j] = b.at(j).get<double>();
            if (!std::isfinite(av[j]) || !std::isfinite(bv[j]))
                throw std::runtime_error("Nonfinite vertex");
        }
        sum[0] += av[1] * bv[2] - av[2] * bv[1];
        sum[1] += av[2] * bv[0] - av[0] * bv[2];
        sum[2] += av[0] * bv[1] - av[1] * bv[0];
    }
    const double area = 0.5 * std::hypot(sum[0], sum[1], sum[2]);
    if (!std::isfinite(area)) throw std::runtime_error("Invalid area");
    return area;
}

inline Json Summary(const Json& reply) {
    Json out{{"probe", "GCCROOM1_metadata_v1"}, {"replyStatus", Status(reply, "status")}};
    out["referenceEpoch"] = reply.at("referenceEpoch").get<uint64_t>();
    out["recenterCount"] = reply.at("recenterCount").get<uint32_t>();
    out["hmdSync"] = reply.at("hmdSync").get<uint32_t>();
    out["xrTime"] = reply.at("xrTime").get<int64_t>();
    out["scenePermission"] = Status(reply, "scenePermission");
    out["sceneCapability"] = reply.at("capabilities").at("sceneQuery").get<bool>();
    out["rawStageCapability"] = reply.at("capabilities").at("rawStage").get<bool>();
    if (!reply.contains("scene")) return out;
    if (reply.at("coordinateSpace") != "XRAPI_BASE" || reply.at("units") != "meters" ||
        reply.at("baseSpaceIndex") != 1) throw std::runtime_error("Unexpected base");
    out["coordinateSpace"] = "XRAPI_BASE"; out["baseSpaceIndex"] = 1; out["units"] = "meters";
    const auto& scene = reply.at("scene");
    out["sceneStatus"] = Status(scene, "status");
    out["sceneQueryResult"] = scene.at("sceneQueryResult").get<int64_t>();
    const auto& ceilings = scene.at("ceilings");
    if (!ceilings.is_array() || ceilings.size() > 64) throw std::runtime_error("Invalid ceiling count");
    out["ceilingCount"] = ceilings.size(); out["ceilings"] = Json::array();
    for (const auto& ceiling : ceilings) {
        const bool valid = ceiling.at("poseValid").get<bool>();
        const auto& polygon = ceiling.at("polygon3DBase");
        Json item{{"locationFlags", ceiling.at("locationFlags").get<uint64_t>()},
                  {"poseValid", valid}, {"poseTracked", ceiling.at("poseTracked").get<bool>()},
                  {"sameFrameTime", ceiling.at("xrTime") == reply.at("xrTime")},
                  {"vertexCount", polygon.size()}};
        item["areaSquareMeters"] = valid ? Json(PolygonArea(polygon)) : Json(nullptr);
        out["ceilings"].push_back(std::move(item));
    }
    return out;
}
} // namespace gcc_diagnostic
