// SPDX-License-Identifier: GPL-3.0-or-later
// CPU regression of the real host query state machine using synthetic OpenXR
// responses. This does not measure or certify Quest tracking or Scene access.
#include "gcc_scene_query.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <vector>

namespace {
template<class T> T Handle(uintptr_t number) { return reinterpret_cast<T>(number); }
XrUuidEXT Id(unsigned char number) { XrUuidEXT id{}; id.data[0] = number; return id; }
struct Fake {
    XrAsyncRequestIdFB next = 10, last = 0;
    std::map<XrAsyncRequestIdFB, bool> rooms;
    std::vector<XrSpace> destroyed;
    XrSpace lastBase = XR_NULL_HANDLE;
    XrTime lastTime = 0;
    int layoutDataCalls = 0, locateCalls = 0;
    bool badSecondBoundaryCount = false, badBoundary = false, componentsEnabled = true;
    XrPosef pose{{0,0,0,1},{1,2,3}};
    XrSpaceLocationFlags flags = 15;
    XrResult locateResult = XR_SUCCESS;
} fake;

XrResult XRAPI_PTR Query(XrSession, const XrSpaceQueryInfoBaseHeaderFB* header, XrAsyncRequestIdFB* request) {
    const auto& info = *reinterpret_cast<const XrSpaceQueryInfoFB*>(header);
    assert(info.maxResultCount == 64 && info.queryAction == XR_SPACE_QUERY_ACTION_LOAD_FB);
    const bool room = info.filter->type == XR_TYPE_SPACE_COMPONENT_FILTER_INFO_FB;
    if (!room) {
        const auto* filter = reinterpret_cast<const XrSpaceUuidFilterInfoFB*>(info.filter);
        assert(filter->uuidCount == 1 && filter->uuids[0].data[0] == 22);
        assert(fake.layoutDataCalls != 0); // UUID from the data call, not undefined count call.
    }
    fake.last = *request = ++fake.next; fake.rooms[*request] = room; return XR_SUCCESS;
}
XrResult XRAPI_PTR Retrieve(XrSession, XrAsyncRequestIdFB id, XrSpaceQueryResultsFB* result) {
    assert(fake.rooms.count(id)); result->resultCountOutput = 1;
    if (result->resultCapacityInput) {
        result->results[0].space = Handle<XrSpace>(1000 + static_cast<uintptr_t>(id));
        result->results[0].uuid = Id(fake.rooms.at(id) ? 11 : 22);
    }
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Enumerate(XrSpace, uint32_t capacity, uint32_t* count, XrSpaceComponentTypeFB* types) {
    const XrSpaceComponentTypeFB all[] = {XR_SPACE_COMPONENT_TYPE_ROOM_LAYOUT_FB,
        XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB, XR_SPACE_COMPONENT_TYPE_BOUNDED_2D_FB,
        XR_SPACE_COMPONENT_TYPE_SEMANTIC_LABELS_FB};
    *count = 4; if (capacity) { assert(capacity >= 4); std::copy(all, all+4, types); } return XR_SUCCESS;
}
XrResult XRAPI_PTR Component(XrSpace, XrSpaceComponentTypeFB, XrSpaceComponentStatusFB* status) {
    status->enabled = fake.componentsEnabled; status->changePending = XR_FALSE; return XR_SUCCESS;
}
XrResult XRAPI_PTR SetComponent(XrSpace, const XrSpaceComponentStatusSetInfoFB*, XrAsyncRequestIdFB* request) {
    *request = 777; return XR_SUCCESS;
}
XrResult XRAPI_PTR Layout(XrSession, XrSpace, XrRoomLayoutFB* layout) {
    layout->wallUuidCountOutput = 0;
    layout->ceilingUuid = Id(0); // Explicitly undefined until a nonzero-capacity call.
    if (layout->wallUuidCapacityInput) { ++fake.layoutDataCalls; layout->ceilingUuid = Id(22); }
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Labels(XrSession, XrSpace, XrSemanticLabelsFB* labels) {
    labels->bufferCountOutput = 8;
    if (labels->bufferCapacityInput) std::memcpy(labels->buffer, "CEILING", 8);
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Boundary(XrSession, XrSpace, XrBoundary2DFB* boundary) {
    boundary->vertexCountOutput = 4;
    if (boundary->vertexCapacityInput) {
        const XrVector2f vertices[] = {{0,0},{1,0},{1,1},{0,1}};
        std::copy(vertices, vertices+4, boundary->vertices);
        if (fake.badBoundary) boundary->vertices[2].x = std::numeric_limits<float>::quiet_NaN();
        if (fake.badSecondBoundaryCount) boundary->vertexCountOutput = 2;
    }
    return XR_SUCCESS;
}
template<class T> void Event(gcc_host::SceneQuery& scene, const T& event) {
    XrEventDataBuffer buffer{XR_TYPE_EVENT_DATA_BUFFER};
    static_assert(sizeof(T) <= sizeof buffer, "event fits");
    std::memcpy(&buffer, &event, sizeof event); scene.OnEvent(buffer);
}
void Available(gcc_host::SceneQuery& scene, XrAsyncRequestIdFB id) {
    XrEventDataSpaceQueryResultsAvailableFB event{XR_TYPE_EVENT_DATA_SPACE_QUERY_RESULTS_AVAILABLE_FB};
    event.requestId = id; Event(scene, event);
}
void Complete(gcc_host::SceneQuery& scene, XrAsyncRequestIdFB id, XrResult result = XR_SUCCESS) {
    XrEventDataSpaceQueryCompleteFB event{XR_TYPE_EVENT_DATA_SPACE_QUERY_COMPLETE_FB};
    event.requestId = id; event.result = result; Event(scene, event);
}
void Init(gcc_host::SceneQuery& scene) {
    assert(scene.Initialize(Handle<XrInstance>(1), Handle<XrSession>(2)));
    scene.RequestRefresh(); scene.Tick(100); assert(scene.Status() == "QUERYING_ROOMS");
}
void Ready(gcc_host::SceneQuery& scene) {
    Init(scene); const auto room = fake.last;
    Available(scene, room); Complete(scene, room); scene.Tick(101);
    assert(scene.Status() == "QUERYING_CEILINGS");
    const auto ceiling = fake.last; Available(scene, ceiling); Complete(scene, ceiling); scene.Tick(102);
}
bool Has(const std::string& json, const char* needle) { return json.find(needle) != std::string::npos; }
void Invalid(gcc_host::SceneQuery& scene) {
    const auto json = scene.SnapshotJson(Handle<XrSpace>(55), 1234);
    assert(Has(json, "WAITING_FOR_VALID_CEILING_POSE"));
    assert(Has(json, "\"planePose\":null") && Has(json, "\"polygon3DBase\":[]"));
    assert(Has(json, "\"poseValid\":false") && Has(json, "\"poseTracked\":false"));
    assert(!Has(json, "nan") && !Has(json, "inf") && !Has(json, "SUCCESS_CEILING_DATA"));
}
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance, const char* name, PFN_xrVoidFunction* function) {
#define MAP(api, mock) if (std::strcmp(name, #api) == 0) { *function = reinterpret_cast<PFN_xrVoidFunction>(mock); return XR_SUCCESS; }
    MAP(xrQuerySpacesFB, Query) MAP(xrRetrieveSpaceQueryResultsFB, Retrieve)
    MAP(xrGetSpaceComponentStatusFB, Component) MAP(xrSetSpaceComponentStatusFB, SetComponent)
    MAP(xrEnumerateSpaceSupportedComponentsFB, Enumerate) MAP(xrGetSpaceRoomLayoutFB, Layout)
    MAP(xrGetSpaceSemanticLabelsFB, Labels) MAP(xrGetSpaceBoundary2DFB, Boundary)
#undef MAP
    *function = nullptr; return XR_ERROR_FUNCTION_UNSUPPORTED;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrResultToString(XrInstance, XrResult result, char buffer[XR_MAX_RESULT_STRING_SIZE]) {
    std::snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "mock_%d", static_cast<int>(result)); return XR_SUCCESS;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrDestroySpace(XrSpace space) {
    assert(std::find(fake.destroyed.begin(), fake.destroyed.end(), space) == fake.destroyed.end());
    fake.destroyed.push_back(space); return XR_SUCCESS;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpace(XrSpace, XrSpace base, XrTime time, XrSpaceLocation* location) {
    ++fake.locateCalls; fake.lastBase = base; fake.lastTime = time;
    location->locationFlags = fake.flags; location->pose = fake.pose; return fake.locateResult;
}

int main() {
    {
        gcc_host::SceneQuery scene; Ready(scene); assert(scene.Status() == "READY");
        auto json = scene.SnapshotJson(Handle<XrSpace>(55), 1234);
        assert(Has(json, "SUCCESS_CEILING_DATA"));
        assert(Has(json, "\"polygon3DBase\":[[1,2,3],[2,2,3],[2,3,3],[1,3,3]]"));
        assert(fake.lastBase == Handle<XrSpace>(55) && fake.lastTime == 1234);
        fake.pose.orientation = {0,0,1,0}; // Exact 180-degree rotation around local Z.
        json = scene.SnapshotJson(Handle<XrSpace>(55), 1234);
        assert(Has(json, "\"polygon3DBase\":[[1,2,3],[0,2,3],[0,1,3],[1,1,3]]"));
        fake.pose.orientation = {0,0,0,1};
        assert(Has(scene.SnapshotJson(XR_NULL_HANDLE, 1234), "INVALID_BASE_SPACE_OR_TIME"));
        fake.flags = 0; Invalid(scene); fake.flags = 15;
        fake.pose.position.x = std::numeric_limits<float>::quiet_NaN(); Invalid(scene); fake.pose.position.x = 1;
        fake.pose.orientation.w = 2; Invalid(scene); fake.pose.orientation.w = 1;
        fake.locateResult = XR_ERROR_RUNTIME_FAILURE; Invalid(scene); fake.locateResult = XR_SUCCESS;
        // Fresh locate calls recover without a cached-invalid or cached-valid pose.
        assert(Has(scene.SnapshotJson(Handle<XrSpace>(77), 1235), "SUCCESS_CEILING_DATA"));
        assert(fake.lastBase == Handle<XrSpace>(77) && fake.lastTime == 1235);
        Complete(scene, 999, XR_ERROR_RUNTIME_FAILURE);
        XrEventDataSpaceSetStatusCompleteFB foreign{XR_TYPE_EVENT_DATA_SPACE_SET_STATUS_COMPLETE_FB};
        foreign.requestId = 999; foreign.result = XR_ERROR_RUNTIME_FAILURE; Event(scene, foreign);
        assert(scene.Status() == "READY");
        scene.RequestRefresh(); assert(fake.destroyed.size() == 2);
        scene.Tick(200); const auto retired = fake.last;
        scene.RequestRefresh(); scene.Tick(201); const auto active = fake.last;
        Available(scene, retired); Complete(scene, retired, XR_ERROR_RUNTIME_FAILURE);
        assert(fake.destroyed.size() == 3 && scene.Status() == "QUERYING_ROOMS");
        Available(scene, active); Complete(scene, active); scene.Tick(202);
        Available(scene, fake.last); Complete(scene, fake.last); scene.Tick(203);
        assert(scene.Status() == "READY");
        scene.Shutdown(); assert(fake.destroyed.size() == 5);
        scene.RequestRefresh(); assert(Has(scene.Status(), "not initialized"));
    }
    for (int bad = 0; bad != 2; ++bad) {
        fake = Fake{}; fake.badBoundary = bad == 0; fake.badSecondBoundaryCount = bad == 1;
        gcc_host::SceneQuery scene; Ready(scene);
        assert(scene.Status().find("ERROR:") == 0);
        const auto json = scene.SnapshotJson(Handle<XrSpace>(55), 1234);
        assert(Has(json, "\"ceilings\":[]") && !Has(json, "nan") && !Has(json, "SUCCESS_CEILING_DATA"));
        assert(fake.locateCalls == 0);
    }
    {
        fake = Fake{}; fake.componentsEnabled = false;
        gcc_host::SceneQuery scene; Init(scene);
        Available(scene, fake.last); Complete(scene, fake.last); scene.Tick(101);
        assert(scene.Status() == "QUERYING_ROOMS");
        XrEventDataSpaceSetStatusCompleteFB event{XR_TYPE_EVENT_DATA_SPACE_SET_STATUS_COMPLETE_FB};
        event.requestId = 999; event.result = XR_ERROR_RUNTIME_FAILURE; Event(scene, event);
        assert(scene.Status() == "QUERYING_ROOMS");
        event.requestId = 777; Event(scene, event);
        assert(scene.Status().find("ERROR:") == 0);
    }
    {
        fake = Fake{};
        gcc_host::SceneQuery scene; Init(scene);
        const auto oldest = fake.last;
        for(unsigned i=0;i<16;++i) { scene.RequestRefresh();scene.Tick(200+i); }
        assert(scene.Status()=="WAITING_FOR_PREVIOUS_SCENE_QUERIES");
        const auto boundedRequests = fake.next;
        assert(boundedRequests==26); // Initial query plus15 replacements, never17 outstanding.
        for(unsigned i=0;i<100;++i) { scene.RequestRefresh();scene.Tick(300+i); }
        assert(fake.next==boundedRequests && !Has(scene.Status(),"ERROR"));
        assert(Has(scene.SnapshotJson(Handle<XrSpace>(55),500),"\"ceilings\":[]"));
        // No more input/refresher calls: completing a retired request alone
        // must allow the next Tick to resume the newest requested capture.
        Available(scene,oldest);Complete(scene,oldest);scene.Tick(501);
        assert(scene.Status()=="QUERYING_ROOMS" && fake.next==boundedRequests+1);
        assert(fake.destroyed.size()==1);
        Available(scene,fake.last);Complete(scene,fake.last);scene.Tick(502);
        Available(scene,fake.last);Complete(scene,fake.last);scene.Tick(503);
        assert(scene.Status()=="READY");
        assert(Has(scene.SnapshotJson(Handle<XrSpace>(55),504),"SUCCESS_CEILING_DATA"));
    }
    std::puts("PASS: SceneQuery CPU regression (fresh base/time, invalid poses, metadata bounds, foreign/retired events, ownership). No headset performance claim.");
    return 0;
}
