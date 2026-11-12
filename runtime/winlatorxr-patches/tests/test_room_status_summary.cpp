// SPDX-License-Identifier: GPL-3.0-or-later
#include "../diagnostics/room_status_summary.h"
#include <cassert>
#include <iostream>
#include <limits>
int main() {
    using gcc_diagnostic::Json;
    const Json polygon = {{-2, 3, -4}, {2, 3, -4}, {2, 3, 4}, {-2, 3, 4}};
    assert(std::abs(gcc_diagnostic::PolygonArea(polygon) - 32.0) < 1e-9);
    const Json vertical = {{0, 0, 0}, {0, 3, 0}, {0, 3, 4}, {0, 0, 4}};
    assert(std::abs(gcc_diagnostic::PolygonArea(vertical) - 12.0) < 1e-9);
    Json ceiling{{"sceneUUID", "PRIVATE_SCENE_UUID"}, {"ceilingUUID", "PRIVATE_CEILING_UUID"},
        {"planePose", "PRIVATE_PLANE"}, {"polygon3DBase", polygon}, {"polygon2D", "PRIVATE_POLYGON"},
        {"locationFlags", 15}, {"poseValid", true}, {"poseTracked", true}, {"xrTime", 200}};
    Json reply{{"status", "OK"}, {"referenceEpoch", 4}, {"recenterCount", 7}, {"hmdSync", 48},
        {"xrTime", 200}, {"scenePermission", "GRANTED"}, {"sessionId", "PRIVATE_SESSION"},
        {"token", "PRIVATE_TOKEN"}, {"capabilities", {{"sceneQuery", true}, {"rawStage", true}}},
        {"coordinateSpace", "XRAPI_BASE"}, {"baseSpaceIndex", 1}, {"units", "meters"},
        {"scene", {{"status", "SUCCESS_CEILING_DATA"}, {"sceneQueryResult", 0}, {"ceilings", Json::array({ceiling})}}}};
    const auto summary = gcc_diagnostic::Summary(reply);
    assert(summary.at("ceilingCount") == 1);
    assert(summary.at("ceilings").at(0).at("areaSquareMeters") == 32.0);
    assert(summary.at("ceilings").at(0).at("sameFrameTime") == true);
    const auto text = summary.dump();
    assert(text.find("PRIVATE_") == std::string::npos);
    assert(text.find("polygon") == std::string::npos && text.find("UUID") == std::string::npos && text.find("token") == std::string::npos);
    reply["status"] = "PRIVATE_ERROR_DETAILS";
    assert(gcc_diagnostic::Summary(reply).at("replyStatus") == "OTHER_STATUS_REDACTED");
    reply["scene"]["ceilings"][0]["polygon3DBase"][0][0] = std::numeric_limits<double>::infinity();
    bool rejected = false;
    try { (void)gcc_diagnostic::Summary(reply); } catch (...) { rejected = true; }
    assert(rejected);
    std::cout << "PASS: sanitized room metadata, horizontal/vertical area, no private fields, invalid geometry rejected\n";
}
