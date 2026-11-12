// SPDX-License-Identifier: GPL-3.0-or-later
// Host-side adaptation of GTA2Ceiling's MIT Scene diagnostic query logic.
#include "gcc_scene_query.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

namespace gcc_host {
namespace {
constexpr uint32_t kEntities = 64, kVertices = 512;
constexpr size_t kAuditEvents = 48, kGeometryBytes = 44000;
constexpr size_t kOutstandingQueries = 16;
using Clock = std::chrono::steady_clock;
std::string Quote(const std::string& value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) { char escaped[7]; std::snprintf(escaped, sizeof(escaped), "\\u%04x", c); out += escaped; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}
std::string Uuid(const XrUuidEXT& id) {
    char out[33];
    for (unsigned i = 0; i < 16; ++i) std::snprintf(out + i * 2, 3, "%02x", id.data[i]);
    return out;
}
bool Nonzero(const XrUuidEXT& id) { for (auto c : id.data) if (c) return true; return false; }
bool HasLabel(const std::string& labels, const char* wanted) {
    std::istringstream input(labels); std::string token;
    while (std::getline(input, token, ',')) {
        auto first = token.find_first_not_of(" \t\r\n"), last = token.find_last_not_of(" \t\r\n");
        if (first != std::string::npos && token.substr(first, last - first + 1) == wanted) return true;
    }
    return false;
}
bool CeilingLabel(const std::string& labels) { return HasLabel(labels, "CEILING"); }
// GTA2Ceiling can also place the city on the floor; both are horizontal planes.
bool PlaneLabel(const std::string& labels) { return CeilingLabel(labels) || HasLabel(labels, "FLOOR"); }
bool Finite(const XrVector3f& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
bool ValidPose(const XrPosef& pose) {
    const auto& q = pose.orientation;
    const float norm = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
    return Finite(pose.position) && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) &&
        std::isfinite(q.w) && std::isfinite(norm) && std::fabs(norm - 1.0f) < 0.01f;
}
XrVector3f Transform(const XrPosef& pose, XrVector2f xy) {
    const auto& q = pose.orientation; const XrVector3f v{xy.x, xy.y, 0};
    const XrVector3f t{2*(q.y*v.z-q.z*v.y), 2*(q.z*v.x-q.x*v.z), 2*(q.x*v.y-q.y*v.x)};
    return {pose.position.x+v.x+q.w*t.x+q.y*t.z-q.z*t.y,
            pose.position.y+v.y+q.w*t.y+q.z*t.x-q.x*t.z,
            pose.position.z+v.z+q.w*t.z+q.x*t.y-q.y*t.x};
}
}

struct SceneQuery::Impl {
    enum class Phase { Idle, Refresh, Rooms, Ceilings, Ready, Failed };
    struct Event { std::string api, name; XrResult result; };
    struct Ceiling {
        XrSpace space = XR_NULL_HANDLE;
        std::string scene, uuid, labels;
        std::vector<XrVector2f> boundary;
        bool metadata = false;
    };
    XrInstance instance = XR_NULL_HANDLE;
    XrSession session = XR_NULL_HANDLE;
    Phase phase = Phase::Idle;
    std::string status = "UNINITIALIZED";
    XrResult queryResult = XR_ERROR_CALL_ORDER_INVALID;
    XrAsyncRequestIdFB roomRequest = 0, ceilingRequest = 0;
    bool roomComplete = false, ceilingComplete = false;
    bool initialized = false;
    Clock::time_point deadline{};
    std::vector<Event> events;
    std::vector<XrSpaceQueryResultFB> rooms;
    std::vector<Ceiling> ceilings;
    std::map<std::string, std::string> ceilingRooms;
    std::map<XrSpace, std::vector<XrSpaceComponentTypeFB>> components;
    std::set<std::pair<XrSpace, XrSpaceComponentTypeFB>> enabling;
    std::set<XrAsyncRequestIdFB> pendingQueries, retiredQueries, componentRequests;
    std::set<XrSpace> owned;
    PFN_xrQuerySpacesFB query = nullptr;
    PFN_xrRetrieveSpaceQueryResultsFB retrieve = nullptr;
    PFN_xrGetSpaceComponentStatusFB getComponent = nullptr;
    PFN_xrSetSpaceComponentStatusFB setComponent = nullptr;
    PFN_xrEnumerateSpaceSupportedComponentsFB enumerate = nullptr;
    PFN_xrGetSpaceRoomLayoutFB layout = nullptr;
    PFN_xrGetSpaceSemanticLabelsFB labels = nullptr;
    PFN_xrGetSpaceBoundary2DFB boundary = nullptr;

    void Note(const char* api, XrResult result) {
        char text[XR_MAX_RESULT_STRING_SIZE]{};
        if (instance) xrResultToString(instance, result, text);
        if (!text[0]) std::snprintf(text, sizeof(text), "XrResult(%d)", static_cast<int>(result));
        if (events.size() == kAuditEvents) events.erase(events.begin());
        events.push_back({std::string(api).substr(0, 96), std::string(text).substr(0, 96), result});
    }
    bool Fail(const std::string& message) { phase = Phase::Failed; status = "ERROR: " + message.substr(0, 200); return false; }
    bool Check(const char* api, XrResult result) {
        Note(api, result);
        return XR_SUCCEEDED(result) || Fail(std::string(api) + ": " + events.back().name);
    }
    template<class T> bool Load(T& target, const char* name) {
        PFN_xrVoidFunction function = nullptr;
        if (!Check(name, xrGetInstanceProcAddr(instance, name, &function))) return false;
        target = reinterpret_cast<T>(function);
        return target || Fail(std::string("Missing function ") + name);
    }
    void ClearSpaces() {
        for (auto space : owned) xrDestroySpace(space);
        owned.clear(); rooms.clear(); ceilings.clear(); ceilingRooms.clear(); components.clear(); enabling.clear(); componentRequests.clear();
    }
    bool Query(bool forRooms, std::vector<XrUuidEXT>* ids = nullptr) {
        XrSpaceStorageLocationFilterInfoFB storage{XR_TYPE_SPACE_STORAGE_LOCATION_FILTER_INFO_FB};
        storage.location = XR_SPACE_STORAGE_LOCATION_LOCAL_FB;
        XrSpaceComponentFilterInfoFB component{XR_TYPE_SPACE_COMPONENT_FILTER_INFO_FB};
        component.componentType = XR_SPACE_COMPONENT_TYPE_ROOM_LAYOUT_FB; component.next = &storage;
        XrSpaceUuidFilterInfoFB uuidFilter{XR_TYPE_SPACE_UUID_FILTER_INFO_FB}; uuidFilter.next = &storage;
        if (ids) { uuidFilter.uuidCount = static_cast<uint32_t>(ids->size()); uuidFilter.uuids = ids->data(); }
        XrSpaceQueryInfoFB info{XR_TYPE_SPACE_QUERY_INFO_FB};
        info.queryAction = XR_SPACE_QUERY_ACTION_LOAD_FB; info.maxResultCount = kEntities; info.timeout = 15000000000LL;
        info.filter = forRooms ? reinterpret_cast<XrSpaceFilterInfoBaseHeaderFB*>(&component) : reinterpret_cast<XrSpaceFilterInfoBaseHeaderFB*>(&uuidFilter);
        auto& request = forRooms ? roomRequest : ceilingRequest;
        queryResult = query(session, reinterpret_cast<XrSpaceQueryInfoBaseHeaderFB*>(&info), &request);
        if (!Check(forRooms ? "xrQuerySpacesFB(ROOM_LAYOUT)" : "xrQuerySpacesFB(CEILING_UUID)", queryResult)) return false;
        pendingQueries.insert(request); phase = forRooms ? Phase::Rooms : Phase::Ceilings;
        status = forRooms ? "QUERYING_ROOMS" : "QUERYING_CEILINGS";
        return true;
    }
    void Results(XrAsyncRequestIdFB request, bool retired) {
        XrSpaceQueryResultsFB output{XR_TYPE_SPACE_QUERY_RESULTS_FB};
        XrResult result = retrieve(session, request, &output);
        Note("xrRetrieveSpaceQueryResultsFB(count)", result);
        if (XR_FAILED(result)) { if (!retired) Fail("Cannot retrieve scene result count"); return; }
        if (output.resultCountOutput > kEntities) { if (!retired) Fail("Scene result count exceeds 64"); return; }
        std::vector<XrSpaceQueryResultFB> items(output.resultCountOutput);
        output.resultCapacityInput = static_cast<uint32_t>(items.size()); output.results = items.data();
        result = retrieve(session, request, &output); Note("xrRetrieveSpaceQueryResultsFB(data)", result);
        if (XR_FAILED(result) || output.resultCountOutput > items.size()) { if (!retired) Fail("Scene result size changed"); return; }
        items.resize(output.resultCountOutput);
        for (const auto& item : items) {
            if (!item.space) continue;
            if (retired) { if (!owned.count(item.space)) xrDestroySpace(item.space); continue; }
            if (!owned.insert(item.space).second) continue;
            if (request == roomRequest) {
                if (rooms.size() >= kEntities) { Fail("Too many room anchors"); continue; }
                rooms.push_back(item);
            } else {
                const auto uuid = Uuid(item.uuid); auto room = ceilingRooms.find(uuid);
                if (room == ceilingRooms.end() || ceilings.size() >= kEntities) { Fail("Unexpected ceiling query result"); continue; }
                Ceiling ceiling; ceiling.space = item.space; ceiling.uuid = uuid; ceiling.scene = room->second;
                ceilings.push_back(std::move(ceiling));
            }
        }
    }
    bool Ready(XrSpace space, XrSpaceComponentTypeFB type) {
        auto available = components.find(space);
        if (available == components.end()) {
            uint32_t count = 0;
            if (!Check("xrEnumerateSpaceSupportedComponentsFB(count)", enumerate(space, 0, &count, nullptr))) return false;
            if (count > 64) return Fail("Too many anchor components");
            std::vector<XrSpaceComponentTypeFB> types(count);
            if (!Check("xrEnumerateSpaceSupportedComponentsFB(data)", enumerate(space, count, &count, types.data()))) return false;
            if (count > types.size()) return Fail("Component count changed");
            types.resize(count); available = components.emplace(space, std::move(types)).first;
        }
        if (std::find(available->second.begin(), available->second.end(), type) == available->second.end())
            return Fail("Required anchor component absent: " + std::to_string(static_cast<int>(type)));
        XrSpaceComponentStatusFB current{XR_TYPE_SPACE_COMPONENT_STATUS_FB};
        auto result = getComponent(space, type, &current);
        if (XR_FAILED(result)) return Check("xrGetSpaceComponentStatusFB", result);
        if (current.enabled) return true;
        if (!current.changePending && enabling.insert({space, type}).second) {
            XrSpaceComponentStatusSetInfoFB enable{XR_TYPE_SPACE_COMPONENT_STATUS_SET_INFO_FB};
            enable.componentType = type; enable.enabled = XR_TRUE; enable.timeout = 10000000000LL;
            XrAsyncRequestIdFB request = 0;
            if (!Check("xrSetSpaceComponentStatusFB", setComponent(space, &enable, &request))) return false;
            componentRequests.insert(request);
        }
        return false;
    }
    bool Metadata(Ceiling& ceiling) {
        if (!Ready(ceiling.space, XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB) ||
            !Ready(ceiling.space, XR_SPACE_COMPONENT_TYPE_BOUNDED_2D_FB) ||
            !Ready(ceiling.space, XR_SPACE_COMPONENT_TYPE_SEMANTIC_LABELS_FB)) return false;
        if (ceiling.metadata) return true;
        XrSemanticLabelsFB names{XR_TYPE_SEMANTIC_LABELS_FB};
        if (!Check("xrGetSpaceSemanticLabelsFB(count)", labels(session, ceiling.space, &names))) return false;
        if (names.bufferCountOutput > 256) return Fail("Semantic labels exceed bounded limit");
        std::vector<char> text(std::max(1u, names.bufferCountOutput) + 1, 0);
        names.bufferCapacityInput = static_cast<uint32_t>(text.size()); names.buffer = text.data();
        if (!Check("xrGetSpaceSemanticLabelsFB(data)", labels(session, ceiling.space, &names))) return false;
        if (names.bufferCountOutput > text.size()) return Fail("Semantic labels changed during read");
        text.back() = 0; ceiling.labels = text.data();
        if (!PlaneLabel(ceiling.labels)) return Fail("Room plane UUID lacks CEILING/FLOOR label");
        XrBoundary2DFB polygon{XR_TYPE_BOUNDARY_2D_FB};
        if (!Check("xrGetSpaceBoundary2DFB(count)", boundary(session, ceiling.space, &polygon))) return false;
        if (polygon.vertexCountOutput < 3 || polygon.vertexCountOutput > kVertices) return Fail("Ceiling requires 3..512 vertices");
        std::vector<XrVector2f> vertices(polygon.vertexCountOutput);
        polygon.vertexCapacityInput = static_cast<uint32_t>(vertices.size()); polygon.vertices = vertices.data();
        if (!Check("xrGetSpaceBoundary2DFB(data)", boundary(session, ceiling.space, &polygon))) return false;
        if (polygon.vertexCountOutput < 3 || polygon.vertexCountOutput > vertices.size()) return Fail("Ceiling boundary changed during read");
        vertices.resize(polygon.vertexCountOutput);
        double twiceArea = 0;
        for (size_t i = 0; i < vertices.size(); ++i) {
            const auto& a = vertices[i]; const auto& b = vertices[(i+1)%vertices.size()];
            if (!std::isfinite(a.x) || !std::isfinite(a.y)) return Fail("Nonfinite ceiling boundary");
            twiceArea += static_cast<double>(a.x)*b.y - static_cast<double>(b.x)*a.y;
        }
        if (!std::isfinite(twiceArea) || std::fabs(twiceArea) < 1e-6) return Fail("Degenerate ceiling boundary");
        ceiling.boundary = std::move(vertices); ceiling.metadata = true; return true;
    }
    std::string FinishJson(const std::string& snapshotStatus, const std::string& ceilingJson) const {
        std::ostringstream out; out.imbue(std::locale::classic());
        out << "{\"status\":" << Quote(snapshotStatus) << ",\"sceneQueryResult\":" << queryResult << ",\"ceilings\":[" << ceilingJson << "],\"events\":[";
        for (size_t i = 0; i < events.size(); ++i) {
            if (i) out << ',';
            out << "{\"api\":" << Quote(events[i].api) << ",\"result\":" << events[i].result << ",\"resultName\":" << Quote(events[i].name) << '}';
        }
        const auto json = out.str() + "]}";
        if (json.size() > 58000) return "{\"status\":\"OUTPUT_LIMIT_EXCEEDED\",\"sceneQueryResult\":-1,\"ceilings\":[],\"events\":[]}";
        return json;
    }
};

SceneQuery::SceneQuery() : impl_(new Impl) {}
SceneQuery::~SceneQuery() { Shutdown(); }
bool SceneQuery::Initialize(XrInstance instance, XrSession session) {
    Shutdown(); auto& p = *impl_; p.events.clear(); p.instance = instance; p.session = session;
    if (!instance || !session) return p.Fail("Missing host OpenXR instance/session");
    if (!p.Load(p.query, "xrQuerySpacesFB") || !p.Load(p.retrieve, "xrRetrieveSpaceQueryResultsFB") ||
        !p.Load(p.getComponent, "xrGetSpaceComponentStatusFB") || !p.Load(p.setComponent, "xrSetSpaceComponentStatusFB") ||
        !p.Load(p.enumerate, "xrEnumerateSpaceSupportedComponentsFB") || !p.Load(p.layout, "xrGetSpaceRoomLayoutFB") ||
        !p.Load(p.labels, "xrGetSpaceSemanticLabelsFB") || !p.Load(p.boundary, "xrGetSpaceBoundary2DFB")) return false;
    p.status = "IDLE"; p.phase = Impl::Phase::Idle; p.initialized = true; return true;
}
void SceneQuery::Shutdown() {
    auto& p = *impl_; p.ClearSpaces(); p.pendingQueries.clear(); p.retiredQueries.clear();
    p.roomRequest = p.ceilingRequest = 0; p.roomComplete = p.ceilingComplete = false;
    p.instance = XR_NULL_HANDLE; p.session = XR_NULL_HANDLE; p.phase = Impl::Phase::Idle;
    p.initialized = false;
    p.queryResult = XR_ERROR_CALL_ORDER_INVALID; p.status = "SHUTDOWN";
}
const std::string& SceneQuery::Status() const { return impl_->status; }
void SceneQuery::RequestRefresh() {
    auto& p = *impl_;
    if (!p.initialized) { p.Fail("Scene query not initialized"); return; }
    p.retiredQueries.insert(p.pendingQueries.begin(), p.pendingQueries.end()); p.pendingQueries.clear();
    p.ClearSpaces(); p.roomRequest = p.ceilingRequest = 0; p.roomComplete = p.ceilingComplete = false;
    p.phase = Impl::Phase::Refresh; p.status = "REFRESH_REQUESTED";
    // A refresh can be requested before the game asks for room data or while
    // permission UI is open. Start the query deadline only when Tick actually
    // dispatches it, not while the host deliberately withholds Scene API calls.
    p.queryResult = XR_ERROR_CALL_ORDER_INVALID; p.deadline = {};
}
void SceneQuery::OnEvent(const XrEventDataBuffer& buffer) {
    auto& p = *impl_; if (!p.instance || !p.session) return;
    if (buffer.type == XR_TYPE_EVENT_DATA_SPACE_QUERY_RESULTS_AVAILABLE_FB) {
        auto id = reinterpret_cast<const XrEventDataSpaceQueryResultsAvailableFB*>(&buffer)->requestId;
        if (p.retiredQueries.count(id)) p.Results(id, true);
        else if (id != 0 && (id == p.roomRequest || id == p.ceilingRequest)) p.Results(id, false);
    } else if (buffer.type == XR_TYPE_EVENT_DATA_SPACE_QUERY_COMPLETE_FB) {
        const auto& event = *reinterpret_cast<const XrEventDataSpaceQueryCompleteFB*>(&buffer);
        if (p.retiredQueries.erase(event.requestId)) return;
        if (!p.pendingQueries.erase(event.requestId)) return;
        p.queryResult = event.result;
        if (!p.Check("XrEventDataSpaceQueryCompleteFB", event.result)) return;
        if (event.requestId == p.roomRequest) p.roomComplete = true;
        if (event.requestId == p.ceilingRequest) p.ceilingComplete = true;
    } else if (buffer.type == XR_TYPE_EVENT_DATA_SPACE_SET_STATUS_COMPLETE_FB) {
        const auto& event = *reinterpret_cast<const XrEventDataSpaceSetStatusCompleteFB*>(&buffer);
        if (p.componentRequests.erase(event.requestId)) p.Check("XrEventDataSpaceSetStatusCompleteFB", event.result);
    }
}
void SceneQuery::Tick(XrTime time) {
    (void)time; auto& p = *impl_;
    if (p.phase == Impl::Phase::Idle || p.phase == Impl::Phase::Failed || p.phase == Impl::Phase::Ready) return;
    if (p.phase == Impl::Phase::Refresh) {
        // Held camera-offset input can invalidate every display frame. There
        // is no FB query cancellation call: drain old requests and coalesce
        // refreshes instead of exhausting the bound and remaining Failed after
        // the input stops. Late results are still retrieved/destroyed by ID.
        if(p.retiredQueries.size()>=kOutstandingQueries) {
            p.status="WAITING_FOR_PREVIOUS_SCENE_QUERIES";return;
        }
        p.deadline = Clock::now() + std::chrono::seconds(45);
        p.Query(true); return;
    }
    if (Clock::now() > p.deadline) { p.Fail("Scene query/component timeout"); return; }
    if (p.phase == Impl::Phase::Rooms && p.roomComplete) {
        if (p.rooms.empty()) { p.status = "NO_EXISTING_SCENE_DATA"; p.phase = Impl::Phase::Idle; return; }
        for (const auto& room : p.rooms) if (!p.Ready(room.space, XR_SPACE_COMPONENT_TYPE_ROOM_LAYOUT_FB)) return;
        std::vector<XrUuidEXT> ids;
        for (const auto& room : p.rooms) {
            XrRoomLayoutFB layout{XR_TYPE_ROOM_LAYOUT_FB};
            if (!p.Check("xrGetSpaceRoomLayoutFB(count)", p.layout(p.session, room.space, &layout))) return;
            if (layout.wallUuidCountOutput > 512) { p.Fail("Room wall count exceeds limit"); return; }
            std::vector<XrUuidEXT> walls(std::max(1u, layout.wallUuidCountOutput));
            layout.wallUuidCapacityInput = static_cast<uint32_t>(walls.size()); layout.wallUuids = walls.data();
            if (!p.Check("xrGetSpaceRoomLayoutFB(data)", p.layout(p.session, room.space, &layout))) return;
            if (Nonzero(layout.ceilingUuid) && p.ceilingRooms.emplace(Uuid(layout.ceilingUuid), Uuid(room.uuid)).second) ids.push_back(layout.ceilingUuid);
            if (Nonzero(layout.floorUuid) && p.ceilingRooms.emplace(Uuid(layout.floorUuid), Uuid(room.uuid)).second) ids.push_back(layout.floorUuid);
        }
        if (ids.empty()) { p.status = "ROOMS_WITHOUT_CEILING_UUID"; p.phase = Impl::Phase::Idle; return; }
        p.Query(false, &ids); return;
    }
    if (p.phase == Impl::Phase::Ceilings && p.ceilingComplete) {
        if (p.ceilings.empty()) { p.status = "CEILING_UUIDS_NOT_LOADED"; p.phase = Impl::Phase::Idle; return; }
        for (auto& ceiling : p.ceilings) if (!p.Metadata(ceiling)) return;
        p.status = "READY"; p.phase = Impl::Phase::Ready;
    }
}
int SceneQuery::PlaneHeights(XrSpace space, XrTime time, float headY, float* ceilY, float* floorY,
                             float* outlineXZ, int maxPoints, int* pointCount) {
    auto& p = *impl_;
    if (pointCount) *pointCount = 0;
    if (p.phase != Impl::Phase::Ready || !space || time <= 0) return 0;
    int mask = 0; float bestCeil = 0, bestFloor = 0;
    std::vector<XrVector3f> ceilOutline, floorOutline;
    for (const auto& plane : p.ceilings) {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(plane.space, space, time, &location))) continue;
        constexpr auto validFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if ((location.locationFlags & validFlags) != validFlags || !ValidPose(location.pose)) continue;
        const float y = location.pose.position.y;
        auto outline = [&](std::vector<XrVector3f>& out) {
            out.clear();
            for (const auto xy : plane.boundary) out.push_back(Transform(location.pose, xy));
        };
        if (CeilingLabel(plane.labels) && y > headY && (!(mask & 1) || y < bestCeil)) { bestCeil = y; mask |= 1; outline(ceilOutline); }
        if (HasLabel(plane.labels, "FLOOR") && y < headY && (!(mask & 2) || y > bestFloor)) { bestFloor = y; mask |= 2; outline(floorOutline); }
    }
    if (ceilY) *ceilY = bestCeil;
    if (floorY) *floorY = bestFloor;
    // Room outline (walls) from the ceiling, else the floor. Too many corners
    // to pass on: none, and the caller treats the room as unbounded.
    const auto& walls = !ceilOutline.empty() ? ceilOutline : floorOutline;
    if (outlineXZ && pointCount && static_cast<int>(walls.size()) <= maxPoints) {
        for (size_t i = 0; i < walls.size(); ++i) { outlineXZ[i * 2] = walls[i].x; outlineXZ[i * 2 + 1] = walls[i].z; }
        *pointCount = static_cast<int>(walls.size());
    }
    return mask;
}
std::string SceneQuery::SnapshotJson(XrSpace base, XrTime time) {
    auto& p = *impl_;
    if (p.phase != Impl::Phase::Ready) return p.FinishJson(p.status, "");
    if (!base || time <= 0) return p.FinishJson("INVALID_BASE_SPACE_OR_TIME", "");
    bool allValid = true; std::string records;
    for (const auto& ceiling : p.ceilings) {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        const XrResult result = xrLocateSpace(ceiling.space, base, time, &location);
        if (XR_FAILED(result)) p.Note("xrLocateSpace(CEILING,BASE)", result);
        constexpr auto validFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        constexpr auto trackedFlags = XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
        bool valid = XR_SUCCEEDED(result) && (location.locationFlags & validFlags) == validFlags && ValidPose(location.pose);
        std::vector<XrVector3f> vertices;
        if (valid) for (const auto xy : ceiling.boundary) {
            const auto vertex = Transform(location.pose, xy);
            if (!Finite(vertex)) { valid = false; vertices.clear(); break; }
            vertices.push_back(vertex);
        }
        allValid &= valid;
        const bool tracked = valid && (location.locationFlags & trackedFlags) == trackedFlags;
        std::ostringstream record; record.imbue(std::locale::classic()); record << std::setprecision(9);
        record << "{\"sceneUUID\":" << Quote(ceiling.scene) << ",\"ceilingUUID\":" << Quote(ceiling.uuid)
            << ",\"semanticLabels\":" << Quote(ceiling.labels) << ",\"locationFlags\":" << location.locationFlags
            << ",\"poseValid\":" << (valid ? "true" : "false") << ",\"poseTracked\":" << (tracked ? "true" : "false")
            << ",\"xrTime\":" << time << ",\"planePose\":";
        if (valid) {
            const auto& pos = location.pose.position; const auto& q = location.pose.orientation;
            record << "{\"position\":[" << pos.x << ',' << pos.y << ',' << pos.z << "],\"orientation\":[" << q.x << ',' << q.y << ',' << q.z << ',' << q.w << "]}";
        } else record << "null";
        record << ",\"polygon2D\":[";
        for (size_t i = 0; i < ceiling.boundary.size(); ++i) { if (i) record << ','; record << '[' << ceiling.boundary[i].x << ',' << ceiling.boundary[i].y << ']'; }
        record << "],\"polygon3DBase\":[";
        if (valid) for (size_t i = 0; i < vertices.size(); ++i) { if (i) record << ','; record << '[' << vertices[i].x << ',' << vertices[i].y << ',' << vertices[i].z << ']'; }
        record << "]}";
        if (records.size() + record.str().size() + 1 > kGeometryBytes) return p.FinishJson("OUTPUT_LIMIT_EXCEEDED", "");
        if (!records.empty()) records += ',';
        records += record.str();
    }
    return p.FinishJson(allValid && !p.ceilings.empty() ? "SUCCESS_CEILING_DATA" : "WAITING_FOR_VALID_CEILING_POSE", records);
}
}
