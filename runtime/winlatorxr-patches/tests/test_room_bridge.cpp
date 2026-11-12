// SPDX-License-Identifier: GPL-3.0-or-later
// Android CPU CLI test of the actual transport implementation. OpenXR and the
// independently tested SceneQuery are mocked. No XR session or UI is opened.
#include "../overlay/gcc_room_bridge.cpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
int refreshes = 0, locates = 0;
XrSpaceLocationFlags locateFlags = 15;
XrPosef locatedPose{{0,0,0,1},{1,2,3}};
XrSpace locatedBase = XR_NULL_HANDLE;
XrTime locatedTime = 0;
template<class T> T FakeHandle(uintptr_t value) { return reinterpret_cast<T>(value); }
bool Has(const std::string& value, const char* text) { return value.find(text) != std::string::npos; }
XrSpace Resolve(int32_t index) { return index == 1 ? FakeHandle<XrSpace>(51) : XR_NULL_HANDLE; }
std::string Request(const char* operation, const char* base, const char* session, const char* epoch, const std::string& token) {
    return std::string("GCCROOM1 ") + operation + " nonce1 " + base + " " + session + " " + epoch + " " + token;
}
}

// These mocks replace Scene access only. Request parsing, tokens, reference
// epochs, size bounds and JNI/UDP control flow come from gcc_room_bridge.cpp.
namespace gcc_host {
struct SceneQuery::Impl { std::string status = "READY"; };
SceneQuery::SceneQuery() : impl_(new Impl) {}
SceneQuery::~SceneQuery() = default;
bool SceneQuery::Initialize(XrInstance, XrSession) { return true; }
void SceneQuery::RequestRefresh() { ++refreshes; impl_->status = "REFRESH_REQUESTED"; }
void SceneQuery::OnEvent(const XrEventDataBuffer&) {}
void SceneQuery::Tick(XrTime) {}
std::string SceneQuery::SnapshotJson(XrSpace base, XrTime time) {
    assert(base == FakeHandle<XrSpace>(51) && time == 100);
    return "{\"status\":\"QUERYING_ROOMS\",\"sceneQueryResult\":0,\"ceilings\":[],\"events\":[]}";
}
const std::string& SceneQuery::Status() const { return impl_->status; }
void SceneQuery::Shutdown() {}
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateReferenceSpaces(XrSession, uint32_t, uint32_t* count, XrReferenceSpaceType*) {
    *count = 0; return XR_SUCCESS;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrCreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo*, XrSpace* space) {
    *space = FakeHandle<XrSpace>(52); return XR_SUCCESS;
}
extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrDestroySpace(XrSpace) { return XR_SUCCESS; }
extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpace(XrSpace, XrSpace base, XrTime time, XrSpaceLocation* location) {
    ++locates; locatedBase = base; locatedTime = time;
    location->locationFlags = locateFlags; location->pose = locatedPose; return XR_SUCCESS;
}

int main() {
    bridge = std::make_unique<Bridge>(); auto& b = *bridge;
    b.token = std::string(32, 'a'); b.session_id = "session1"; b.epoch = 7;
    b.token_file_ready=true;
    b.sync = 12; b.recenter = 3; b.time = 100; b.resolve = Resolve;
    b.scene_available = true; b.permission = 1; b.raw_stage = FakeHandle<XrSpace>(52);
    auto response = b.Handle(nullptr, "GCCROOM1 CAPS nonce1");
    assert(Has(response, "\"status\":\"OK\"") && Has(response, "\"tokenReady\":true"));
    assert(!Has(response, b.token.c_str())); // Capability discovery never returns the secret.
    assert(Has(b.Handle(nullptr, "GCCROOM1 CAPS nonce1 extra"), "MALFORMED_REQUEST"));
    assert(Has(b.Handle(nullptr, "GCCROOM1 CAPS bad!nonce"), "MALFORMED_REQUEST"));
    auto get = [&](const char* base, const char* session, const char* epoch, const std::string& token) {
        return b.Handle(nullptr, Request("GET_ROOM",base,session,epoch,token));
    };
    assert(Has(get("1","session1","7",std::string(32,'b')), "UNAUTHORIZED"));
    assert(Has(get("1","oldsession","7",b.token), "STALE_SESSION"));
    assert(Has(get("1","session1","6",b.token), "STALE_REFERENCE_EPOCH"));
    assert(Has(get("1","session1","18446744073709551616",b.token), "MALFORMED_REQUEST"));
    assert(Has(get("-1","session1","7",b.token), "INVALID_BASE_SPACE"));
    assert(Has(get("2147483648","session1","7",b.token), "INVALID_BASE_SPACE"));
    assert(Has(get("2","session1","7",b.token), "UNKNOWN_BASE_SPACE"));
    b.permission = -1;
    assert(Has(get("1","session1","7",b.token), "SCENE_PERMISSION_DENIED"));
    b.permission = 0;
    assert(Has(get("1","session1","7",b.token), "SCENE_PERMISSION_REQUIRED"));
    b.permission = 1;
    response = get("1","session1","7",b.token);
    assert(Has(response, "\"coordinateSpace\":\"XRAPI_BASE\"") && Has(response, "\"baseSpaceIndex\":1"));
    assert(Has(response, "\"hmdSync\":12") && Has(response, "QUERYING_ROOMS"));
    // Pending asynchronous Scene data returns immediately; no guessed geometry.
    assert(Has(response, "\"ceilings\":[]") && response.size() <= MaxResponse);
    response = b.Handle(nullptr, Request("RAW_STAGE","1","session1","7",b.token));
    assert(Has(response, "\"rawStagePose\":") && Has(response, "\"position\":[1,2,3]"));
    assert(locatedBase == FakeHandle<XrSpace>(51) && locatedTime == 100);
    locateFlags = 0;
    assert(Has(b.Handle(nullptr, Request("RAW_STAGE","1","session1","7",b.token)), "RAW_STAGE_POSE_INVALID"));
    locateFlags = 15; locatedPose.position.x = std::numeric_limits<float>::quiet_NaN();
    response = b.Handle(nullptr, Request("RAW_STAGE","1","session1","7",b.token));
    assert(Has(response, "RAW_STAGE_POSE_INVALID") && !Has(response, "nan"));
    b.permission_requested = true;
    assert(Has(b.Handle(nullptr, "GCCROOM1 REFRESH nonce1 session1 7 " + b.token), "REFRESH_ACCEPTED"));
    assert(refreshes == 1 && b.want_scene && !b.permission_requested);
    b.ChangeEpoch(); assert(b.epoch == 8 && refreshes == 2);
    assert(Has(get("1","session1","7",b.token), "STALE_REFERENCE_EPOCH"));

    // Frame metadata and reference invalidation do not depend on a Java mock:
    // activity=null / socket=-1 deliberately disable platform I/O in this test.
    b.want_scene = false;
    GccRoomBridgeFrame(nullptr, 900, 24, 3);
    assert(b.sync == 24 && b.time == 900 && b.epoch == 8);
    assert(refreshes == 3); // Permission becomes unavailable: discard prior Scene data.
    XrEventDataReferenceSpaceChangePending change{XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING};
    change.session=b.session;change.changeTime = 1000;
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER}; std::memcpy(&event, &change, sizeof change);
    GccRoomBridgeEvent(&event); GccRoomBridgeFrame(nullptr, 999, 36, 3); assert(b.epoch == 8);
    GccRoomBridgeFrame(nullptr, 1000, 48, 3); assert(b.epoch == 9 && refreshes == 4);
    GccRoomBridgeFrame(nullptr, 1001, 60, 4); assert(b.epoch == 10 && refreshes == 5);
    GccRoomBridgeInvalidate(); GccRoomBridgeFrame(nullptr, 1002, 72, 4); assert(b.epoch == 11 && refreshes == 6);
    // Actual bridge integration: two future changes must not collapse to the
    // last timestamp retained by the stock renderer's recenter bookkeeping.
    gcc_host::ReferenceCounter published;published.Reset(4);
    b.recenter=0;const auto initialEpoch=b.epoch;
    change.changeTime=1010;std::memcpy(&event,&change,sizeof change);GccRoomBridgeEvent(&event);
    change.changeTime=1020;std::memcpy(&event,&change,sizeof change);GccRoomBridgeEvent(&event);
    assert(!GccRoomBridgeReferenceChangeDue(1009));
    assert(GccRoomBridgeReferenceChangeDue(1010));
    published.MarkCoordinatesChanged();
    GccRoomBridgeFrame(nullptr,1010,84,published.Publish(4));
    assert(b.recenter==published.Value() && b.recenter==1 && b.epoch==initialEpoch+2);
    assert(!GccRoomBridgeReferenceChangeDue(1019));
    assert(GccRoomBridgeReferenceChangeDue(1020));
    published.MarkCoordinatesChanged();
    GccRoomBridgeFrame(nullptr,1020,96,published.Publish(5));
    assert(b.recenter==published.Value() && b.recenter==2 && b.epoch==initialEpoch+4);
    assert(!GccRoomBridgeReferenceChangeDue(1021));
    change.session=FakeHandle<XrSession>(999);change.changeTime=1021;
    std::memcpy(&event,&change,sizeof change);GccRoomBridgeEvent(&event);
    assert(!GccRoomBridgeReferenceChangeDue(1021));
    {
        // Real native file adapter, isolated test-owned directory. No host app
        // directory or production token is opened, and no secret is printed.
        char path[]="/data/local/tmp/gcc-room-token-XXXXXX";
        const char* directory=::mkdtemp(path);assert(directory);
        NativeTokenFile file{std::string(directory)+"/access.token"};
        std::string cache(32,'c');const auto original=cache;
        assert(gcc_host::EnsurePrivateToken(cache,file));
        struct stat info{};assert(::stat(file.path.c_str(),&info)==0 && (info.st_mode&0777)==0600 && info.st_size==32);
        assert(::unlink(file.path.c_str())==0); // Simulate startup tmp cleanup.
        assert(gcc_host::EnsurePrivateToken(cache,file));
        assert(cache==original && file.Read().value==original);
        assert(::chmod(file.path.c_str(),0644)==0);
        assert(!gcc_host::EnsurePrivateToken(cache,file) && cache.empty());
        assert(::stat(file.path.c_str(),&info)==0 && (info.st_mode&0777)==0644); // Never rewritten.
        assert(::unlink(file.path.c_str())==0);
        assert(::symlink("missing-target",file.path.c_str())==0);
        assert(!gcc_host::EnsurePrivateToken(cache,file));
        assert(::lstat(file.path.c_str(),&info)==0 && S_ISLNK(info.st_mode));
        assert(::unlink(file.path.c_str())==0 && ::rmdir(directory)==0);
    }
    bridge.reset();
    std::puts("PASS: actual RoomBridge request/auth/epoch CPU tests; mocked Scene/OpenXR, no headset session or UI.");
    return 0;
}
