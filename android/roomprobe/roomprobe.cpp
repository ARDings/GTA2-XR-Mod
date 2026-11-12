#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
JavaVM* vm = nullptr;
std::atomic<bool> stopRequested{false}, resumed{false}, workerActive{false};
using Clock = std::chrono::steady_clock;
constexpr XrDuration seconds(int n) { return static_cast<XrDuration>(n) * 1000000000; }
std::string quoted(const std::string& value) {
    std::string result = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 32) { char escaped[7]; std::snprintf(escaped, sizeof(escaped), "\\u%04x", c); result += escaped; }
        else result += static_cast<char>(c);
    }
    return result + '"';
}
std::string uuidText(const XrUuidEXT& id) {
    char text[33];
    for (unsigned i = 0; i < 16; ++i) std::snprintf(text + i * 2, 3, "%02x", id.data[i]);
    return text;
}
bool nonzero(const XrUuidEXT& id) {
    for (auto value : id.data) if (value) return true;
    return false;
}
bool ceilingLabel(const std::string& labels) {
    std::istringstream stream(labels);
    std::string token;
    while (std::getline(stream, token, ',')) {
        const auto first = token.find_first_not_of(" \t\r\n");
        const auto last = token.find_last_not_of(" \t\r\n");
        if (first != std::string::npos && token.substr(first, last - first + 1) == "CEILING") return true;
    }
    return false;
}
XrVector3f transform(const XrPosef& pose, XrVector2f xy) {
    const auto& q = pose.orientation;
    const XrVector3f v{xy.x, xy.y, 0};
    const XrVector3f t{2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z), 2 * (q.x * v.y - q.y * v.x)};
    return {pose.position.x + v.x + q.w * t.x + q.y * t.z - q.z * t.y,
            pose.position.y + v.y + q.w * t.y + q.z * t.x - q.x * t.z,
            pose.position.z + v.z + q.w * t.z + q.x * t.y - q.y * t.x};
}
struct ApiEvent { std::string api, name; XrResult result; };
struct Ceiling {
    XrSpace space = XR_NULL_HANDLE;
    std::string scene, uuid, labels;
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    XrTime time = 0;
    bool metadata = false, valid = false, tracked = false;
    std::vector<XrVector2f> boundary;
};

struct Probe {
    jobject activity;
    std::string reportPath, status = "INITIALIZING", runtimeName;
    XrVersion runtimeVersion = 0;
    std::vector<ApiEvent> events;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace stage = XR_NULL_HANDLE;
    XrExtent2Df stageBounds{};
    XrResult boundsResult = XR_ERROR_CALL_ORDER_INVALID;
    XrResult sceneResult = XR_ERROR_CALL_ORDER_INVALID;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLConfig config = nullptr;
    bool running = false, done = false, roomDone = false, ceilingDone = false, ceilingIssued = false;
    XrAsyncRequestIdFB roomRequest = 0, ceilingRequest = 0;
    XrTime initialStageStableAfter = 0;
    std::vector<XrSpaceQueryResultFB> rooms;
    std::vector<Ceiling> ceilings;
    std::map<std::string, std::string> ceilingRooms;
    std::set<std::pair<XrSpace, XrSpaceComponentTypeFB>> enabling;
    std::map<XrSpace, std::vector<XrSpaceComponentTypeFB>> supportedComponents;
    std::set<XrSpace> ownedSpaces;
    XrPassthroughFB passthrough = XR_NULL_HANDLE;
    XrPassthroughLayerFB passthroughLayer = XR_NULL_HANDLE;
    PFN_xrQuerySpacesFB query = nullptr;
    PFN_xrRetrieveSpaceQueryResultsFB retrieve = nullptr;
    PFN_xrGetSpaceComponentStatusFB componentStatus = nullptr;
    PFN_xrSetSpaceComponentStatusFB componentSet = nullptr;
    PFN_xrEnumerateSpaceSupportedComponentsFB enumerateComponents = nullptr;
    PFN_xrGetSpaceRoomLayoutFB roomLayout = nullptr;
    PFN_xrGetSpaceSemanticLabelsFB labels = nullptr;
    PFN_xrGetSpaceBoundary2DFB boundary = nullptr;
    PFN_xrDestroyPassthroughFB destroyPassthrough = nullptr;
    PFN_xrDestroyPassthroughLayerFB destroyPassthroughLayer = nullptr;

    void note(const char* api, XrResult result) {
        char name[XR_MAX_RESULT_STRING_SIZE]{};
        if (instance) xrResultToString(instance, result, name);
        if (!name[0]) std::snprintf(name, sizeof(name), "XrResult(%d)", static_cast<int>(result));
        events.push_back({api, name, result});
        __android_log_print(ANDROID_LOG_INFO, "GTA2RoomProbe", "%s: %s (%d)", api, name, result);
    }
    void check(const char* api, XrResult result) {
        note(api, result);
        if (XR_FAILED(result)) throw std::runtime_error(std::string(api) + ": " + events.back().name);
    }
    template<class T> T function(const char* name) {
        PFN_xrVoidFunction value = nullptr;
        check(name, xrGetInstanceProcAddr(instance, name, &value));
        if (!value) throw std::runtime_error(std::string("Missing function: ") + name);
        return reinterpret_cast<T>(value);
    }
    void init() {
        PFN_xrInitializeLoaderKHR initialize = nullptr;
        check("xrGetInstanceProcAddr(xrInitializeLoaderKHR)", xrGetInstanceProcAddr(XR_NULL_HANDLE,
              "xrInitializeLoaderKHR", reinterpret_cast<PFN_xrVoidFunction*>(&initialize)));
        XrLoaderInitInfoAndroidKHR loader{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        loader.applicationVM = vm; loader.applicationContext = activity;
        check("xrInitializeLoaderKHR", initialize(reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR*>(&loader)));
        uint32_t count = 0;
        check("xrEnumerateInstanceExtensionProperties(count)", xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr));
        std::vector<XrExtensionProperties> available(count, {XR_TYPE_EXTENSION_PROPERTIES});
        check("xrEnumerateInstanceExtensionProperties(data)", xrEnumerateInstanceExtensionProperties(nullptr, count, &count, available.data()));
        const auto has = [&](const char* name) { return std::any_of(available.begin(), available.end(),
            [&](const auto& value) { return std::strcmp(name, value.extensionName) == 0; }); };
        std::vector<const char*> required{XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
            XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME, XR_FB_SPATIAL_ENTITY_EXTENSION_NAME,
            XR_FB_SPATIAL_ENTITY_QUERY_EXTENSION_NAME, XR_FB_SCENE_EXTENSION_NAME};
        for (const auto* extension : required) if (!has(extension)) throw std::runtime_error(std::string("Missing extension: ") + extension);
        const bool passthroughSupported = has(XR_FB_PASSTHROUGH_EXTENSION_NAME);
        if (passthroughSupported) required.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
        XrInstanceCreateInfoAndroidKHR android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
        android.applicationVM = vm; android.applicationActivity = activity;
        XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO}; create.next = &android;
        std::strcpy(create.applicationInfo.applicationName, "GTA2CeilingRoomProbe");
        std::strcpy(create.applicationInfo.engineName, "GTA2Ceiling");
        create.applicationInfo.applicationVersion = 1;
        create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        create.enabledExtensionCount = static_cast<uint32_t>(required.size()); create.enabledExtensionNames = required.data();
        check("xrCreateInstance", xrCreateInstance(&create, &instance));
        XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
        check("xrGetInstanceProperties", xrGetInstanceProperties(instance, &properties));
        runtimeName = properties.runtimeName; runtimeVersion = properties.runtimeVersion;
        XrSystemGetInfo getSystem{XR_TYPE_SYSTEM_GET_INFO}; getSystem.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        check("xrGetSystem", xrGetSystem(instance, &getSystem, &system));
        const auto graphics = function<PFN_xrGetOpenGLESGraphicsRequirementsKHR>("xrGetOpenGLESGraphicsRequirementsKHR");
        XrGraphicsRequirementsOpenGLESKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
        check("xrGetOpenGLESGraphicsRequirementsKHR", graphics(instance, system, &requirements));
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) throw std::runtime_error("EGL initialize failed");
        const EGLint configAttributes[]{EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
        EGLint configs = 0;
        if (!eglChooseConfig(display, configAttributes, &config, 1, &configs) || configs != 1) throw std::runtime_error("No EGL ES3 config");
        const EGLint contextAttributes[]{EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        const EGLint pbufferAttributes[]{EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
        surface = eglCreatePbufferSurface(display, config, pbufferAttributes);
        if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE || !eglMakeCurrent(display, surface, surface, context))
            throw std::runtime_error("EGL context failed");
        XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
        binding.display = display; binding.config = config; binding.context = context;
        XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO}; sessionInfo.next = &binding; sessionInfo.systemId = system;
        check("xrCreateSession", xrCreateSession(instance, &sessionInfo, &session));
        uint32_t spaceCount = 0;
        check("xrEnumerateReferenceSpaces(count)", xrEnumerateReferenceSpaces(session, 0, &spaceCount, nullptr));
        std::vector<XrReferenceSpaceType> types(spaceCount);
        check("xrEnumerateReferenceSpaces(data)", xrEnumerateReferenceSpaces(session, spaceCount, &spaceCount, types.data()));
        if (std::find(types.begin(), types.end(), XR_REFERENCE_SPACE_TYPE_STAGE) == types.end()) throw std::runtime_error("Natural STAGE unavailable; no LOCAL substitution");
        XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE; spaceInfo.poseInReferenceSpace.orientation.w = 1;
        check("xrCreateReferenceSpace(NATURAL_STAGE)", xrCreateReferenceSpace(session, &spaceInfo, &stage));
        boundsResult = xrGetReferenceSpaceBoundsRect(session, XR_REFERENCE_SPACE_TYPE_STAGE, &stageBounds);
        note("xrGetReferenceSpaceBoundsRect(STAGE)", boundsResult);
        query = function<PFN_xrQuerySpacesFB>("xrQuerySpacesFB");
        retrieve = function<PFN_xrRetrieveSpaceQueryResultsFB>("xrRetrieveSpaceQueryResultsFB");
        componentStatus = function<PFN_xrGetSpaceComponentStatusFB>("xrGetSpaceComponentStatusFB");
        componentSet = function<PFN_xrSetSpaceComponentStatusFB>("xrSetSpaceComponentStatusFB");
        enumerateComponents = function<PFN_xrEnumerateSpaceSupportedComponentsFB>("xrEnumerateSpaceSupportedComponentsFB");
        roomLayout = function<PFN_xrGetSpaceRoomLayoutFB>("xrGetSpaceRoomLayoutFB");
        labels = function<PFN_xrGetSpaceSemanticLabelsFB>("xrGetSpaceSemanticLabelsFB");
        boundary = function<PFN_xrGetSpaceBoundary2DFB>("xrGetSpaceBoundary2DFB");
        if (passthroughSupported) {
            const auto make = function<PFN_xrCreatePassthroughFB>("xrCreatePassthroughFB");
            const auto makeLayer = function<PFN_xrCreatePassthroughLayerFB>("xrCreatePassthroughLayerFB");
            destroyPassthrough = function<PFN_xrDestroyPassthroughFB>("xrDestroyPassthroughFB");
            destroyPassthroughLayer = function<PFN_xrDestroyPassthroughLayerFB>("xrDestroyPassthroughLayerFB");
            XrPassthroughCreateInfoFB info{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB}; info.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
            const XrResult result = make(session, &info, &passthrough); note("xrCreatePassthroughFB", result);
            if (XR_SUCCEEDED(result)) {
                XrPassthroughLayerCreateInfoFB layer{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
                layer.passthrough = passthrough; layer.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
                layer.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
                note("xrCreatePassthroughLayerFB", makeLayer(session, &layer, &passthroughLayer));
            }
        }
        status = "WAITING_FOR_XR_SESSION";
    }

    void startQuery(bool forRooms, std::vector<XrUuidEXT>* ids = nullptr) {
        XrSpaceStorageLocationFilterInfoFB storage{XR_TYPE_SPACE_STORAGE_LOCATION_FILTER_INFO_FB};
        storage.location = XR_SPACE_STORAGE_LOCATION_LOCAL_FB;
        XrSpaceComponentFilterInfoFB component{XR_TYPE_SPACE_COMPONENT_FILTER_INFO_FB};
        component.componentType = XR_SPACE_COMPONENT_TYPE_ROOM_LAYOUT_FB; component.next = &storage;
        XrSpaceUuidFilterInfoFB uuids{XR_TYPE_SPACE_UUID_FILTER_INFO_FB}; uuids.next = &storage;
        if (ids) { uuids.uuidCount = static_cast<uint32_t>(ids->size()); uuids.uuids = ids->data(); }
        XrSpaceQueryInfoFB info{XR_TYPE_SPACE_QUERY_INFO_FB};
        info.queryAction = XR_SPACE_QUERY_ACTION_LOAD_FB; info.maxResultCount = 64; info.timeout = seconds(15);
        info.filter = forRooms ? reinterpret_cast<XrSpaceFilterInfoBaseHeaderFB*>(&component) : reinterpret_cast<XrSpaceFilterInfoBaseHeaderFB*>(&uuids);
        const XrResult result = query(session, reinterpret_cast<XrSpaceQueryInfoBaseHeaderFB*>(&info), forRooms ? &roomRequest : &ceilingRequest);
        sceneResult = result; check(forRooms ? "xrQuerySpacesFB(ROOM_LAYOUT)" : "xrQuerySpacesFB(CEILING_UUID)", result);
        status = forRooms ? "QUERYING_EXISTING_ROOMS" : "QUERYING_CEILING_UUIDS";
    }
    void results(XrAsyncRequestIdFB request) {
        if (request != roomRequest && request != ceilingRequest) return;
        XrSpaceQueryResultsFB result{XR_TYPE_SPACE_QUERY_RESULTS_FB};
        check("xrRetrieveSpaceQueryResultsFB(count)", retrieve(session, request, &result));
        if (result.resultCountOutput > 64) throw std::runtime_error("Scene query exceeded bounded result limit");
        std::vector<XrSpaceQueryResultFB> items(result.resultCountOutput);
        result.resultCapacityInput = static_cast<uint32_t>(items.size()); result.results = items.data();
        check("xrRetrieveSpaceQueryResultsFB(data)", retrieve(session, request, &result));
        if (result.resultCountOutput > items.size()) throw std::runtime_error("Scene query result count changed");
        items.resize(result.resultCountOutput);
        for (const auto& item : items) {
            if (!ownedSpaces.insert(item.space).second) continue;
            if (request == roomRequest) rooms.push_back(item);
            else {
                Ceiling ceiling; ceiling.space = item.space; ceiling.uuid = uuidText(item.uuid);
                auto room = ceilingRooms.find(ceiling.uuid);
                if (room == ceilingRooms.end()) throw std::runtime_error("Unexpected ceiling UUID result");
                ceiling.scene = room->second; ceilings.push_back(ceiling);
            }
        }
    }
    bool ready(XrSpace space, XrSpaceComponentTypeFB type) {
        auto supported = supportedComponents.find(space);
        if (supported == supportedComponents.end()) {
            uint32_t count = 0;
            check("xrEnumerateSpaceSupportedComponentsFB(count)", enumerateComponents(space, 0, &count, nullptr));
            if (count > 64) throw std::runtime_error("Unexpected component count");
            std::vector<XrSpaceComponentTypeFB> types(count);
            check("xrEnumerateSpaceSupportedComponentsFB(data)", enumerateComponents(space, count, &count, types.data()));
            if (count > types.size()) throw std::runtime_error("Component count changed during read");
            types.resize(count); supported = supportedComponents.emplace(space, std::move(types)).first;
        }
        if (std::find(supported->second.begin(), supported->second.end(), type) == supported->second.end())
            throw std::runtime_error("Scene anchor lacks component " + std::to_string(static_cast<int>(type)));
        XrSpaceComponentStatusFB state{XR_TYPE_SPACE_COMPONENT_STATUS_FB};
        const XrResult result = componentStatus(space, type, &state);
        if (XR_FAILED(result)) check("xrGetSpaceComponentStatusFB", result);
        if (state.enabled) return true;
        if (!state.changePending && enabling.insert({space, type}).second) {
            XrSpaceComponentStatusSetInfoFB set{XR_TYPE_SPACE_COMPONENT_STATUS_SET_INFO_FB};
            set.componentType = type; set.enabled = XR_TRUE; set.timeout = seconds(10);
            XrAsyncRequestIdFB request = 0;
            check("xrSetSpaceComponentStatusFB", componentSet(space, &set, &request));
        }
        return false;
    }
    void advance(XrTime time) {
        if (!roomRequest && time < initialStageStableAfter) { status = "WAITING_FOR_INITIAL_STAGE_ORIGIN"; return; }
        if (!roomRequest) startQuery(true);
        if (!roomDone) return;
        if (rooms.empty()) { status = "NO_EXISTING_SCENE_DATA"; done = true; return; }
        if (!ceilingIssued) {
            for (const auto& room : rooms) if (!ready(room.space, XR_SPACE_COMPONENT_TYPE_ROOM_LAYOUT_FB)) return;
            std::vector<XrUuidEXT> ids;
            for (const auto& room : rooms) {
                XrRoomLayoutFB layout{XR_TYPE_ROOM_LAYOUT_FB};
                check("xrGetSpaceRoomLayoutFB(count)", roomLayout(session, room.space, &layout));
                if (layout.wallUuidCountOutput > 512) throw std::runtime_error("Room exceeds wall UUID limit");
                std::vector<XrUuidEXT> walls(std::max(1u, layout.wallUuidCountOutput));
                layout.wallUuidCapacityInput = static_cast<uint32_t>(walls.size()); layout.wallUuids = walls.data();
                check("xrGetSpaceRoomLayoutFB(data)", roomLayout(session, room.space, &layout));
                // The UUID fields are only defined by the successful data call.
                if (nonzero(layout.ceilingUuid) && ceilingRooms.emplace(uuidText(layout.ceilingUuid), uuidText(room.uuid)).second)
                    ids.push_back(layout.ceilingUuid);
            }
            ceilingIssued = true;
            if (ids.empty()) { status = "ROOMS_WITHOUT_CEILING_UUID"; done = true; return; }
            startQuery(false, &ids);
            return;
        }
        if (!ceilingDone) return;
        if (ceilings.empty()) { status = "CEILING_UUIDS_NOT_LOADED"; done = true; return; }
        bool allReady = true;
        for (auto& ceiling : ceilings) {
            if (!ready(ceiling.space, XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB) ||
                !ready(ceiling.space, XR_SPACE_COMPONENT_TYPE_BOUNDED_2D_FB) ||
                !ready(ceiling.space, XR_SPACE_COMPONENT_TYPE_SEMANTIC_LABELS_FB)) { allReady = false; continue; }
            if (!ceiling.metadata) {
                XrSemanticLabelsFB semantic{XR_TYPE_SEMANTIC_LABELS_FB};
                check("xrGetSpaceSemanticLabelsFB(count)", labels(session, ceiling.space, &semantic));
                if (semantic.bufferCountOutput > 4096) throw std::runtime_error("Semantic label exceeds limit");
                std::vector<char> text(std::max(1u, semantic.bufferCountOutput) + 1, 0);
                semantic.bufferCapacityInput = static_cast<uint32_t>(text.size()); semantic.buffer = text.data();
                check("xrGetSpaceSemanticLabelsFB(data)", labels(session, ceiling.space, &semantic));
                ceiling.labels = text.data();
                if (!ceilingLabel(ceiling.labels)) throw std::runtime_error("Room ceiling UUID lacks CEILING label");
                XrBoundary2DFB polygon{XR_TYPE_BOUNDARY_2D_FB};
                check("xrGetSpaceBoundary2DFB(count)", boundary(session, ceiling.space, &polygon));
                if (polygon.vertexCountOutput < 3 || polygon.vertexCountOutput > 8192) throw std::runtime_error("Invalid ceiling polygon size");
                ceiling.boundary.resize(polygon.vertexCountOutput);
                polygon.vertexCapacityInput = static_cast<uint32_t>(ceiling.boundary.size()); polygon.vertices = ceiling.boundary.data();
                check("xrGetSpaceBoundary2DFB(data)", boundary(session, ceiling.space, &polygon));
                if (polygon.vertexCountOutput < 3 || polygon.vertexCountOutput > ceiling.boundary.size()) throw std::runtime_error("Ceiling polygon changed during read");
                ceiling.boundary.resize(polygon.vertexCountOutput);
                for (const auto& xy : ceiling.boundary) if (!std::isfinite(xy.x) || !std::isfinite(xy.y)) throw std::runtime_error("Nonfinite ceiling polygon");
                ceiling.metadata = true;
            }
            const XrResult result = xrLocateSpace(ceiling.space, stage, time, &ceiling.location);
            if (XR_FAILED(result)) check("xrLocateSpace(CEILING,NATURAL_STAGE)", result);
            const auto flags = ceiling.location.locationFlags;
            const auto valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            const auto tracked = XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
            ceiling.valid = (flags & valid) == valid;
            ceiling.tracked = (flags & tracked) == tracked;
            ceiling.time = time;
            if (ceiling.valid) {
                const auto& p = ceiling.location.pose.position;
                const auto& q = ceiling.location.pose.orientation;
                const float norm = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
                const bool finite = std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
                    std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w) && std::fabs(norm - 1.0f) < 0.01f;
                if (!finite) { ceiling.valid = false; throw std::runtime_error("Runtime supplied nonfinite or nonunit ceiling pose"); }
                for (const auto xy : ceiling.boundary) {
                    const auto vertex = transform(ceiling.location.pose, xy);
                    if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z)) {
                        ceiling.valid = false; throw std::runtime_error("Nonfinite stage-space ceiling vertex");
                    }
                }
            }
            if (!ceiling.valid) allReady = false;
        }
        if (allReady) { status = "SUCCESS_CEILING_DATA"; done = true; }
        else status = "WAITING_FOR_VALID_CEILING_POSE";
    }
    void poll() {
        XrEventDataBuffer buffer{XR_TYPE_EVENT_DATA_BUFFER};
        XrResult result;
        while ((result = xrPollEvent(instance, &buffer)) == XR_SUCCESS) {
            if (buffer.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto& event = *reinterpret_cast<XrEventDataSessionStateChanged*>(&buffer);
                __android_log_print(ANDROID_LOG_INFO, "GTA2RoomProbe", "Session state %d", event.state);
                if (event.state == XR_SESSION_STATE_READY && !running) {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO}; begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    check("xrBeginSession", xrBeginSession(session, &begin)); running = true;
                } else if (event.state == XR_SESSION_STATE_STOPPING && running) {
                    check("xrEndSession", xrEndSession(session)); running = false;
                } else if (event.state == XR_SESSION_STATE_EXITING || event.state == XR_SESSION_STATE_LOSS_PENDING)
                    throw std::runtime_error("OpenXR session exited before probe completion");
            } else if (buffer.type == XR_TYPE_EVENT_DATA_SPACE_QUERY_RESULTS_AVAILABLE_FB) {
                results(reinterpret_cast<XrEventDataSpaceQueryResultsAvailableFB*>(&buffer)->requestId);
            } else if (buffer.type == XR_TYPE_EVENT_DATA_SPACE_QUERY_COMPLETE_FB) {
                const auto& event = *reinterpret_cast<XrEventDataSpaceQueryCompleteFB*>(&buffer);
                if (event.requestId == roomRequest || event.requestId == ceilingRequest) {
                    sceneResult = event.result; check("XrEventDataSpaceQueryCompleteFB", event.result);
                    if (event.requestId == roomRequest) roomDone = true; else ceilingDone = true;
                }
            } else if (buffer.type == XR_TYPE_EVENT_DATA_SPACE_SET_STATUS_COMPLETE_FB) {
                const auto& event = *reinterpret_cast<XrEventDataSpaceSetStatusCompleteFB*>(&buffer);
                check("XrEventDataSpaceSetStatusCompleteFB", event.result);
            } else if (buffer.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
                const auto& event = *reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&buffer);
                __android_log_print(ANDROID_LOG_INFO, "GTA2RoomProbe", "Reference change type=%d time=%lld, queryStarted=%d",
                    event.referenceSpaceType, static_cast<long long>(event.changeTime), roomRequest != 0);
                // Runtimes announce initial/local origins during session startup.
                // The export uses STAGE only; wait for an initial STAGE origin,
                // and reject actual STAGE changes once a capture has started.
                if (event.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE) {
                    if (roomRequest) throw std::runtime_error("STAGE changed during capture; rerun probe");
                    initialStageStableAfter = std::max(initialStageStableAfter, event.changeTime);
                }
            }
            buffer = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (result != XR_EVENT_UNAVAILABLE) check("xrPollEvent", result);
    }
    void run() {
        init();
        const auto deadline = Clock::now() + std::chrono::seconds(60);
        while (!done && !stopRequested && Clock::now() < deadline) {
            poll();
            if (!running || !resumed) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
            XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO}; XrFrameState frame{XR_TYPE_FRAME_STATE};
            const auto waitResult = xrWaitFrame(session, &wait, &frame);
            if (XR_FAILED(waitResult)) check("xrWaitFrame", waitResult);
            XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
            const auto beginResult = xrBeginFrame(session, &begin);
            if (XR_FAILED(beginResult)) check("xrBeginFrame", beginResult);
            advance(frame.predictedDisplayTime);
            XrCompositionLayerPassthroughFB layer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
            layer.layerHandle = passthroughLayer;
            const XrCompositionLayerBaseHeader* layers[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO}; end.displayTime = frame.predictedDisplayTime;
            end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            if (passthroughLayer && frame.shouldRender) { end.layerCount = 1; end.layers = layers; }
            const auto endResult = xrEndFrame(session, &end);
            if (XR_FAILED(endResult)) check("xrEndFrame", endResult);
        }
        if (!done) status = stopRequested ? "CANCELLED" : "TIMED_OUT_WITHOUT_COMPLETE_CEILING_DATA";
    }
    void save() {
        std::ostringstream out; out.imbue(std::locale::classic()); out << std::setprecision(9);
        const auto unixMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        out << "{\"formatVersion\":1,\"source\":\"GTA2CeilingRoomProbe/OpenXR\",\"runtime\":{\"name\":" << quoted(runtimeName)
            << ",\"version\":" << quoted(std::to_string(XR_VERSION_MAJOR(runtimeVersion)) + "." + std::to_string(XR_VERSION_MINOR(runtimeVersion)) + "." + std::to_string(XR_VERSION_PATCH(runtimeVersion)))
            << "},\"coordinateSpace\":\"NATURAL_STAGE\",\"units\":\"meters\",\"status\":" << quoted(status)
            << ",\"sceneQueryResult\":" << sceneResult << ",\"captureUnixMs\":" << unixMs
            << ",\"stageBounds\":{\"result\":" << boundsResult << ",\"width\":" << stageBounds.width << ",\"depth\":" << stageBounds.height << "},\"ceilings\":[";
        for (size_t i = 0; i < ceilings.size(); ++i) {
            const auto& c = ceilings[i]; if (i) out << ',';
            out << "{\"sceneUUID\":" << quoted(c.scene) << ",\"ceilingUUID\":" << quoted(c.uuid) << ",\"semanticLabels\":" << quoted(c.labels)
                << ",\"locationFlags\":" << c.location.locationFlags << ",\"poseValid\":" << (c.valid ? "true" : "false")
                << ",\"poseTracked\":" << (c.tracked ? "true" : "false") << ",\"xrTime\":" << c.time << ",\"planePose\":";
            if (c.valid) {
                const auto& p = c.location.pose.position; const auto& q = c.location.pose.orientation;
                out << "{\"position\":[" << p.x << ',' << p.y << ',' << p.z << "],\"orientation\":[" << q.x << ',' << q.y << ',' << q.z << ',' << q.w << "]}";
            } else out << "null";
            out << ",\"polygon2D\":[";
            for (size_t v = 0; v < c.boundary.size(); ++v) { if (v) out << ','; out << '[' << c.boundary[v].x << ',' << c.boundary[v].y << ']'; }
            out << "],\"polygon3DStage\":[";
            if (c.valid) for (size_t v = 0; v < c.boundary.size(); ++v) {
                if (v) out << ','; const auto p = transform(c.location.pose, c.boundary[v]); out << '[' << p.x << ',' << p.y << ',' << p.z << ']';
            }
            out << "]}";
        }
        out << "],\"events\":[";
        for (size_t i = 0; i < events.size(); ++i) { if (i) out << ','; out << "{\"api\":" << quoted(events[i].api) << ",\"result\":" << events[i].result << ",\"resultName\":" << quoted(events[i].name) << '}'; }
        out << "]}\n";
        const auto temporary = reportPath + ".tmp";
        std::ofstream file(temporary, std::ios::binary); file << out.str(); file.close();
        if (!file || std::rename(temporary.c_str(), reportPath.c_str()) != 0) __android_log_print(ANDROID_LOG_ERROR, "GTA2RoomProbe", "Could not write report");
        __android_log_print(ANDROID_LOG_INFO, "GTA2RoomProbe", "Result %s; report %s; ceilings=%zu", status.c_str(), reportPath.c_str(), ceilings.size());
    }
    void close() {
        if (session && running) xrRequestExitSession(session);
        for (const auto space : ownedSpaces) xrDestroySpace(space);
        if (stage) xrDestroySpace(stage);
        if (passthroughLayer && destroyPassthroughLayer) destroyPassthroughLayer(passthroughLayer);
        if (passthrough && destroyPassthrough) destroyPassthrough(passthrough);
        if (session) xrDestroySession(session);
        if (instance) xrDestroyInstance(instance);
        if (display != EGL_NO_DISPLAY) {
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
            if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
            eglTerminate(display);
        }
    }
};
}

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* javaVm, void*) { vm = javaVm; return JNI_VERSION_1_6; }
extern "C" JNIEXPORT void JNICALL Java_com_gta2ceiling_roomprobe_RoomProbeActivity_nativeSetResumed(JNIEnv*, jobject, jboolean value) { resumed = value == JNI_TRUE; }
extern "C" JNIEXPORT void JNICALL Java_com_gta2ceiling_roomprobe_RoomProbeActivity_nativeStop(JNIEnv*, jobject) { stopRequested = true; }
extern "C" JNIEXPORT void JNICALL Java_com_gta2ceiling_roomprobe_RoomProbeActivity_nativeStart(JNIEnv* env, jobject activity, jstring path) {
    if (workerActive.exchange(true)) return;
    stopRequested = false;
    jobject reference = env->NewGlobalRef(activity);
    const char* chars = env->GetStringUTFChars(path, nullptr); const std::string reportPath = chars; env->ReleaseStringUTFChars(path, chars);
    std::thread([reference, reportPath] {
        JNIEnv* workerEnv = nullptr;
        if (vm->AttachCurrentThread(&workerEnv, nullptr) != JNI_OK) { workerActive = false; return; }
        Probe probe; probe.activity = reference; probe.reportPath = reportPath;
        try { probe.run(); } catch (const std::exception& error) { probe.status = std::string("ERROR: ") + error.what(); }
        probe.save(); probe.close();
        jclass type = workerEnv->GetObjectClass(reference);
        jmethodID callback = workerEnv->GetMethodID(type, "onNativeResult", "(Ljava/lang/String;)V");
        if (callback) { jstring result = workerEnv->NewStringUTF(probe.status.c_str()); workerEnv->CallVoidMethod(reference, callback, result); workerEnv->DeleteLocalRef(result); }
        workerEnv->DeleteLocalRef(type); workerEnv->DeleteGlobalRef(reference);
        if (workerEnv->ExceptionCheck()) workerEnv->ExceptionClear();
        vm->DetachCurrentThread(); workerActive = false;
    }).detach();
}
