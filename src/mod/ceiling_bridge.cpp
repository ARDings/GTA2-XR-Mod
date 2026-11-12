#include "ceiling_bridge.h"
#include "gta2ceiling/ceiling_geometry.h"
#include "ceiling_mask.h"
#include "log.h"
#include "../../third_party/gta2-renderer/src/dynamic_draw.h"
#include <windows.h>
#include <bcrypt.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>

namespace gta2ceiling {
namespace {
gta2::Mat4 Convert(const Mat4& m) { gta2::Mat4 result; std::memcpy(result.m,m.m,sizeof(result.m)); return result; }
Vec3 RotateByPose(const XrApiPose& p, const Vec3& v) {
    const Vec3 q{p.orientation_x,p.orientation_y,p.orientation_z};
    const Vec3 t=Vec3::cross(q,v)*2.0f;
    return v+t*p.orientation_w+Vec3::cross(q,t);
}
void SetMatrix(IDirect3DDevice9* d, D3DTRANSFORMSTATETYPE type, const Mat4& value) {
    D3DMATRIX m; std::memcpy(&m,value.m,sizeof(m)); d->SetTransform(type,&m);
}
}
void CeilingBridge::Configure() {
    if (configured_) return;
    configured_ = true;
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr,path,MAX_PATH);
    char* leaf=std::strrchr(path,'\\');
    if (leaf) std::strcpy(leaf+1,"gta2ceiling.ini");
    enabled_=GetPrivateProfileIntA("xr","enabled",1,path)!=0;
    immersiveProjection_=GetPrivateProfileIntA("xr","projection_mode",1,path)!=0;
    distance_=GetPrivateProfileIntA("ceiling","distance_millimeters",1600,path)/1000.0f;
    scale_=GetPrivateProfileIntA("ceiling","scale_millionths",120000,path)/1000000.0f;
    distance_=std::clamp(distance_,0.7f,4.0f);
    scale_=std::clamp(scale_,0.01f,0.3f);
    width_=std::clamp(GetPrivateProfileIntA("ceiling","width_millimeters",2400,path)/1000.0f,0.5f,10.0f);
    depth_=std::clamp(GetPrivateProfileIntA("ceiling","depth_millimeters",2400,path)/1000.0f,0.5f,10.0f);
    mixedReality_=GetPrivateProfileIntA("ceiling","mixed_reality",1,path)!=0;
    roomScanEnabled_=GetPrivateProfileIntA("ceiling","room_scan",1,path)!=0;
    // Extra stereo pairs between game frames, only for a new head pose. The
    // host re-renders every display frame for the current head pose itself
    // (display-rate reprojection), so by default the game renders exactly
    // once per simulation frame: one steady rate, all CPU/GPU for the game.
    poseRefresh_=GetPrivateProfileIntA("xr","pose_refresh",0,path)!=0;
    spriteBodies_=GetPrivateProfileIntA("sprites","depth",1,path)!=0;
    if (leaf) {
        const std::string directory(path,static_cast<size_t>(leaf-path));
        placementFile_=std::make_unique<PlacementFileStore>(directory);
        userGraphicsPath_=directory+"\\gta2ceiling_user_graphics.ini";
        char surface[16]{};
        GetPrivateProfileStringA("ceiling","surface","ceiling",surface,sizeof(surface),userGraphicsPath_.c_str());
        floor_=std::strcmp(surface,"floor")==0;
        occlusion_=GetPrivateProfileIntA("occlusion","enabled",1,userGraphicsPath_.c_str())!=0;
        highEnd_=true;  // fixed: sharp texture filtering and smooth bodies (no profile switch any more)
        shadows_=false;  // GTA2Ceiling: shadows off for good (GPU budget)

        char shape[16]{};
        GetPrivateProfileStringA("ceiling","shape","room",shape,sizeof(shape),userGraphicsPath_.c_str());
        viewRange_=std::clamp(GetPrivateProfileIntA("graphics","view_range_percent",100,
                                                    userGraphicsPath_.c_str())/100.0f,1.0f,3.0f);
        holoCuts_=false;  // the holo cut is switched off for now (no menu entry)
        modernControls_=GetPrivateProfileIntA("controller","modern",1,userGraphicsPath_.c_str())!=0;  // modern by default
        spriteBodies_=GetPrivateProfileIntA("sprites","depth",spriteBodies_?1:0,userGraphicsPath_.c_str())!=0;
        circleSize_=std::clamp(GetPrivateProfileIntA("ceiling","circle_diameter_millimeters",1000,
                                                     userGraphicsPath_.c_str())/1000.0f,0.5f,3.0f);
        // The former table mode is gone; a saved "table" becomes the circle.
        shape_=std::strcmp(shape,"circle")==0||std::strcmp(shape,"table")==0?Shape::Circle:Shape::Room;
        gta2dx9::Log("play field: %s",shape);
        gta2dx9::Log("user options: surface=%s occlusion=%d profile=%s",floor_?"floor":"ceiling",
                     occlusion_?1:0,highEnd_?"high-end":"standard");
        WriteOcclusionFlag();
        PlacementSettings initial;
        initial.distance_m=distance_; initial.width_m=width_; initial.depth_m=depth_;
        initial.meters_per_tile=scale_;
        placement_=std::make_unique<PlacementControls>(initial,*placementFile_);
        placement_->LoadSaved();
        if (!placement_->LastError().empty()) gta2dx9::Log("placement load: %s",placement_->LastError().c_str());
    }
    LARGE_INTEGER frequency{}, now{};
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&now);
    clockFrequency_=frequency.QuadPart; clockOrigin_=now.QuadPart;
    if (leaf && GetPrivateProfileIntA("diagnostics","timing_csv",1,path)!=0) {
        std::strcpy(leaf+1,"gta2ceiling_ticks.csv"); ticks_=std::fopen(path,"w");
        std::strcpy(leaf+1,"gta2ceiling_present.csv"); presents_=std::fopen(path,"w");
        if (ticks_) std::fprintf(ticks_,"timestamp_ms,delta_ms,source\n");
        if (presents_) std::fprintf(presents_,"timestamp_ms,draw_present_ms,hmd_sync,sample_id\n");
    }
}
void CeilingBridge::Shutdown() {
    room_.Stop();roomSurface_=nullptr;roomWasAvailable_=false;
    if (placement_) placement_->Cancel();
    placement_.reset(); placementFile_.reset();
    if (guideTexture_) { guideTexture_->Release(); guideTexture_=nullptr; }
    previousInputMs_=guideRefreshMs_=statusUntilMs_=0;
    transport_.stop(); anchored_=false; configured_=false; previousTick_=0;
    if (ticks_) std::fclose(ticks_);
    if (presents_) std::fclose(presents_);
    ticks_=presents_=nullptr;
}
void CeilingBridge::RecordGameBoundary() {
    Configure();
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
    if (ticks_ && previousTick_ && clockFrequency_) {
        std::fprintf(ticks_,"%.6f,%.6f,gbh_EndScene\n",1000.0*(now.QuadPart-clockOrigin_)/clockFrequency_,
                     1000.0*(now.QuadPart-previousTick_)/clockFrequency_);
        std::fflush(ticks_);
    }
    previousTick_=now.QuadPart;
}
void CeilingBridge::RecordPresent(double seconds) {
    const uint64_t nowMs=GetTickCount64();
    ++summaryPresents_; summarySumMs_+=1000.0*seconds;
    summaryMaxMs_=std::max(summaryMaxMs_,1000.0*seconds);
    if (!summaryAt_) summaryAt_=nowMs;
    if (nowMs-summaryAt_>=10000) {
        const double span=(nowMs-summaryAt_)/1000.0;
        gta2dx9::Log("summary %.0fs: %.1f pairs/s, draw avg %.2f ms max %.2f ms, tracking gaps %llu, "
                     "surface=%s room=\"%s\" scale=%.3f m/block anchor=(%.2f,%.2f,%.2f) scene %d%% msaa %u profile=%s occl=%d",
            span,summaryPresents_/span,summaryPresents_?summarySumMs_/summaryPresents_:0.0,summaryMaxMs_,
            staleEvents_,floor_?"floor":"ceiling",room_.Status().c_str(),scale_,anchor_.x,anchor_.y,anchor_.z,
            renderScale_,msaa_,highEnd_?"high-end":"standard",writtenOcclusion_);
        summaryAt_=nowMs; summaryPresents_=0; summarySumMs_=summaryMaxMs_=0; staleEvents_=0;
    }
    if (!presents_ || !clockFrequency_) return;
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
    std::fprintf(presents_,"%.6f,%.6f,%u,%llu\n",1000.0*(now.QuadPart-clockOrigin_)/clockFrequency_,
                 1000.0*seconds,frame_.hmd_sync,frame_.local_sample_id);
    if ((frames_%30)==0) std::fflush(presents_);
}
bool CeilingBridge::Prepare(const gta2::Vec3& cityCenter, gta2::Camera (&eyes)[2]) {
    Configure();
    if (!enabled_) return false;
    if (!transport_.running()) {
        if (GetTickCount64()<retry_at_ms_) return false;
        retry_at_ms_=GetTickCount64()+3000;
        std::string error;
        XrTransportOptions options;
        options.immersive_projection=immersiveProjection_;
        if (!transport_.start(error,options)) { gta2dx9::Log("XR start pending: %s",error.c_str()); return false; }
        gta2dx9::Log("XR projection=%s",immersiveProjection_?"native VR (MODE1)":"flat SBS diagnostic with UDP (MODE2)");
        gta2dx9::Log("XrAPI 0.6 loopback transport started; awaiting measured tracking");
    }
    if (!transport_.latest_tracking(frame_)) {
        menuAnchorValid_=false;
        if (!trackingLost_) { trackingLost_=true; trackingLostAt_=GetTickCount64(); ++staleEvents_; }
        return false;
    }
    if (trackingLost_) {
        trackingLost_=false;
        const uint64_t gap=GetTickCount64()-trackingLostAt_;
        if (gap>=200) gta2dx9::Log("tracking resumed after %llu ms",gap);
    }
    UpdateMenuAnchor();
    roomSurface_=nullptr;
    if(roomScanEnabled_) {room_.SetFloorMode(OnFloor());room_.Poll(frame_);roomSurface_=room_.Surface();}
    if(roomScanEnabled_ && room_.Status()!=loggedRoomStatus_) {
        loggedRoomStatus_=room_.Status();
        gta2dx9::Log("room status: %s",loggedRoomStatus_.c_str());
    }
    if(roomSurface_ && (roomSurface_!=loggedSurface_ || std::fabs(roomSurface_->centre.y-loggedSurfaceY_)>0.02f ||
                        std::fabs(roomSurface_->area-loggedSurfaceArea_)>0.05f)) {
        gta2dx9::Log("room %s: centre (%.2f,%.2f,%.2f) area %.2f m2, %zu corners, head y %.2f",
            floor_?"floor":"ceiling",roomSurface_->centre.x,roomSurface_->centre.y,roomSurface_->centre.z,
            roomSurface_->area,roomSurface_->boundary.size(),frame_.pose.position_y);
        loggedSurfaceY_=roomSurface_->centre.y; loggedSurfaceArea_=roomSurface_->area;
    }
    loggedSurface_=roomSurface_;
    if(roomSurface_) roomWasAvailable_=true;
    if (placement_) {
        const auto& settings=placement_->GetSettings();
        distance_=settings.distance_m; width_=settings.width_m; depth_=settings.depth_m;
        scale_=settings.meters_per_tile; yaw_=settings.yaw_radians;
    }
    if (!anchored_ || recenter_!=frame_.recenter_count) {
        reference_={frame_.pose.position_x,frame_.pose.position_y,frame_.pose.position_z};
        recenter_=frame_.recenter_count;
        anchored_=true;
        gta2dx9::Log("ceiling reference %.3f %.3f %.3f, source recenter %u",reference_.x,reference_.y,reference_.z,recenter_);
    }
    anchor_=reference_+Vec3{0,OnFloor()?-distance_:distance_,0};
    if (placement_) {
        anchor_.x+=placement_->GetSettings().offset_x_m;
        anchor_.z+=placement_->GetSettings().offset_z_m;
    }
    placementBase_=roomSurface_?roomSurface_->centre:reference_;
    if(shape_!=Shape::Room) {
        // A round field instead of the whole surface: centred on the scanned
        // ceiling/floor (or the fallback reference) plus the placement offset.
        // The left stick moves the field itself; the city stays centred in it.
        const Vec3 base=roomSurface_?roomSurface_->centre:Vec3{reference_.x,anchor_.y,reference_.z};
        Vec3 centre{base.x,base.y,base.z};
        if(placement_) { centre.x+=placement_->GetSettings().offset_x_m; centre.z+=placement_->GetSettings().offset_z_m; }
        const float diameter=FieldDiameter();
        if(circleSurface_.boundary.empty() || std::fabs(centre.x-circleCentre_.x)>0.001f ||
           std::fabs(centre.y-circleCentre_.y)>0.001f || std::fabs(centre.z-circleCentre_.z)>0.001f ||
           diameter!=circleDiameter_) {
            // Built directly, not through the room-scan validator.
            constexpr int kSegments=48;
            const float radius=0.5f*diameter;
            circleSurface_={};
            for(int i=0;i<kSegments;++i) {
                const float a=6.2831853f*static_cast<float>(i)/kSegments;
                circleSurface_.boundary.push_back({centre.x+radius*std::cos(a),centre.y,centre.z+radius*std::sin(a)});
            }
            for(uint32_t i=1;i+1<kSegments;++i) {
                circleSurface_.triangles.push_back(0);
                circleSurface_.triangles.push_back(i);
                circleSurface_.triangles.push_back(i+1);
            }
            circleSurface_.centre=centre;
            circleSurface_.minimum={centre.x-radius,centre.y,centre.z-radius};
            circleSurface_.maximum={centre.x+radius,centre.y,centre.z+radius};
            circleSurface_.area=3.14159265f*radius*radius;
            static uint64_t lastFieldLog=0;
            if(GetTickCount64()-lastFieldLog>1000) {
                lastFieldLog=GetTickCount64();
                gta2dx9::Log("play field circle: centre (%.2f,%.2f,%.2f) diameter %.2f m",
                    centre.x,centre.y,centre.z,diameter);
            }
            circleCentre_=centre; circleDiameter_=diameter;
        }
        roomSurface_=circleSurface_.boundary.empty()?nullptr:&circleSurface_;
        anchor_=centre;
        width_=depth_=diameter+0.05f;
    } else if(roomSurface_) {
        anchor_=roomSurface_->centre;
        // The physical scan defines the mask. Placement adjusts the city under
        // it; it never replaces the scan by a guessed rectangle.
        if(placement_) {
            anchor_.x+=placement_->GetSettings().offset_x_m;
            anchor_.z+=placement_->GetSettings().offset_z_m;
        }
        const float w=roomSurface_->maximum.x-roomSurface_->minimum.x;
        const float h=roomSurface_->maximum.z-roomSurface_->minimum.z;
        const float offsetX=anchor_.x-roomSurface_->centre.x;
        const float offsetZ=anchor_.z-roomSurface_->centre.z;
        // CPU batches are centred on the player/city, while the scan mask
        // remains fixed when the city is panned. Include that displacement so
        // the far side of the real ceiling cannot lose otherwise visible tiles.
        width_=depth_=std::sqrt(w*w+h*h)+2*std::hypot(offsetX,offsetZ)+0.1f;
    }
    // GTA2 is left-handed, OpenXR is right-handed. Reflect Y so rooftops face
    // the person below; the XR view's Z reflection restores D3D's winding.
    Mat4 model;
    if (!TryMakeCeilingCityModel({cityCenter.x,cityCenter.y,cityCenter.z},anchor_,scale_,model,yaw_,OnFloor())) return false;
    for (int i=0;i<2;++i) {
        Mat4 view, projection;
        XrTransport::compute_eye_transforms(frame_,i==0?Eye::Left:Eye::Right,view,projection);
        eyes[i].SetMatrices(Convert(Mat4::multiply(model,view)),Convert(projection));
    }
    return true;
}
bool CeilingBridge::DrawBaseAndClip(IDirect3DDevice9* d, Eye eye, const gta2::Vec3& cityCenter,
                                    const std::function<void()>* underlay) {
    if(roomScanEnabled_ && !roomSurface_) return false;
    Mat4 view, projection;
    XrTransport::compute_eye_transforms(frame_,eye,view,projection);
    // GTA2 blocks span up to 8 levels; keep the whole hanging building height
    // visible in front of the real walls instead of cutting at the outline.
    if(roomSurface_) {
        const Vec3 viewer{frame_.pose.position_x,frame_.pose.position_y,frame_.pose.position_z};
        // The walk-out player is drawn first (underlay), so the wall band that
        // keeps the buildings whole cannot hide it when it is behind the disc.
        if(!DrawRoomCeiling(d,view,projection,*roomSurface_,8.0f*scale_+0.05f,OnFloor(),
                            shape_!=Shape::Room?&viewer:nullptr,underlay)) return false;
        // Round field (may be seen from outside): also cut the city to
        // a hexagon around it so nothing outside stands in front of the edge.
        if(shape_!=Shape::Room) SetCityHexClip(d,{cityCenter.x,cityCenter.y,cityCenter.z},scale_,0.5f*FieldDiameter());
        return true;
    }
    // Rotate the physical base about its own centre, matching the city model.
    Mat4 rotate=Mat4::multiply(Mat4::translation(anchor_*-1),CeilingYaw(yaw_));
    rotate=Mat4::multiply(rotate,Mat4::translation(anchor_));
    if (!DrawCeilingBase(d,Mat4::multiply(rotate,view),projection,anchor_,width_,depth_,OnFloor())) return false;
    return SetCityClip(d,{cityCenter.x,cityCenter.y,cityCenter.z},scale_,width_,depth_);
}
void CeilingBridge::DrawHud(IDirect3DDevice9* d, IDirect3DTexture9* texture, Eye eye, bool inWorld) {
    if (!d || !texture) return;
    if(inWorld && roomScanEnabled_ && !roomSurface_) return;
    if(!inWorld) EndRoomCeilingMask(d);
    ClearClip(d);
    Mat4 view, projection;
    XrTransport::compute_eye_transforms(frame_,eye,view,projection);
    SetMatrix(d,D3DTS_WORLD,Mat4::identity());
    SetMatrix(d,D3DTS_VIEW,view);
    SetMatrix(d,D3DTS_PROJECTION,projection);
    D3DSURFACE_DESC description{};
    if (FAILED(texture->GetLevelDesc(0,&description)) || !description.Height) return;
    std::array<CeilingHudVertex,4> quad;
    // On a round field the HUD shrinks with it so it stays inside the circle.
    const float hudHalfHeight=shape_==Shape::Room?0.48f:0.28f*FieldDiameter(); // corners stay inside
    const float hudLift=shape_==Shape::Room?0.18f:0.1f;
    if (!TryMakeCeilingHudQuad(anchor_,description.Width,description.Height,quad,hudHalfHeight,hudLift)) return;
    if (OnFloor()) {
        // Same panel turned 180 degrees about X: above the floor, readable from above.
        for (auto& vertex:quad) {
            vertex.y=anchor_.y+(anchor_.y-vertex.y);
            vertex.z=anchor_.z-(vertex.z-anchor_.z);
        }
    }
    if (inWorld) {
        const float c=std::cos(yaw_),s=std::sin(yaw_);
        for (auto& vertex:quad) {
            const float x=vertex.x-anchor_.x,z=vertex.z-anchor_.z;
            vertex.x=anchor_.x+x*c+z*s; vertex.z=anchor_.z-x*s+z*c;
        }
    }
    if (!inWorld) {
        // Start/menu controls remain within reach before the ceiling is placed.
        // A parallel stereo panel 1.6 m in front of the head; game HUD stays on
        // the physical ceiling. Physical IPD is never scaled with the city.
        // World-locked (lazy follow) instead of head-locked: reprojection of a
        // head-locked panel made the menu judder on every head movement.
        SetMatrix(d,D3DTS_WORLD,MenuToWorld());
        const float h=0.42f,w=h*description.Width/description.Height,z=1.6f;
        quad={{{-w,h,z,0,0},{w,h,z,1,0},{w,-h,z,1,1},{-w,-h,z,0,1}}};
    }
    d->SetRenderState(D3DRS_ZENABLE,FALSE); d->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
    d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE); d->SetRenderState(D3DRS_LIGHTING,FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE); d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
    d->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS,TRUE);
    d->SetRenderState(D3DRS_COLORWRITEENABLE,inWorld?0x7:0xf);
    d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,FALSE);
    d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);
    d->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
    d->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
    d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);
    d->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE);
    d->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_ONE); d->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
    d->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1); d->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);
    d->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1); d->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);
    d->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE);
    d->SetTexture(0,texture); d->SetFVF(D3DFVF_XYZ|D3DFVF_TEX1);
    gta2::DynamicDraw::Instance().Draw(d,D3DPT_TRIANGLEFAN,2,quad.data(),sizeof(CeilingHudVertex));
    d->SetTexture(0,nullptr); d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
}
void CeilingBridge::UpdateMenuAnchor() {
    // Menus float in front of you and stay put in the room; they glide after
    // the head only once you look more than ~25 degrees away (like Quest UI).
    // Once per tracking sample (72 Hz), however often the idle pacer calls in.
    if (menuAnchorValid_ && frame_.local_sample_id==menuAnchorSample_) return;
    menuAnchorSample_=frame_.local_sample_id;
    const Vec3 head{frame_.pose.position_x,frame_.pose.position_y,frame_.pose.position_z};
    float q[4]={frame_.pose.orientation_x,frame_.pose.orientation_y,frame_.pose.orientation_z,frame_.pose.orientation_w};
    if (!menuAnchorValid_) {
        std::copy(q,q+4,menuAnchorQ_); menuAnchorP_=head; menuAnchorValid_=true; menuFollowing_=false; return;
    }
    XrApiPose anchorPose{}; anchorPose.orientation_x=menuAnchorQ_[0]; anchorPose.orientation_y=menuAnchorQ_[1];
    anchorPose.orientation_z=menuAnchorQ_[2]; anchorPose.orientation_w=menuAnchorQ_[3];
    const Vec3 forward=RotateByPose(frame_.pose,{0,0,-1}), anchorForward=RotateByPose(anchorPose,{0,0,-1});
    const float cosine=std::clamp(Vec3::dot(forward,anchorForward),-1.0f,1.0f);
    const float moved=(head-menuAnchorP_).length();
    if (cosine<0.906f || moved>0.4f) menuFollowing_=true;        // > ~25 deg or 40 cm
    if (!menuFollowing_) return;
    const float dot=q[0]*menuAnchorQ_[0]+q[1]*menuAnchorQ_[1]+q[2]*menuAnchorQ_[2]+q[3]*menuAnchorQ_[3];
    if (dot<0) for (float& v:q) v=-v;
    constexpr float rate=0.12f;
    float length=0;
    for (int i=0;i<4;++i) { menuAnchorQ_[i]+=(q[i]-menuAnchorQ_[i])*rate; length+=menuAnchorQ_[i]*menuAnchorQ_[i]; }
    length=std::sqrt(length);
    if (length>1e-6f) for (float& v:menuAnchorQ_) v/=length;
    menuAnchorP_=menuAnchorP_+(head-menuAnchorP_)*rate;
    if (cosine>0.9986f && moved<0.02f) menuFollowing_=false;    // settled (< ~3 deg)
}
Mat4 CeilingBridge::MenuToWorld() const {
    // Panel space: x right, y up, z forward (as in the D3D eye space); world is
    // OpenXR LOCAL (right-handed, -Z forward).
    XrApiPose anchorPose{}; anchorPose.orientation_x=menuAnchorQ_[0]; anchorPose.orientation_y=menuAnchorQ_[1];
    anchorPose.orientation_z=menuAnchorQ_[2]; anchorPose.orientation_w=menuAnchorQ_[3];
    const Vec3 r=RotateByPose(anchorPose,{1,0,0}), u=RotateByPose(anchorPose,{0,1,0}), f=RotateByPose(anchorPose,{0,0,-1});
    Mat4 m=Mat4::identity();
    m.m[0]=r.x; m.m[1]=r.y; m.m[2]=r.z;
    m.m[4]=u.x; m.m[5]=u.y; m.m[6]=u.z;
    m.m[8]=f.x; m.m[9]=f.y; m.m[10]=f.z;
    m.m[12]=menuAnchorP_.x; m.m[13]=menuAnchorP_.y; m.m[14]=menuAnchorP_.z;
    return m;
}
void CeilingBridge::SaveFieldOptions() {
    if (userGraphicsPath_.empty()) return;
    WritePrivateProfileStringA("ceiling","shape",shape_==Shape::Circle?"circle":"room",userGraphicsPath_.c_str());
    WritePrivateProfileStringA("ceiling","circle_diameter_millimeters",
        std::to_string(static_cast<int>(std::lround(circleSize_*1000))).c_str(),userGraphicsPath_.c_str());
    gta2dx9::Log("play field: %s, circle %.2f m",shape_==Shape::Circle?"circle":"whole surface",circleSize_);
}
void CeilingBridge::WriteOcclusionFlag() {
    // Read by the GTA2Ceiling host beside its private room token.
    const int wanted=(occlusion_ && occlusionContext_ && !Placing())?1:0;
    // A round field (circle/table) also tells the host where the city really
    // lies, in the XrAPI base space: "1 y centreX centreZ radius". A table is in
    // front of the room shell, so the shell alone would hide it behind the real
    // table top. Rounded to centimetres so the file is rewritten rarely.
    char text[96];
    if (wanted && shape_!=Shape::Room && !circleSurface_.boundary.empty())
        std::snprintf(text,sizeof(text),"1 %.2f %.2f %.2f %.2f",circleCentre_.y,circleCentre_.x,circleCentre_.z,
                      0.5f*circleDiameter_);
    else std::snprintf(text,sizeof(text),"%d",wanted);
    if (text==writtenOcclusionText_) return;
    if (FILE* flag=std::fopen("Z:\\tmp\\xr\\gta2ceiling_occlusion","wb")) {
        std::fputs(text,flag);
        std::fclose(flag);
        writtenOcclusion_=wanted;
        writtenOcclusionText_=text;
        gta2dx9::Log("occlusion flag for host: %s",text);
    }
}
bool CeilingBridge::TakeGraphicsRequest(int& renderScalePercent, unsigned& msaaSamples) {
    if (!graphicsPending_) return false;
    graphicsPending_=false;
    renderScalePercent=requestedScale_; msaaSamples=requestedMsaa_;
    return true;
}
void CeilingBridge::SetGraphicsEffective(int renderScalePercent, unsigned msaaSamples) {
    if (renderScalePercent==renderScale_ && msaaSamples==msaa_) return;
    renderScale_=renderScalePercent; msaa_=msaaSamples; guideRefreshMs_=0;
}
bool CeilingBridge::ConsumeController(const GamepadState& state) {
    Configure();
    if (!placement_) return false;
    const uint64_t now=GetTickCount64();
    const double dt=previousInputMs_?static_cast<double>(now-previousInputMs_)/1000.0:0;
    previousInputMs_=now;
    const bool wasPlacing=placement_->Active();
    const uint16_t pressed=state.available?static_cast<uint16_t>(state.buttons&~previousButtons_):0;
    previousButtons_=state.available?state.buttons:0;
    lastPad_=state;
    const bool consume=placement_->ConsumeInput(state,dt);
    {
        // Right stick (alone, outside the menu) held 4 s: freeze / release the city.
        const bool r3=state.available && (state.buttons&0x0080) && !(state.buttons&0x0040);
        if (r3 && !wasPlacing && !placement_->Active() && shape_==Shape::Circle) {
            r3HeldSeconds_+=std::clamp(dt,0.0,0.25);
            if (!r3Latched_ && r3HeldSeconds_>=4.0) {
                r3Latched_=true;
                worldLocked_=!worldLocked_;
                gta2dx9::Log("city %s",worldLocked_?"frozen in place: the game walks out of the circle":"follows the player again");
            }
        } else {
            r3HeldSeconds_=0; r3Latched_=false;
        }
        if (shape_!=Shape::Circle) worldLocked_=false;
    }
    if (wasPlacing && placement_->Active() && (pressed&(0x0004|0x0008))) {
        // D-pad left/right: play field (whole surface / circle).
        shape_=shape_==Shape::Room?Shape::Circle:Shape::Room;
        SaveFieldOptions();
        guideRefreshMs_=0;
    }
    if (wasPlacing && placement_->Active() && (pressed&0x8000)) {
        // Y: view range (where traffic and pedestrians appear), 1x .. 3x.
        viewRange_=viewRange_>=2.99f?1.0f:viewRange_+0.5f;
        if (!userGraphicsPath_.empty())
            WritePrivateProfileStringA("graphics","view_range_percent",
                std::to_string(static_cast<int>(std::lround(viewRange_*100))).c_str(),userGraphicsPath_.c_str());
        gta2dx9::Log("view range: %.1fx",viewRange_);
        guideRefreshMs_=0;
    }
    if (wasPlacing && placement_->Active() && shape_==Shape::Circle && (pressed&(0x0001|0x0002))) {
        // D-pad up/down: circle size, 0.5 to 3 m in 25 cm steps.
        circleSize_=std::clamp(circleSize_+((pressed&0x0001)?0.25f:-0.25f),0.5f,3.0f);
        SaveFieldOptions();
        guideRefreshMs_=0;
    }
    if (wasPlacing && placement_->Active() && (pressed&(0x0010|0x0020))) {
        // Back: ceiling <-> floor. Start: real-world occlusion on/off.
        if (pressed&0x0020) {
            floor_=!floor_;
            anchored_=false; // re-reference the fallback rectangle too
            if (!userGraphicsPath_.empty())
                WritePrivateProfileStringA("ceiling","surface",floor_?"floor":"ceiling",userGraphicsPath_.c_str());
            gta2dx9::Log("city surface: %s",floor_?"floor":"ceiling");
        }
        if (pressed&0x0010) {
            occlusion_=!occlusion_;
            if (!userGraphicsPath_.empty())
                WritePrivateProfileStringA("occlusion","enabled",occlusion_?"1":"0",userGraphicsPath_.c_str());
            WriteOcclusionFlag();
            gta2dx9::Log("real-world occlusion: %s",occlusion_?"on":"off");
        }
        guideRefreshMs_=0;
    }
    {
        // RT: classic (tank) or modern controls.
        const bool rightTrigger=state.available && state.right_trigger>128;
        if (rightTrigger && !previousRightTrigger_ && wasPlacing && placement_->Active()) {
            modernControls_=!modernControls_;
            if (!userGraphicsPath_.empty())
                WritePrivateProfileStringA("controller","modern",modernControls_?"1":"0",userGraphicsPath_.c_str());
            gta2dx9::Log("controls: %s",modernControls_?"modern":"classic");
            guideRefreshMs_=0;
        }
        previousRightTrigger_=rightTrigger;
    }
    const bool leftTrigger=state.available && state.left_trigger>128;
    const bool leftTriggerPressed=leftTrigger && !previousLeftTrigger_;
    previousLeftTrigger_=leftTrigger;
    if (leftTriggerPressed && wasPlacing && placement_->Active()) {
        // LT: stacked sprite bodies for cars and people, or flat sprites.
        spriteBodies_=!spriteBodies_;
        if (!userGraphicsPath_.empty())
            WritePrivateProfileStringA("sprites","depth",spriteBodies_?"1":"0",userGraphicsPath_.c_str());
        gta2dx9::Log("sprite bodies: %s",spriteBodies_?"stacked":"flat");
        guideRefreshMs_=0;
    }
    switch (placement_->TakeEvent()) {
    case PlacementEvent::Opened:
        placementStatus_="Place the ceiling field"; guideRefreshMs_=0;
        gta2dx9::Log("placement opened; game input captured"); break;
    case PlacementEvent::Committed:
        placementStatus_.clear(); statusUntilMs_=0; guideRefreshMs_=0;  // no toast: back to the game at once
        gta2dx9::Log("placement saved to gta2ceiling_user.ini"); break;
    case PlacementEvent::Cancelled:
        placementStatus_.clear(); statusUntilMs_=0; guideRefreshMs_=0;
        gta2dx9::Log("placement cancelled; committed settings restored"); break;
    case PlacementEvent::RecenterRequested: {
        // Put the followed player (the city centre) straight above the head.
        // The scan still defines the ceiling outline; only the city moves.
        // Offsets are relative to the scanned surface centre (or reference),
        // never to the round field, which already includes the offset.
        const float baseX=placementBase_.x;
        const float baseZ=placementBase_.z;
        if (frame_.pose_valid &&
            placement_->SetOffsets(frame_.pose.position_x-baseX,frame_.pose.position_z-baseZ)) {
            placementStatus_=OnFloor()?"Player centred below you":"Player centred above you";
            gta2dx9::Log("recentred city on head %.3f %.3f",frame_.pose.position_x,frame_.pose.position_z);
        } else {
            placementStatus_="Centring failed";
            gta2dx9::Log("recentre failed: %s",placement_->LastError().c_str());
        }
        statusUntilMs_=now+3000; guideRefreshMs_=0; break;
    }
    case PlacementEvent::SaveFailed:
        placementStatus_="Saving failed - A again / B discard"; guideRefreshMs_=0;
        gta2dx9::Log("placement save failed: %s",placement_->LastError().c_str()); break;
    default: break;
    }
    return consume;
}
void CeilingBridge::DrawPlacementGuide(IDirect3DDevice9* d,Eye eye) {
    EndRoomCeilingMask(d);
    const uint64_t now=GetTickCount64();
    if(roomScanEnabled_ && !roomSurface_) {
        const auto& roomStatus=room_.Status();
        placementStatus_=roomStatus.empty() ? "Loading the saved room" : roomStatus;
        statusUntilMs_=now+1000;
    }
    if (!d || !placement_ || (!Placing() && now>=statusUntilMs_)) return;
    constexpr int width=1600,height=1000;
    constexpr float statusStrip=0.14f; // top part of the texture used for status-only display
    if (!guideTexture_ || now>=guideRefreshMs_) {
        // Full mip chain: the panel is shown smaller than its texture, and
        // without mips the minified text shimmered and looked low-resolution.
        if (!guideTexture_ && FAILED(d->CreateTexture(width,height,0,D3DUSAGE_AUTOGENMIPMAP,D3DFMT_A8R8G8B8,
                                                      D3DPOOL_MANAGED,&guideTexture_,nullptr))) return;
        BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=width; info.bmiHeader.biHeight=-height;
        info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
        void* bits=nullptr;
        HDC dc=CreateCompatibleDC(nullptr);
        HBITMAP bitmap=dc?CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0):nullptr;
        if (!bitmap || !bits) { if(bitmap) DeleteObject(bitmap); if(dc) DeleteDC(dc); return; }
        HGDIOBJ oldBitmap=SelectObject(dc,bitmap);
        SetBkMode(dc,TRANSPARENT);
        // Palette: dark slate card, GTA-yellow accent, soft grey labels.
        const COLORREF backdrop=RGB(10,12,18), card=RGB(20,24,34), header=RGB(28,33,46),
            accent=RGB(255,196,42), chip=RGB(54,62,82), text=RGB(232,236,244), muted=RGB(150,158,176);
        auto fill=[&](RECT r,COLORREF colour,int radius,COLORREF border=CLR_INVALID,int borderWidth=0) {
            HBRUSH brush=CreateSolidBrush(colour);
            HPEN pen=CreatePen(PS_SOLID,borderWidth>0?borderWidth:1,border==CLR_INVALID?colour:border);
            HGDIOBJ ob=SelectObject(dc,brush), op=SelectObject(dc,pen);
            RoundRect(dc,r.left,r.top,r.right,r.bottom,radius,radius);
            SelectObject(dc,ob); SelectObject(dc,op); DeleteObject(brush); DeleteObject(pen);
        };
        auto makeFont=[](int size,int weight) {
            return CreateFontW(size,0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Tahoma");
        };
        HFONT titleFont=makeFont(58,FW_BOLD), sectionFont=makeFont(30,FW_BOLD), rowFont=makeFont(38,FW_NORMAL),
              valueFont=makeFont(38,FW_BOLD), chipFont=makeFont(30,FW_BOLD), statusFont=makeFont(34,FW_NORMAL);
        HGDIOBJ oldFont=SelectObject(dc,rowFont);
        auto write=[&](HFONT font,COLORREF colour,RECT r,const std::wstring& value,UINT format) {
            SelectObject(dc,font); SetTextColor(dc,colour);
            DrawTextW(dc,value.c_str(),-1,&r,format|DT_NOPREFIX|DT_SINGLELINE|DT_VCENTER);
        };
        auto widen=[](const std::string& value) {
            std::wstring out; out.reserve(value.size());
            for (unsigned char c:value) out.push_back(static_cast<wchar_t>(c));
            return out;
        };
        RECT all{0,0,width,height}; HBRUSH clear=CreateSolidBrush(backdrop); FillRect(dc,&all,clear); DeleteObject(clear);
        const std::wstring status=widen(placementStatus_);
        if (Placing()) {
            const auto& s=placement_->GetSettings();
            fill({6,6,width-6,height-6},card,56,accent,4);
            fill({10,10,width-10,128},header,48);
            RECT stripe{10,92,width-10,128}; HBRUSH hb=CreateSolidBrush(header); FillRect(dc,&stripe,hb); DeleteObject(hb);
            const wchar_t* title=!roomScanEnabled_?L"Place the ceiling field":OnFloor()?L"City on the floor":L"City on the ceiling";
            write(titleFont,text,{56,14,width-420,124},title,DT_LEFT);
            write(sectionFont,accent,{width-420,14,width-56,124},L"GTA2CEILING",DT_RIGHT);
            // One row: key chip, description, value (accent; muted when inactive).
            auto row=[&](int x,int y,int columnWidth,const wchar_t* key,const wchar_t* label,
                         const std::wstring& value,bool active=true) {
                constexpr int chipWidth=170,rowHeight=62;
                fill({x,y+6,x+chipWidth,y+rowHeight-6},chip,22);
                write(chipFont,text,{x,y,x+chipWidth,y+rowHeight},key,DT_CENTER);
                write(rowFont,active?text:muted,{x+chipWidth+22,y,x+columnWidth-190,y+rowHeight},label,DT_LEFT);
                write(valueFont,active?accent:muted,{x+columnWidth-200,y,x+columnWidth,y+rowHeight},value,DT_RIGHT);
            };
            auto section=[&](int x,int y,const wchar_t* name) {
                write(sectionFont,accent,{x,y,x+600,y+44},name,DT_LEFT);
                RECT line{x,y+46,x+700,y+49}; HBRUSH lb=CreateSolidBrush(chip); FillRect(dc,&line,lb); DeleteObject(lb);
            };
            wchar_t number[64];
            const int leftX=56,rightX=830,columnWidth=714,top=160,step=66;
            if (roomScanEnabled_) {
                section(leftX,top,L"PLAY FIELD");
                int y=top+60;
                row(leftX,y,columnWidth,L"L-Stick",shape_==Shape::Room?L"Move the city":L"Move the field",L""); y+=step;
                std::swprintf(number,std::size(number),L"%.0f deg",s.yaw_radians*180.0/3.141592653589793);
                row(leftX,y,columnWidth,L"R-Stick",L"Rotate",number); y+=step;
                std::swprintf(number,std::size(number),L"%.0f cm/block",s.meters_per_tile*100.0);
                row(leftX,y,columnWidth,L"LB / RB",L"Scale",number); y+=step;
                row(leftX,y,columnWidth,L"Back",L"Surface",OnFloor()?L"floor":L"ceiling"); y+=step;
                row(leftX,y,columnWidth,L"D-pad left/right",L"Play field",
                    shape_==Shape::Room?L"whole surface":L"circle"); y+=step;
                std::swprintf(number,std::size(number),L"%.2f m",circleSize_);
                row(leftX,y,columnWidth,L"D-pad up/down",L"Circle size",shape_==Shape::Circle?number:L"-",
                    shape_==Shape::Circle); y+=step;
                row(leftX,y,columnWidth,L"R3 4s",L"Freeze the city",WorldLocked()?L"on":L"off",shape_==Shape::Circle);
                section(rightX,top,L"DISPLAY");
                y=top+60;
                row(rightX,y,columnWidth,L"Start",L"Occlusion",occlusion_?L"on":L"off"); y+=step;
                std::swprintf(number,std::size(number),L"%.1fx",viewRange_);
                row(rightX,y,columnWidth,L"Y",L"View range",number); y+=step;
                row(rightX,y,columnWidth,L"RT",L"Controls",modernControls_?L"modern":L"classic"); y+=step;
                row(rightX,y,columnWidth,L"LT",L"Sprite bodies",spriteBodies_?L"stacked":L"flat");
            } else {
                section(leftX,top,L"FIELD");
                int y=top+60;
                row(leftX,y,columnWidth,L"L-Stick",L"Move",L""); y+=step;
                std::swprintf(number,std::size(number),L"%.2f m",s.distance_m);
                row(leftX,y,columnWidth,L"R-Stick",L"Height / rotation",number); y+=step;
                std::swprintf(number,std::size(number),L"%.1f x %.1f m",s.width_m,s.depth_m);
                row(leftX,y,columnWidth,L"D-pad",L"Width / depth",number); y+=step;
                std::swprintf(number,std::size(number),L"%.0f cm/block",s.meters_per_tile*100.0);
                row(leftX,y,columnWidth,L"LB / RB",L"Scale",number);
            }
            // Footer: confirm/cancel and the latest status message.
            RECT footer{10,height-128,width-10,height-10};
            HBRUSH fb=CreateSolidBrush(header); FillRect(dc,&footer,fb); DeleteObject(fb);
            fill({56,height-108,226,height-38},RGB(46,160,90),22);
            write(chipFont,text,{56,height-108,226,height-38},L"A",DT_CENTER);
            write(rowFont,text,{244,height-108,520,height-38},L"Save",DT_LEFT);
            fill({540,height-108,710,height-38},RGB(196,64,64),22);
            write(chipFont,text,{540,height-108,710,height-38},L"B",DT_CENTER);
            write(rowFont,text,{728,height-108,1000,height-38},L"Discard",DT_LEFT);
            write(statusFont,muted,{1010,height-108,width-56,height-38},status,DT_RIGHT|DT_END_ELLIPSIS);
        } else {
            // Status only: a slim pill in the top strip of the texture.
            const int stripHeight=static_cast<int>(height*statusStrip);
            fill({6,6,width-6,stripHeight-6},card,60,accent,4);
            write(statusFont,text,{40,6,width-40,stripHeight-6},status,DT_CENTER|DT_END_ELLIPSIS);
        }
        GdiFlush();
        auto* pixels=static_cast<uint32_t*>(bits);
        // Rounded corners: the untouched backdrop becomes transparent.
        const uint32_t backdropPixel=0x000A0C12u; // RGB(10,12,18) as BGRA
        for(int i=0;i<width*height;++i)
            pixels[i]=(pixels[i]&0x00FFFFFFu)==backdropPixel?0u:(pixels[i]|0xFF000000u);
        D3DLOCKED_RECT lock{};
        if(SUCCEEDED(guideTexture_->LockRect(0,&lock,nullptr,0))) {
            for(int row=0;row<height;++row)
                std::memcpy(static_cast<char*>(lock.pBits)+row*lock.Pitch,pixels+row*width,width*4);
            guideTexture_->UnlockRect(0);
            guideTexture_->GenerateMipSubLevels();
        }
        SelectObject(dc,oldFont);
        for (HFONT f:{titleFont,sectionFont,rowFont,valueFont,chipFont,statusFont}) if (f) DeleteObject(f);
        SelectObject(dc,oldBitmap); DeleteObject(bitmap); DeleteDC(dc);
        guideRefreshMs_=now+250;
    }
    ClearClip(d);
    Mat4 view,projection; XrTransport::compute_eye_transforms(frame_,eye,view,projection);
    // Same world-locked menu frame as the GTA2 menu panel (no reprojection judder).
    SetMatrix(d,D3DTS_WORLD,MenuToWorld()); SetMatrix(d,D3DTS_VIEW,view); SetMatrix(d,D3DTS_PROJECTION,projection);
    const float halfWidth=0.6f,halfHeight=Placing()?0.375f:0.375f*statusStrip,centreY=-0.3f,z=1.1f;
    const float bottomV=Placing()?1.0f:statusStrip;
    const std::array<CeilingHudVertex,4> quad{{
        {-halfWidth,centreY+halfHeight,z,0,0},{halfWidth,centreY+halfHeight,z,1,0},
        {halfWidth,centreY-halfHeight,z,1,bottomV},
        {-halfWidth,centreY-halfHeight,z,0,bottomV}}};
    d->SetVertexShader(nullptr); d->SetPixelShader(nullptr);
    d->SetRenderState(D3DRS_ZENABLE,FALSE); d->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
    d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE); d->SetRenderState(D3DRS_LIGHTING,FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE); d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
    d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,FALSE);
    d->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA); d->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS,TRUE);
    d->SetRenderState(D3DRS_COLORWRITEENABLE,0xf);
    d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);
    d->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
    d->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
    d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);
    d->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_LINEAR);
    d->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1); d->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);
    d->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1); d->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);
    d->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE);
    d->SetTexture(0,guideTexture_); d->SetFVF(D3DFVF_XYZ|D3DFVF_TEX1);
    gta2::DynamicDraw::Instance().Draw(d,D3DPT_TRIANGLEFAN,2,quad.data(),sizeof(CeilingHudVertex)); d->SetTexture(0,nullptr);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
}
float CeilingBridge::HeadYawPitch(float& pitch) const {
    const Vec3 f=RotateByPose(frame_.pose,{0,0,-1});
    pitch=std::asin(std::clamp(f.y,-1.0f,1.0f))*57.29578f;
    return std::atan2(-f.x,-f.z)*57.29578f;
}
bool CeilingBridge::PoseRefreshWorthwhile() const {
    // Only a truly still head skips: any real movement renders at full rate
    // (the city is ~1.5 m away, so eye translation from neck rotation shows
    // as judder if the image is only reprojected).
    if (!renderedAtMs_ || GetTickCount64()-renderedAtMs_>=50) return true;
    const float dx=frame_.pose.position_x-renderedPose_.position_x;
    const float dy=frame_.pose.position_y-renderedPose_.position_y;
    const float dz=frame_.pose.position_z-renderedPose_.position_z;
    if (dx*dx+dy*dy+dz*dz>0.001f*0.001f) return true;   // 1 mm
    const float dot=std::fabs(frame_.pose.orientation_x*renderedPose_.orientation_x+frame_.pose.orientation_y*renderedPose_.orientation_y+
        frame_.pose.orientation_z*renderedPose_.orientation_z+frame_.pose.orientation_w*renderedPose_.orientation_w);
    return dot<0.9999962f;                                 // > ~0.3 degrees
}
void CeilingBridge::StampSync(IDirect3DDevice9* d,int width,int height) {
    renderedPose_=frame_.pose; renderedAtMs_=GetTickCount64();
    const D3DVIEWPORT9 viewport{0,0,static_cast<DWORD>(width),static_cast<DWORD>(height),0,1};
    d->SetViewport(&viewport);
    d->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE);
    d->SetRenderState(D3DRS_DITHERENABLE,FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
    // An 8x8 block, not a single pixel: the host scales the window into its
    // eye buffers, and only a block keeps exact values in its interior. Next
    // to it a pure green 8x8 marker, so a black (or red) area of the image can
    // never be mistaken for a stamp. Pixel (0,0) is still the XrAPI sync pixel.
    // The same pair again in the bottom-right corner (torn-image check).
    const LONG w=width,h=height;
    const D3DRECT blocks[]{{0,0,8,8},{w-16,h-8,w-8,h}}, markers[]{{8,0,16,8},{w-8,h-8,w,h}};
    d->Clear(2,blocks,D3DCLEAR_TARGET,XrTransport::encode_sync_pixel(frame_.hmd_sync),1,0);
    d->Clear(2,markers,D3DCLEAR_TARGET,0xFF00FF00u,1,0);
    ++frames_;
    if (frames_%180==0) gta2dx9::Log("XR pairs=%llu sync=%u pose=(%.3f,%.3f,%.3f) q=(%.4f,%.4f,%.4f,%.4f) fov=(%.3f,%.3f) hostImmersiveScreen=%d SBS=%d",frames_,frame_.hmd_sync,
        frame_.pose.position_x,frame_.pose.position_y,frame_.pose.position_z,
        frame_.pose.orientation_x,frame_.pose.orientation_y,frame_.pose.orientation_z,frame_.pose.orientation_w,
        frame_.fov_left.angle_right-frame_.fov_left.angle_left,frame_.fov_left.angle_up-frame_.fov_left.angle_down,
        frame_.immersive_screen?1:0,frame_.sbs?1:0);
}
bool VerifyOriginalGame(std::string& reason) {
    char path[MAX_PATH]{}; GetModuleFileNameA(nullptr,path,MAX_PATH);
    FILE* f=std::fopen(path,"rb");
    if (!f) { reason="Cannot read the running GTA2 executable."; return false; }
    BCRYPT_ALG_HANDLE algorithm=nullptr; BCRYPT_HASH_HANDLE hash=nullptr;
    bool ok=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0;
    DWORD objectSize=0,got=0;
    if (ok) ok=BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectSize),sizeof(objectSize),&got,0)>=0;
    std::vector<unsigned char> object(objectSize), buffer(65536);
    if (ok) ok=BCryptCreateHash(algorithm,&hash,object.data(),objectSize,nullptr,0,0)>=0;
    size_t total=0,n=0;
    while (ok && (n=std::fread(buffer.data(),1,buffer.size(),f))!=0) {
        total+=n; ok=BCryptHashData(hash,buffer.data(),static_cast<ULONG>(n),0)>=0;
    }
    if (std::ferror(f)) ok=false;
    std::fclose(f);
    unsigned char digest[32]{};
    if (ok) ok=BCryptFinishHash(hash,digest,sizeof(digest),0)>=0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm,0);
    char hex[65]{};
    for (int i=0;i<32;++i) std::snprintf(hex+i*2,3,"%02x",digest[i]);
    if (!ok || total!=1667072 || std::strcmp(hex,"420af867036b1a66ef94c54b8db2e7e654da359031987c6602bcfa74972e089f")!=0) {
        reason="Unsupported GTA2 executable. GTA2Ceiling currently requires the verified v9.6 release; no hooks were installed.";
        return false;
    }
    return true;
}
}
