#include "../src/mod/room_bridge.h"
#include "../third_party/nlohmann-json/json.hpp"
#include <cassert>
#include <iostream>
using namespace gta2ceiling;
int main() {
    using Json=nlohmann::json;
    Json response={{"protocol","GCCROOM1"},{"request","CAPS"},{"nonce","test_1"},{"status","OK"},
        {"upstreamCommit","39b270c6401a69e7c28ea6ffd61de3dbac9e9b1e"},{"sessionId","abc012"},
        {"referenceEpoch",1},{"recenterCount",0},{"hmdSync",12},{"xrTime",1000},
        {"capabilities",{{"sceneQuery",true}}}};
    RoomReply result;std::string error;
    assert(ParseRoomReply(response.dump(),result,error) && result.sceneCapable);
    response["request"]="GET_ROOM";response["coordinateSpace"]="XRAPI_BASE";
    response["baseSpaceIndex"]=1;response["units"]="meters";
    Json ceiling={{"poseValid",true},{"poseTracked",true},{"locationFlags",15},{"ceilingUUID","012abc"},
        {"semanticLabels","CEILING"},{"xrTime",1000},
        {"polygon3DBase",{{-2,2.5,-2},{2,2.5,-2},{2,2.5,2},{-2,2.5,2}}}};
    response["scene"]={{"status","SUCCESS_CEILING_DATA"},{"ceilings",Json::array({ceiling})}};
    assert(ParseRoomReply(response.dump(),result,error) && result.surfaces.size()==1);
    assert(result.surfaces[0].area==16 && result.ceilingUuids[0]=="012abc");
    const auto valid=response;
    response["coordinateSpace"]="NATURAL_STAGE";
    assert(!ParseRoomReply(response.dump(),result,error));assert(result.surfaces.size()==1);
    response=valid;response["baseSpaceIndex"]=3;assert(!ParseRoomReply(response.dump(),result,error));
    response=valid;response["units"]="centimeters";assert(!ParseRoomReply(response.dump(),result,error));
    response=valid;response["upstreamCommit"]="unknown";assert(!ParseRoomReply(response.dump(),result,error));
    response=valid;response["nonce"]="bad token";assert(!ParseRoomReply(response.dump(),result,error));
    response=valid;response["referenceEpoch"]=-1;assert(!ParseRoomReply(response.dump(),result,error));
    response=valid;response["hmdSync"]=256;assert(!ParseRoomReply(response.dump(),result,error));
    response=valid;response["hmdSync"]=13;assert(!ParseRoomReply(response.dump(),result,error));
    response=valid;response["xrTime"]=-1;assert(!ParseRoomReply(response.dump(),result,error));
    response=valid;response.erase("hmdSync");assert(!ParseRoomReply(response.dump(),result,error));
    response=valid;response["scene"]["ceilings"][0]["xrTime"]=999;
    assert(!ParseRoomReply(response.dump(),result,error));
    // A labelled floor is accepted separately (city-on-floor mode)...
    response=valid;response["scene"]["ceilings"][0]["semanticLabels"]="FLOOR";
    assert(ParseRoomReply(response.dump(),result,error) && result.surfaces.empty() &&
           result.floorSurfaces.size()==1 && result.floorUuids[0]=="012abc");
    // ...anything else is still rejected.
    response=valid;response["scene"]["ceilings"][0]["semanticLabels"]="WALL_FACE";
    assert(!ParseRoomReply(response.dump(),result,error));
    response=valid;response["scene"]["ceilings"][0]["locationFlags"]=-1;
    assert(!ParseRoomReply(response.dump(),result,error));
    response=valid;response["scene"]["ceilings"][0]["poseValid"]=false;
    assert(ParseRoomReply(response.dump(),result,error) && result.surfaces.empty());
    response=valid;response["scene"]["ceilings"][0]["locationFlags"]=2;
    assert(ParseRoomReply(response.dump(),result,error) && result.surfaces.empty());
    response=valid;response["scene"]["ceilings"][0]["polygon3DBase"][2][1]=3.0;
    assert(ParseRoomReply(response.dump(),result,error) && result.surfaces.empty());
    response=valid;response["status"]="UNAUTHORIZED";
    assert(ParseRoomReply(response.dump(),result,error) && result.surfaces.empty());
    assert(!ParseRoomReply(std::string(60001,'x'),result,error));
    assert(!ParseRoomReply("{}",result,error));
    assert(ParseRoomReply(valid.dump(),result,error));
    XrTrackingFrame frame;frame.pose_valid=true;frame.hmd_sync=12;frame.recenter_count=0;
    assert(RoomReplyMatchesTracking(result,frame,0));
    assert(RoomReplyMatchesTracking(result,frame,100));
    assert(!RoomReplyMatchesTracking(result,frame,101));
    frame.hmd_sync=60;assert(RoomReplyMatchesTracking(result,frame,20));
    frame.hmd_sync=72;assert(!RoomReplyMatchesTracking(result,frame,20));
    result.hmdSync=252;frame.hmd_sync=0;assert(RoomReplyMatchesTracking(result,frame,20));
    result.hmdSync=0;frame.hmd_sync=252;assert(RoomReplyMatchesTracking(result,frame,20));
    frame.recenter_count=1;assert(!RoomReplyMatchesTracking(result,frame,20));
    frame.recenter_count=0;frame.pose_valid=false;assert(!RoomReplyMatchesTracking(result,frame,20));
    std::cout<<"Room bridge packet validation passed; no cross-space room import or invalid pose accepted.\n";
}
