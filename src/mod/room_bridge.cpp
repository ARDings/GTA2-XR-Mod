#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include "room_bridge.h"
#include "../../third_party/nlohmann-json/json.hpp"
#include <chrono>
#include <locale>
#include <sstream>
#include <fstream>
#include <limits>

namespace gta2ceiling {
namespace {
using Json=nlohmann::json;
constexpr const char* Commit="39b270c6401a69e7c28ea6ffd61de3dbac9e9b1e";
bool Identifier(const std::string& value,size_t max=128) {
    return !value.empty() && value.size()<=max && value.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_-")==std::string::npos;
}
bool Contains(const RoomSurface& surface,float x,float z) {
    bool inside=false;
    for(size_t i=0,j=surface.boundary.size()-1;i<surface.boundary.size();j=i++) {
        const auto& a=surface.boundary[i];const auto& b=surface.boundary[j];
        if(room_detail::OnSegment(a,b,{x,0,z})) return true;
        if((a.z>z)!=(b.z>z) && x<(b.x-a.x)*(z-a.z)/(b.z-a.z)+a.x) inside=!inside;
    }
    return inside;
}
uint64_t Millis() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
std::string RoomToken() {
    // Host-owned Wine imagefs path, never shared Android Downloads or logs.
    std::ifstream file("Z:\\tmp\\xr\\gta2ceiling_room.token",std::ios::binary);
    std::string token;std::getline(file,token);
    if(!token.empty() && token.back()=='\r') token.pop_back();
    if(token.size()!=32 || token.find_first_not_of("0123456789abcdefABCDEF")!=std::string::npos) return {};
    return token;
}
bool HasSemantic(const std::string& labels,const char* wanted) {
    std::istringstream input(labels);std::string token;
    while(std::getline(input,token,',')) {
        const auto first=token.find_first_not_of(" \t\r\n");
        const auto last=token.find_last_not_of(" \t\r\n");
        if(first!=std::string::npos && token.substr(first,last-first+1)==wanted) return true;
    }
    return false;
}
bool CeilingSemantic(const std::string& labels) { return HasSemantic(labels,"CEILING"); }
bool FloorSemantic(const std::string& labels) { return HasSemantic(labels,"FLOOR"); }
}
bool RoomReplyMatchesTracking(const RoomReply& reply,const XrTrackingFrame& frame,
                              uint64_t requestAgeMs) {
    // Pinned host: sync=0,12,...252, then0 (22slots). At Quest's maximum120Hz,
    // a full ring lasts183ms. A <=100ms round trip plus <=4slot distance cannot
    // mistake an entire old ring for a current capture. Allow the response to
    // lead the separate tracking socket by a few frames due to receive order.
    if(!frame.pose_valid || reply.xrTime<=0 || requestAgeMs>100 ||
       reply.recenter!=frame.recenter_count || reply.hmdSync>252 || frame.hmd_sync>252 ||
       reply.hmdSync%12 || frame.hmd_sync%12) return false;
    const unsigned a=reply.hmdSync/12,b=frame.hmd_sync/12;
    const unsigned difference=a>b?a-b:b-a;
    return std::min(difference,22u-difference)<=4;
}
bool ParseRoomReply(std::string_view text,RoomReply& output,std::string& error) {
    error.clear();
    try {
        if(text.empty() || text.size()>60000 || text.find('\0')!=std::string_view::npos)
            throw std::runtime_error("Invalid room packet length.");
        const auto j=Json::parse(text);
        if(j.at("protocol")!="GCCROOM1" || j.at("upstreamCommit")!=Commit)
            throw std::runtime_error("Unrecognised room bridge protocol or runtime source.");
        RoomReply reply;
        reply.request=j.at("request").get<std::string>();reply.nonce=j.at("nonce").get<std::string>();
        reply.status=j.at("status").get<std::string>();reply.session=j.at("sessionId").get<std::string>();
        if(!Identifier(reply.nonce,32) || !Identifier(reply.session) || !Identifier(reply.status))
            throw std::runtime_error("Malformed room response identity.");
        if(!j.at("referenceEpoch").is_number_unsigned() || !j.at("recenterCount").is_number_unsigned())
            throw std::runtime_error("Missing nonnegative room reference epoch.");
        reply.epoch=j.at("referenceEpoch").get<uint64_t>();
        const auto recenter=j.at("recenterCount").get<uint64_t>();
        if(recenter>UINT32_MAX) throw std::runtime_error("Invalid recenter counter.");
        reply.recenter=static_cast<uint32_t>(recenter);
        if(!j.at("hmdSync").is_number_unsigned() || !j.at("xrTime").is_number_unsigned())
            throw std::runtime_error("Missing host capture sync/time.");
        const auto sync=j.at("hmdSync").get<uint64_t>(),time=j.at("xrTime").get<uint64_t>();
        if(sync>252 || sync%12 || !time || time>static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            throw std::runtime_error("Invalid host capture sync/time.");
        reply.hmdSync=static_cast<uint8_t>(sync);reply.xrTime=static_cast<int64_t>(time);
        if(reply.request=="CAPS") {
            reply.sceneCapable=reply.status=="OK" && j.at("capabilities").at("sceneQuery").get<bool>();
        } else if(reply.request=="GET_ROOM" && reply.status=="OK") {
            if(j.at("coordinateSpace")!="XRAPI_BASE" || j.at("units")!="meters" || j.at("baseSpaceIndex")!=1)
                throw std::runtime_error("Room and eyes do not use the same reference space.");
            const auto& scene=j.at("scene");
            reply.sceneStatus=scene.at("status").get<std::string>();
            if(reply.sceneStatus.size()>256) throw std::runtime_error("Oversized Scene status.");
            if(reply.sceneStatus=="SUCCESS_CEILING_DATA") {
                const auto& ceilings=scene.at("ceilings");
                if(!ceilings.is_array() || ceilings.size()>32) throw std::runtime_error("Invalid ceiling count.");
                for(const auto& ceiling:ceilings) {
                    if(!ceiling.at("xrTime").is_number_unsigned() ||
                       ceiling.at("xrTime").get<uint64_t>()!=time)
                        throw std::runtime_error("Ceiling pose and room reply have different capture times.");
                    if(!ceiling.at("poseValid").get<bool>()) continue;
                    if(!ceiling.at("locationFlags").is_number_unsigned())
                        throw std::runtime_error("Invalid ceiling location flags.");
                    const auto flags=ceiling.at("locationFlags").get<uint64_t>();
                    if((flags&3)!=3) continue; // XR_SPACE_LOCATION_ORIENTATION/POSITION_VALID_BIT.
                    const auto labels=ceiling.at("semanticLabels").get<std::string>();
                    const bool isFloor=!CeilingSemantic(labels) && FloorSemantic(labels);
                    if(!isFloor && !CeilingSemantic(labels))
                        throw std::runtime_error("Room entity is not a labelled ceiling or floor.");
                    const auto uuid=ceiling.at("ceilingUUID").get<std::string>();
                    if(!Identifier(uuid)) throw std::runtime_error("Invalid ceiling UUID.");
                    const auto& polygon=ceiling.at("polygon3DBase");
                    if(!polygon.is_array() || polygon.size()>257) throw std::runtime_error("Oversized ceiling boundary.");
                    std::vector<Vec3> vertices;
                    for(const auto& p:polygon) {
                        if(!p.is_array()||p.size()!=3) throw std::runtime_error("Invalid ceiling vertex.");
                        vertices.push_back({p[0].get<float>(),p[1].get<float>(),p[2].get<float>()});
                    }
                    RoomSurface surface;std::string why;
                    if(!BuildRoomSurface(std::move(vertices),surface,why)) continue;
                    if(isFloor) {reply.floorSurfaces.push_back(std::move(surface));reply.floorUuids.push_back(uuid);}
                    else {reply.surfaces.push_back(std::move(surface));reply.ceilingUuids.push_back(uuid);}
                }
            }
        } else if(reply.request!="GET_ROOM") throw std::runtime_error("Unexpected room reply kind.");
        output=std::move(reply);return true;
    } catch(const std::exception& e) {error=e.what();return false;}
}
struct RoomBridge::State {
    bool initialized=false,capable=false,haveSurface=false,floor=false;
    // GTA2Ceiling: once a surface was found it stays (no expiry, no status
    // churn); the host is asked again only after a recenter or a ceiling/floor
    // switch. Re-polling every 100 ms with a 600 ms expiry made the city drop
    // out and re-anchor whenever the host answered late (e.g. after standby).
    bool locked=false;
    uint64_t due=0,pendingAt=0,surfaceAt=0,nonceCounter=0,epoch=0;
    int64_t latestSceneTime=0;
    uint32_t recenter=0;
    std::string nonce,request,session,uuid,status="Looking for the room host";
    RoomSurface surface;
#ifdef _WIN32
    SOCKET socket=INVALID_SOCKET;sockaddr_in host{};
#endif
};
RoomBridge::RoomBridge():state_(std::make_unique<State>()){}
RoomBridge::~RoomBridge(){Stop();}
void RoomBridge::Stop(){
#ifdef _WIN32
    if(state_->socket!=INVALID_SOCKET) closesocket(state_->socket);
    if(state_->initialized) WSACleanup();
#endif
    *state_=State{};
}
void RoomBridge::SetFloorMode(bool floor) {
    if(state_->floor==floor) return;
    state_->floor=floor;state_->haveSurface=false;state_->locked=false;state_->due=0;
}
const RoomSurface* RoomBridge::Surface() const {
    return state_->haveSurface?&state_->surface:nullptr;
}
const std::string& RoomBridge::CeilingUuid() const{return state_->uuid;}
const std::string& RoomBridge::Status() const{return state_->status;}
void RoomBridge::Poll(const XrTrackingFrame& frame) {
#ifdef _WIN32
    auto& s=*state_;const auto now=Millis();
    if(s.recenter!=frame.recenter_count) {
        s.recenter=frame.recenter_count;if(!s.locked) s.haveSurface=false;s.capable=false;s.nonce.clear();s.due=0;s.latestSceneTime=0;
    }
    if(!s.initialized) {
        if(now<s.due) return;
        WSADATA data{};s.due=now+3000;
        if(WSAStartup(MAKEWORD(2,2),&data)!=0) return;
        s.initialized=true;s.socket=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        if(s.socket==INVALID_SOCKET) {Stop();return;}
        sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        u_long nonblocking=1;
        if(bind(s.socket,reinterpret_cast<sockaddr*>(&local),sizeof(local))!=0 ||
           ioctlsocket(s.socket,FIONBIO,&nonblocking)!=0) {Stop();return;}
        s.host.sin_family=AF_INET;s.host.sin_port=htons(7279);s.host.sin_addr.s_addr=htonl(INADDR_LOOPBACK);s.due=0;
    }
    for(int count=0;count<8;++count) {
        char buffer[60001];sockaddr_in peer{};int peerSize=sizeof(peer);
        const int bytes=recvfrom(s.socket,buffer,sizeof(buffer),0,reinterpret_cast<sockaddr*>(&peer),&peerSize);
        if(bytes<=0) break;
        if(peer.sin_addr.s_addr!=s.host.sin_addr.s_addr || peer.sin_port!=s.host.sin_port || s.nonce.empty()) continue;
        RoomReply reply;std::string error;
        if(!ParseRoomReply(std::string_view(buffer,bytes),reply,error)) {
            // An invalid response from the bound host must not leave a cached
            // ceiling visible until its timeout. Retry discovery fail-closed.
            if(!s.locked) s.haveSurface=false;s.capable=false;s.nonce.clear();s.due=now+100;
            s.status="Raumdaten werden erneut geprueft";continue;
        }
        if(reply.nonce!=s.nonce || reply.request!=s.request || now-s.pendingAt>500) continue;
        s.nonce.clear();
        if(reply.status!="OK" || reply.recenter!=frame.recenter_count) {
            const bool currentSession=reply.session==s.session && reply.epoch==s.epoch &&
                reply.recenter==frame.recenter_count;
            const bool permission=currentSession && (reply.status=="SCENE_PERMISSION_REQUIRED" ||
                reply.status=="SCENE_PERMISSION_DENIED" || reply.status=="PERMISSION_REQUEST_FAILED");
            if(!s.locked) s.haveSurface=false;if(!permission) s.capable=false;
            if(reply.status=="SCENE_PERMISSION_REQUIRED")
                s.status="Raumfreigabe im Headset bestaetigen";
            else if(reply.status=="SCENE_PERMISSION_DENIED")
                s.status="Raumfreigabe fuer diese App in den Quest-Einstellungen erlauben";
            else if(reply.status=="PERMISSION_REQUEST_FAILED")
                s.status="Could not open the room access token. Restart the GTA2Ceiling host";
            else s.status="Reloading the room reference";
            s.due=now+(permission?1000:100);continue;
        }
        if(reply.request=="CAPS") {
            if(reply.session!=s.session || reply.epoch!=s.epoch) {
                if(!s.locked) s.haveSurface=false;s.latestSceneTime=0;
            }
            s.session=reply.session;s.epoch=reply.epoch;s.capable=reply.sceneCapable;
            s.status=s.capable?"Loading the saved room":"This runtime does not support the room scan";
            s.due=now+(s.capable?0:3000);
        } else {
            if(reply.session!=s.session || reply.epoch!=s.epoch) {
                if(!s.locked) s.haveSurface=false;s.capable=false;s.latestSceneTime=0;s.due=now;continue;
            }
            if(!RoomReplyMatchesTracking(reply,frame,now-s.pendingAt) || reply.xrTime<s.latestSceneTime) {
                if(!s.locked) s.haveSurface=false;s.status="Aktuelle Raumdaten werden erwartet";s.due=now;continue;
            }
            s.latestSceneTime=reply.xrTime;
            const bool hadSurface=s.haveSurface;
            s.haveSurface=false;float nearest=10000;
            const auto& candidates=s.floor?reply.floorSurfaces:reply.surfaces;
            const auto& uuids=s.floor?reply.floorUuids:reply.ceilingUuids;
            for(size_t i=0;i<candidates.size();++i) {
                const auto& surface=candidates[i];
                // Ceiling above the head, or floor below it.
                const float distance=s.floor?frame.pose.position_y-surface.centre.y:surface.centre.y-frame.pose.position_y;
                if(distance>0.25f && distance<nearest && Contains(surface,frame.pose.position_x,frame.pose.position_z)) {
                    s.surface=surface;s.uuid=uuids[i];nearest=distance;s.haveSurface=true;
                    s.surfaceAt=s.pendingAt; // Include transport delay in cache age.
                }
            }
            if(!s.haveSurface && s.locked && hadSurface) s.haveSurface=true;  // keep the known one
            if(s.haveSurface) s.locked=true;
            if(s.haveSurface) s.status=s.floor?"Floor from the room scan":"Ceiling from the room scan";
            else if(s.floor && reply.sceneStatus=="SUCCESS_CEILING_DATA" && reply.floorSurfaces.empty())
                s.status="No floor in the room scan. Update the host app or scan the room again";
            else if(reply.sceneStatus=="NO_EXISTING_SCENE_DATA" || reply.sceneStatus=="ROOMS_WITHOUT_CEILING_UUID" ||
                    reply.sceneStatus=="CEILING_UUIDS_NOT_LOADED")
                s.status="No room with a ceiling saved. Run the Meta room setup";
            else if(reply.sceneStatus=="WAITING_FOR_VALID_CEILING_POSE")
                s.status="Restoring ceiling tracking";
            else if(reply.sceneStatus=="SUCCESS_CEILING_DATA")
                s.status="No usable ceiling above the current position";
            else if(reply.sceneStatus.rfind("ERROR:",0)==0 || reply.sceneStatus=="OUTPUT_LIMIT_EXCEEDED")
                s.status="The saved room data cannot be used right now";
            else s.status="Loading the saved room";
            s.due=now+(s.locked?24ull*3600*1000:100);  // found: no further polling
        }
    }
    if(!s.nonce.empty() && now-s.pendingAt>500) {
        s.nonce.clear();if(!s.locked) s.haveSurface=false;s.capable=false;s.due=now+2000;
        s.status="Room host not reachable. Start GTA2 in the GTA2Ceiling host";
    }
    if(now<s.due || !s.nonce.empty()) return;
    s.nonce="gcc_"+std::to_string(++s.nonceCounter);s.request=s.capable?"GET_ROOM":"CAPS";
    const std::string token=s.capable?RoomToken():std::string{};
    if(s.capable && token.empty()) {
        s.nonce.clear();if(!s.locked) s.haveSurface=false;s.due=now+2000;
        s.status="Room access not connected yet. Restart the GTA2Ceiling host";return;
    }
    std::ostringstream request;request.imbue(std::locale::classic());
    request<<"GCCROOM1 "<<s.request<<' '<<s.nonce;
    if(s.capable) request<<" 1 "<<s.session<<' '<<s.epoch<<' '<<token;
    const auto message=request.str();
    sendto(s.socket,message.data(),static_cast<int>(message.size()),0,reinterpret_cast<sockaddr*>(&s.host),sizeof(s.host));
    s.pendingAt=now;s.due=now+100;
#else
    (void)frame;
#endif
}
}
