#pragma once
#include "gta2ceiling/xr_transport.h"
#include "camera.h"
#include <d3d9.h>
#include <cstdio>
#include "placement_controls.h"
#include "room_bridge.h"
#include <functional>
#include <memory>

namespace gta2ceiling {
// Runs on the original GTA2 graphics thread. The network receiver owns no D3D
// resources and each SBS pair uses one immutable tracking packet.
class CeilingBridge {
public:
    void Configure();
    void Shutdown();
    bool Prepare(const gta2::Vec3& cityCenter, gta2::Camera (&eyes)[2]);
    void DrawHud(IDirect3DDevice9* device, IDirect3DTexture9* texture, Eye eye, bool inWorld);
    void DrawPlacementGuide(IDirect3DDevice9* device, Eye eye);
    bool ConsumeController(const GamepadState& state);
    bool Placing() const { return placement_ && placement_->Active(); }
    float HalfWidthTiles() const { return width_/(2*scale_); }
    float HalfDepthTiles() const { return depth_/(2*scale_); }
    // Radius of the round play field's visible disc, in map tiles.
    float FieldRadiusTiles() const { return 0.5f*FieldDiameter()/scale_; }
    // underlay: see DrawRoomCeiling (the walk-out player, round field only).
    bool DrawBaseAndClip(IDirect3DDevice9* device, Eye eye, const gta2::Vec3& cityCenter,
                         const std::function<void()>* underlay = nullptr);
    bool MixedReality() const { return mixedReality_; }
    void StampSync(IDirect3DDevice9* device, int width, int height);
    bool Active() const { return transport_.running(); }
    uint64_t SampleId() const { return frame_.local_sample_id; }
    void RecordGameBoundary();
    void RecordPresent(double drawAndPresentSeconds);
    uint64_t RenderedFrames() const { return frames_; }
    // Head-pose-only refresh between game frames: worth a new stereo pair only
    // when the head moved enough for parallax to matter, or before the host's
    // pose ring (about 300 ms) would lose the last rendered pose. Rotation alone
    // is reprojected exactly by the compositor.
    bool PoseRefreshWorthwhile() const;
    bool PoseRefreshEnabled() const { return poseRefresh_; }
    // Circle field only: hold the right stick 4 s to freeze the city in place
    // while the game goes on, so the player drives or walks out of the disc
    // onto the real floor and back in anywhere; hold again to let the city
    // follow again (see walk_out.h in the renderer).
    bool WorldLocked() const { return worldLocked_ && shape_==Shape::Circle; }
    // How far around the player GTA2 spawns, keeps and draws cars and people,
    // as a multiple of the stock view rectangle (Y in the menu: 1x to 3x).
    float ViewRange() const { return viewRange_; }
    // X in the menu: buildings cut open by the round field's edge are closed
    // with a holographic surface on the cut (WorldView::DrawHoloCut). Off by default.
    bool HoloCuts() const { return holoCuts_; }
    // RT in the menu: modern controls (modern_controls.h). Off by default.
    bool ModernControls() const { return modernControls_; }
    // The user's own options file (gta2ceiling_user_graphics.ini), empty if unknown.
    const std::string& UserOptionsPath() const { return userGraphicsPath_; }
    // The round play field (cut with the six planes of SetCityHexClip).
    bool RoundField() const { return shape_==Shape::Circle; }
    // Latest controller sample.
    const GamepadState& LastPad() const { return lastPad_; }
    bool CityOnFloor() const { return floor_; }
    // A new level starts with the city following the player again.
    void ReleaseWorld() { worldLocked_ = false; r3HeldSeconds_ = 0; }
    // Age of the tracking sample used for this pair (ms) and head turn rate.
    double PoseAgeMs() const {
        return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-frame_.received_at).count();
    }
    float HeadYawPitch(float& pitch) const;
    uint32_t SyncColour() const { return XrTransport::encode_sync_pixel(frame_.hmd_sync); }
    // Hidden graphics options inside the placement menu (X / Y).
    bool TakeGraphicsRequest(int& renderScalePercent, unsigned& msaaSamples);
    void SetGraphicsEffective(int renderScalePercent, unsigned msaaSamples);
    bool HighEnd() const { return highEnd_; }
    bool Shadows() const { return shadows_; }
    // Real-world occlusion only applies to the city on the room surface, never
    // to head-locked menus or the placement panel.
    void SetOcclusionContext(bool cityVisible) { occlusionContext_=cityVisible; WriteOcclusionFlag(); }
private:
    XrTransport transport_;
    RoomBridge room_;
    const RoomSurface* roomSurface_=nullptr;
    bool roomScanEnabled_=true;
    bool roomWasAvailable_=false;
    XrTrackingFrame frame_;
    bool enabled_ = true, configured_ = false, anchored_ = false;
    uint64_t retry_at_ms_ = 0, frames_ = 0;
    uint32_t recenter_ = 0;
    float distance_ = 1.6f, scale_ = 0.12f;
    float width_ = 2.4f, depth_ = 2.4f;
    bool mixedReality_ = true;
    bool immersiveProjection_ = true;
    Vec3 anchor_;
    Vec3 reference_;
    float yaw_=0;
    std::unique_ptr<PlacementFileStore> placementFile_;
    std::unique_ptr<PlacementControls> placement_;
    uint64_t previousInputMs_=0, guideRefreshMs_=0, statusUntilMs_=0;
    std::string placementStatus_;
    std::string userGraphicsPath_;
    int renderScale_=100, requestedScale_=100;
    unsigned msaa_=2, requestedMsaa_=2;
    bool graphicsPending_=false;
    bool floor_=false, occlusion_=true, occlusionContext_=false, highEnd_=false;
    // Play field: the whole scanned surface or a 1 m circle on it.
    enum class Shape { Room, Circle };
    Shape shape_=Shape::Room;
    RoomSurface circleSurface_;
    Vec3 placementBase_{};
    float menuAnchorQ_[4]{0,0,0,1};
    Vec3 menuAnchorP_{};
    bool menuAnchorValid_=false, menuFollowing_=false;
    uint64_t menuAnchorSample_=0;
    void UpdateMenuAnchor();
    Mat4 MenuToWorld() const;
    Vec3 circleCentre_{};
    float circleDiameter_=0;
    bool OnFloor() const { return floor_; }
    float FieldDiameter() const { return circleSize_; }
    float circleSize_=1.0f;  // circle field diameter in metres (D-pad up/down)
    void SaveFieldOptions();
    bool previousRightTrigger_=false, previousLeftTrigger_=false, shadows_=true;
    bool poseRefresh_=false;
    bool worldLocked_=false, r3Latched_=false;
    float viewRange_=1.0f;
    bool holoCuts_=false, modernControls_=false;
    GamepadState lastPad_{};
    double r3HeldSeconds_=0;
    int writtenOcclusion_=-1;
    XrApiPose renderedPose_{};
    uint64_t renderedAtMs_=0;
    std::string writtenOcclusionText_;
    // Diagnostics (gta2dx9.log): state changes and a 10 s summary.
    std::string loggedRoomStatus_;
    const RoomSurface* loggedSurface_=nullptr;
    float loggedSurfaceY_=0, loggedSurfaceArea_=0;
    bool trackingLost_=false;
    uint64_t trackingLostAt_=0, summaryAt_=0, summaryPresents_=0, staleEvents_=0;
    double summaryMaxMs_=0, summarySumMs_=0;
    void WriteOcclusionFlag();
    uint16_t previousButtons_=0;
    IDirect3DTexture9* guideTexture_=nullptr;
    FILE* ticks_ = nullptr;
    FILE* presents_ = nullptr;
    int64_t clockFrequency_ = 0, clockOrigin_ = 0, previousTick_ = 0;
};
bool VerifyOriginalGame(std::string& reason);
}
