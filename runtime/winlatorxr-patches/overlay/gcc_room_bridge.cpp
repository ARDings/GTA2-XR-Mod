// SPDX-License-Identifier: GPL-3.0-or-later
// GTA2Ceiling optional host channel for WinlatorXR 39b270c6401a69e7c28ea6ffd61de3dbac9e9b1e.
// All OpenXR calls run on the host XR thread. No independent session or head-pose alignment.
#include "gcc_room_bridge.h"
#include "gcc_scene_query.h"
#include "gcc_reference_counter.h"
#include "gcc_private_token.h"
#include <android/log.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <iomanip>
#include <locale>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {
constexpr const char* Commit = "39b270c6401a69e7c28ea6ffd61de3dbac9e9b1e";
constexpr size_t MaxResponse = 60000;
constexpr uint16_t Port = 7279;
bool scene_extensions = false;
std::atomic<bool> invalidate{false};
std::string Quote(const std::string& value) {
    std::string result="\"";
    for (unsigned char c:value) {
        if(c=='"'||c=='\\') { result+='\\'; result+=static_cast<char>(c); }
        else if(c<32) { char escaped[7]; std::snprintf(escaped,sizeof escaped,"\\u%04x",c); result+=escaped; }
        else result+=static_cast<char>(c);
    }
    return result+'"';
}
bool Identifier(const std::string& s) {
    return !s.empty() && s.size()<=32 && std::all_of(s.begin(),s.end(),[](unsigned char c) {
        return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-';
    });
}
bool Unsigned(const std::string& s,uint64_t& number) {
    if(s.empty()||s.size()>20) return false;
    number=0;
    for(unsigned char c:s) {
        if(c<'0'||c>'9'||number>(UINT64_MAX-(c-'0'))/10) return false;
        number=number*10+(c-'0');
    }
    return true;
}
bool ValidPose(const XrSpaceLocation& location) {
    constexpr auto valid=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if((location.locationFlags&valid)!=valid) return false;
    const auto& p=location.pose.position; const auto& q=location.pose.orientation;
    const float norm=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
    return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&
        std::isfinite(norm)&&std::abs(norm-1.0f)<0.01f;
}
std::string PoseJson(const XrSpaceLocation& location) {
    const auto& p=location.pose.position; const auto& q=location.pose.orientation;
    std::ostringstream out; out.imbue(std::locale::classic()); out<<std::setprecision(9);
    constexpr auto tracked=XR_SPACE_LOCATION_POSITION_TRACKED_BIT|XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    out<<"{\"locationFlags\":"<<location.locationFlags<<",\"poseValid\":true,\"poseTracked\":"
       <<(((location.locationFlags&tracked)==tracked)?"true":"false")<<",\"position\":["
       <<p.x<<','<<p.y<<','<<p.z<<"],\"orientation\":["<<q.x<<','<<q.y<<','<<q.z<<','<<q.w<<"]}";
    return out.str();
}
struct NativeTokenFile {
    std::string path;
    gcc_host::TokenFileRead Read() const {
        const int fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
        if(fd<0) return {errno==ENOENT?gcc_host::TokenFileState::Missing:gcc_host::TokenFileState::Unsafe,{}};
        struct stat info{};char bytes[33]{};
        const bool safe=fstat(fd,&info)==0 && S_ISREG(info.st_mode) && info.st_uid==geteuid() &&
            (info.st_mode&0077)==0 && info.st_size==32;
        const auto count=safe?::read(fd,bytes,sizeof bytes):-1;
        ::close(fd);
        if(!safe || count!=32) return {gcc_host::TokenFileState::Unsafe,{}};
        const std::string value(bytes,32);
        return {gcc_host::ValidPrivateToken(value)?gcc_host::TokenFileState::Valid:gcc_host::TokenFileState::Unsafe,value};
    }
    std::string Generate() const {
        unsigned char random[16];
        const int fd=::open("/dev/urandom",O_RDONLY|O_CLOEXEC);
        if(fd<0) return {};
        const auto count=::read(fd,random,sizeof random);::close(fd);
        if(count!=sizeof random) return {};
        char encoded[33]{};
        for(size_t i=0;i<sizeof random;++i) std::snprintf(encoded+i*2,3,"%02x",random[i]);
        return std::string(encoded,32);
    }
    bool CreateExclusive(const std::string& value) const {
        const int fd=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd<0) return false;
        const bool saved=::write(fd,value.data(),value.size())==static_cast<ssize_t>(value.size()) && ::fsync(fd)==0;
        ::close(fd);
        if(!saved) ::unlink(path.c_str()); // Only the file just created by this call.
        return saved;
    }
};
struct Bridge {
    XrInstance instance=XR_NULL_HANDLE;
    XrSession session=XR_NULL_HANDLE;
    XrSpace raw_stage=XR_NULL_HANDLE;
    GccResolveSpace resolve=nullptr;
    jobject activity=nullptr;
    jmethodID permission_method=nullptr,request_method=nullptr,token_method=nullptr;
    int socket=-1,permission=0;
    bool permission_requested=false,want_scene=false,scene_available=false;
    bool token_file_ready=false;
    std::chrono::steady_clock::time_point token_check_at{};
    std::string token,session_id,occlusion_path,logged_scene_status;
    int logged_permission=-3;
    bool occlusion=true,field_valid=false;
    float field[4]{};  // y, centre x, centre z, radius in the XrAPI base space
    std::chrono::steady_clock::time_point occlusion_check_at{};
    uint64_t epoch=1;
    XrTime time=0;
    uint32_t sync=0,recenter=0;
    bool have_recenter=false;
    std::vector<XrTime> pending_changes;
    gcc_host::SceneQuery scene;
    ~Bridge() { if(socket>=0) ::close(socket); }

    bool JavaException(JNIEnv* env) {
        if(!env->ExceptionCheck()) return false;
        env->ExceptionClear(); return true;
    }
    int ReadPermission(JNIEnv* env) {
        if(!activity||!permission_method) return -2;
        const int result=env->CallIntMethod(activity,permission_method);
        return JavaException(env)?-2:result;
    }
    void EnsureToken(JNIEnv* env) {
        if(!activity||!token_method) return;
        const auto now=std::chrono::steady_clock::now();
        if(now<token_check_at) return;
        token_check_at=now+std::chrono::milliseconds(500);
        const bool was_ready=token_file_ready;
        token_file_ready=false;
        auto value=static_cast<jstring>(env->CallObjectMethod(activity,token_method));
        if(JavaException(env)||!value) return;
        const char* text=env->GetStringUTFChars(value,nullptr);
        if(!text) { JavaException(env); env->DeleteLocalRef(value); return; }
        std::string path(text);
        env->ReleaseStringUTFChars(value,text); env->DeleteLocalRef(value);
        if(path.empty()) return;
        // The game's in-headset menu writes its real-world occlusion choice
        // beside the private token (Wine: Z:\\tmp\\xr\\gta2ceiling_occlusion).
        if(const auto slash=path.rfind('/');slash!=std::string::npos)
            occlusion_path=path.substr(0,slash)+"/gta2ceiling_occlusion";
        NativeTokenFile file{path};
        token_file_ready=gcc_host::EnsurePrivateToken(token,file);
        if(token_file_ready!=was_ready) __android_log_print(ANDROID_LOG_INFO,"GTA2RoomBridge",
            "Private room access file %s",token_file_ready?"ready":"unavailable");
    }
    bool Authorized(const std::string& supplied) const {
        if(!token_file_ready||token.size()!=32||supplied.size()!=32) return false;
        unsigned char difference=0;
        for(size_t i=0;i<32;++i) difference|=static_cast<unsigned char>(token[i]^supplied[i]);
        return difference==0;
    }
    std::string Header(const std::string& request,const std::string& nonce,const char* status) const {
        const char* permission_text=permission==1?"GRANTED":permission==-1?"DENIED":permission==-2?"BRIDGE_JAVA_UNAVAILABLE":"UNKNOWN";
        return "{\"protocol\":\"GCCROOM1\",\"request\":"+Quote(request)+",\"nonce\":"+Quote(nonce)+
            ",\"status\":"+Quote(status)+",\"upstreamCommit\":\""+Commit+"\",\"sessionId\":"+Quote(session_id)+
            ",\"referenceEpoch\":"+std::to_string(epoch)+",\"hmdSync\":"+std::to_string(sync)+
            ",\"recenterCount\":"+std::to_string(recenter)+",\"xrTime\":"+std::to_string(time)+
            ",\"capabilities\":{\"rawStage\":"+(raw_stage?"true":"false")+
            ",\"sceneQuery\":"+(scene_available?"true":"false")+"},\"scenePermission\":"+Quote(permission_text);
    }
    void ChangeEpoch() {
        ++epoch;
        // Invalidate anchor cache rather than apply a guessed coordinate offset.
        if(scene_available) scene.RequestRefresh();
    }
    std::string Handle(JNIEnv* env,const std::string& packet) {
        std::istringstream in(packet); std::vector<std::string> parts;
        for(std::string part;in>>part;) { parts.push_back(part); if(parts.size()>7) break; }
        const std::string request=parts.size()>1&&Identifier(parts[1])?parts[1]:"INVALID";
        const std::string nonce=parts.size()>2&&Identifier(parts[2])?parts[2]:"INVALID";
        const auto error=[&](const char* status) { return Header(request,nonce,status)+"}"; };
        if(parts.size()<3||parts[0]!="GCCROOM1"||nonce=="INVALID") return error("MALFORMED_REQUEST");
        if(request=="CAPS") {
            if(parts.size()!=3) return error("MALFORMED_REQUEST");
            return Header(request,nonce,"OK")+",\"auth\":\"private-token-v1\",\"tokenReady\":"+
                (token_file_ready?"true":"false")+",\"maxResponseBytes\":60000}";
        }
        const bool refresh=request=="REFRESH";
        if((!refresh&&request!="GET_ROOM"&&request!="RAW_STAGE")||parts.size()!=(refresh?6u:7u))
            return error("MALFORMED_REQUEST");
        if(!Authorized(parts.back())) return error("UNAUTHORIZED");
        const size_t session_index=refresh?3:4,epoch_index=refresh?4:5;
        uint64_t requested_epoch=0;
        if(!Unsigned(parts[epoch_index],requested_epoch)) return error("MALFORMED_REQUEST");
        if(parts[session_index]!=session_id) return error("STALE_SESSION");
        if(requested_epoch!=epoch) return error("STALE_REFERENCE_EPOCH");
        if(refresh) {
            if(!scene_available) return error("SCENE_UNSUPPORTED");
            want_scene=true; permission_requested=false; scene.RequestRefresh();
            return error("REFRESH_ACCEPTED");
        }
        uint64_t base_index=0;
        if(!Unsigned(parts[3],base_index)||base_index>INT32_MAX) return error("INVALID_BASE_SPACE");
        const XrSpace base=resolve?resolve(static_cast<int32_t>(base_index)):XR_NULL_HANDLE;
        if(!base) return error("UNKNOWN_BASE_SPACE");
        const std::string metadata=",\"baseSpaceIndex\":"+std::to_string(base_index)+
            ",\"coordinateSpace\":\"XRAPI_BASE\",\"units\":\"meters\"";
        if(request=="RAW_STAGE") {
            if(!raw_stage) return error("NATURAL_STAGE_UNSUPPORTED");
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            const auto result=xrLocateSpace(raw_stage,base,time,&location);
            if(XR_FAILED(result)||!ValidPose(location)) return error("RAW_STAGE_POSE_INVALID");
            return Header(request,nonce,"OK")+metadata+",\"rawStagePose\":"+PoseJson(location)+"}";
        }
        if(!scene_available) return error("SCENE_UNSUPPORTED");
        want_scene=true;
        if(permission!=1) {
            if(permission==0&&!permission_requested&&request_method) {
                permission_requested=true;
                env->CallVoidMethod(activity,request_method);
                if(JavaException(env)) return error("PERMISSION_REQUEST_FAILED");
            }
            return error(permission==-1?"SCENE_PERMISSION_DENIED":"SCENE_PERMISSION_REQUIRED");
        }
        if(scene.Status()=="IDLE") scene.RequestRefresh();
        // Query completion/pose validity remain explicit inside scene.status.
        return Header(request,nonce,"OK")+metadata+",\"scene\":"+scene.SnapshotJson(base,time)+"}";
    }
};
std::unique_ptr<Bridge> bridge;
}

extern "C" void GccRoomBridgeSetSceneExtensionsEnabled(int enabled) { scene_extensions=enabled!=0; }
extern "C" void GccRoomBridgeInit(XrInstance instance,XrSession session,JNIEnv* env,jobject activity,GccResolveSpace resolver) {
    GccRoomBridgeShutdown(env);
    auto next=std::make_unique<Bridge>(); next->instance=instance; next->session=session; next->resolve=resolver;
    next->activity=env->NewGlobalRef(activity);
    jclass type=env->GetObjectClass(activity);
    next->permission_method=env->GetMethodID(type,"gccScenePermissionState","()I"); next->JavaException(env);
    next->request_method=env->GetMethodID(type,"gccRequestScenePermission","()V"); next->JavaException(env);
    next->token_method=env->GetMethodID(type,"gccRoomTokenPath","()Ljava/lang/String;"); next->JavaException(env);
    env->DeleteLocalRef(type);
    std::ostringstream session_id;
    session_id<<std::hex<<getpid()<<std::chrono::steady_clock::now().time_since_epoch().count();
    next->session_id=session_id.str();
    uint32_t count=0;
    if(XR_SUCCEEDED(xrEnumerateReferenceSpaces(session,0,&count,nullptr))&&count<=64) {
        std::vector<XrReferenceSpaceType> types(count);
        if(XR_SUCCEEDED(xrEnumerateReferenceSpaces(session,count,&count,types.data()))&&
            std::find(types.begin(),types.end(),XR_REFERENCE_SPACE_TYPE_STAGE)!=types.end()) {
            XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            info.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_STAGE; info.poseInReferenceSpace.orientation.w=1;
            if(XR_FAILED(xrCreateReferenceSpace(session,&info,&next->raw_stage))) next->raw_stage=XR_NULL_HANDLE;
        }
    }
    next->scene_available=scene_extensions&&next->scene.Initialize(instance,session);
    next->socket=::socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    sockaddr_in address{}; address.sin_family=AF_INET; address.sin_port=htons(Port); address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(next->socket<0||::bind(next->socket,reinterpret_cast<sockaddr*>(&address),sizeof address)!=0) {
        __android_log_print(ANDROID_LOG_ERROR,"GTA2RoomBridge","Could not bind loopback room channel; feature disabled");
        if(next->socket>=0) ::close(next->socket); next->socket=-1;
    }
    next->EnsureToken(env);
    bridge=std::move(next);
}
extern "C" void GccRoomBridgeFrame(JNIEnv* env,XrTime time,uint32_t sync,uint32_t recenter_count) {
    if(!bridge) return;
    auto& b=*bridge; b.time=time; b.sync=sync;
    if(invalidate.exchange(false)) b.ChangeEpoch();
    if(b.have_recenter&&b.recenter!=recenter_count) b.ChangeEpoch();
    b.recenter=recenter_count; b.have_recenter=true;
    const unsigned due=gcc_host::ConsumeReferenceChanges(b.pending_changes,time);
    for(unsigned i=0;i<due;++i) b.ChangeEpoch();
    const auto previous_permission=b.permission;
    b.permission=b.ReadPermission(env);
    if(previous_permission==1&&b.permission!=1&&b.scene_available) b.scene.RequestRefresh();
    if(b.permission==1&&b.want_scene&&b.scene_available) b.scene.Tick(time);
    if(b.permission!=b.logged_permission) {
        __android_log_print(ANDROID_LOG_INFO,"GTA2RoomBridge","scene permission state %d",b.permission);
        b.logged_permission=b.permission;
    }
    if(b.scene_available&&b.scene.Status()!=b.logged_scene_status) {
        b.logged_scene_status=b.scene.Status();
        __android_log_print(ANDROID_LOG_INFO,"GTA2RoomBridge","scene query: %s",b.logged_scene_status.c_str());
    }
    b.EnsureToken(env);
    if(b.socket<0) return;
    // Bounded per-frame work: a malformed or flooding client cannot starve XR.
    for(unsigned i=0;i<8;++i) {
        char data[513]; sockaddr_in source{}; socklen_t size=sizeof source;
        const auto bytes=::recvfrom(b.socket,data,sizeof data,MSG_DONTWAIT|MSG_TRUNC,reinterpret_cast<sockaddr*>(&source),&size);
        if(bytes<0) break;
        if(bytes>512||bytes==0||source.sin_addr.s_addr!=htonl(INADDR_LOOPBACK)||std::memchr(data,0,bytes)) continue;
        std::string response;
        std::string magic,request,nonce;
        std::istringstream identity(std::string(data,bytes)); identity>>magic>>request>>nonce;
        if(!Identifier(request)) request="INVALID";
        if(!Identifier(nonce)) nonce="INVALID";
        try { response=b.Handle(env,std::string(data,bytes)); }
        catch(const std::exception&) { response=b.Header(request,nonce,"INTERNAL_ERROR")+"}"; }
        if(response.size()>MaxResponse) response=b.Header(request,nonce,"RESPONSE_TOO_LARGE")+"}";
        ::sendto(b.socket,response.data(),response.size(),MSG_DONTWAIT,reinterpret_cast<sockaddr*>(&source),size);
    }
}
extern "C" int GccRoomBridgeReferenceChangeDue(XrTime time) {
    return bridge && (invalidate.load() || gcc_host::ReferenceChangeDue(bridge->pending_changes,time));
}
extern "C" void GccRoomBridgeEvent(const XrEventDataBuffer* event) {
    if(!bridge||!event) return;
    if(event->type==XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
        const auto* change=reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(event);
        if(change->session!=bridge->session) return;
        if(bridge->pending_changes.size()<32) bridge->pending_changes.push_back(change->changeTime);
        else invalidate=true;
    }
    if(bridge->scene_available) bridge->scene.OnEvent(*event);
}
extern "C" void GccRoomBridgeInvalidate(void) { invalidate=true; }
extern "C" void GccRoomBridgeShutdown(JNIEnv* env) {
    if(!bridge) return;
    bridge->scene.Shutdown();
    if(bridge->raw_stage) xrDestroySpace(bridge->raw_stage);
    if(bridge->activity) env->DeleteGlobalRef(bridge->activity);
    bridge.reset(); invalidate=false;
}
extern "C" int GccRoomBridgePlaneHeights(XrSpace space,XrTime time,float head_y,float* ceiling_y,float* floor_y,
                                         float* outline_xz,int max_points,int* point_count) {
    if(point_count) *point_count=0;
    if(!bridge||!bridge->scene_available||bridge->permission!=1) return 0;
    return bridge->scene.PlaneHeights(space,time,head_y,ceiling_y,floor_y,outline_xz,max_points,point_count);
}
extern "C" int GccRoomBridgeOcclusionWanted(void) {
    if(!bridge) return 0;
    auto& b=*bridge;
    const auto now=std::chrono::steady_clock::now();
    if(now>=b.occlusion_check_at && !b.occlusion_path.empty()) {
        b.occlusion_check_at=now+std::chrono::milliseconds(500);
        char text[96]={'1',0};
        const int fd=::open(b.occlusion_path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        if(fd>=0) {
            const auto bytes=::read(fd,text,sizeof(text)-1);
            text[bytes>0?bytes:0]=0;
            if(bytes<=0) { text[0]='1'; text[1]=0; }
            ::close(fd);
        }
        b.occlusion=text[0]!='0';
        float y=0,x=0,z=0,r=0;
        const bool field=std::sscanf(text,"%*d %f %f %f %f",&y,&x,&z,&r)==4 &&
            std::isfinite(y)&&std::isfinite(x)&&std::isfinite(z)&&std::isfinite(r)&&r>0.05f&&r<5.0f;
        if(field!=b.field_valid || (field && (std::fabs(y-b.field[0])>0.005f || std::fabs(r-b.field[3])>0.005f)))
            __android_log_print(ANDROID_LOG_INFO,"GTA2RoomBridge","game field: %s y=%.2f centre=(%.2f,%.2f) r=%.2f",
                                field?"round":"room",y,x,z,r);
        b.field_valid=field;
        if(field) { b.field[0]=y; b.field[1]=x; b.field[2]=z; b.field[3]=r; }
    }
    return b.occlusion?1:0;
}
extern "C" int GccRoomBridgeField(XrSpace space,XrTime time,float* out) {
    if(!bridge||!bridge->field_valid||!bridge->resolve||!space||time<=0||!out) return 0;
    const XrSpace base=bridge->resolve(1);
    if(!base) return 0;
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    if(XR_FAILED(xrLocateSpace(base,space,time,&location))||!ValidPose(location)) return 0;
    const auto& q=location.pose.orientation; const auto& t=location.pose.position;
    const float v[3]={bridge->field[1],bridge->field[0],bridge->field[2]};
    // Rotate v by q, then translate: the base space origin in the layer space.
    const float cx=2*(q.y*v[2]-q.z*v[1]),cy=2*(q.z*v[0]-q.x*v[2]),cz=2*(q.x*v[1]-q.y*v[0]);
    const float px=v[0]+q.w*cx+(q.y*cz-q.z*cy)+t.x;
    const float py=v[1]+q.w*cy+(q.z*cx-q.x*cz)+t.y;
    const float pz=v[2]+q.w*cz+(q.x*cy-q.y*cx)+t.z;
    out[0]=px; out[1]=py; out[2]=pz; out[3]=bridge->field[3];
    return 1;
}
