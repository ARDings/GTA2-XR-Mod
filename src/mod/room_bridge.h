#pragma once
#include "gta2ceiling/room_surface.h"
#include "gta2ceiling/xr_transport.h"
#include <memory>

namespace gta2ceiling {
struct RoomReply {
    std::string request,nonce,status,session,ceilingUuid,sceneStatus;
    uint64_t epoch=0;
    int64_t xrTime=0;
    uint32_t recenter=0;
    uint8_t hmdSync=0;
    bool sceneCapable=false;
    std::vector<RoomSurface> surfaces;
    std::vector<std::string> ceilingUuids;
    std::vector<RoomSurface> floorSurfaces;
    std::vector<std::string> floorUuids;
};
bool ParseRoomReply(std::string_view text,RoomReply& output,std::string& error);
// Room geometry is static in its reference space, so it need not share the
// exact eye frame. Accept only nearby host samples within one unambiguous sync
// ring interval; the caller separately verifies session and reference epoch.
bool RoomReplyMatchesTracking(const RoomReply& reply,const XrTrackingFrame& frame,
                              uint64_t requestAgeMs);
class RoomBridge {
public:
    RoomBridge(); ~RoomBridge();
    void Poll(const XrTrackingFrame& frame);
    void Stop();
    // Selects the scanned FLOOR below the head instead of the CEILING above.
    void SetFloorMode(bool floor);
    const RoomSurface* Surface() const;
    const std::string& CeilingUuid() const;
    const std::string& Status() const;
private:
    struct State; std::unique_ptr<State> state_;
};
}
