#include "gta2ceiling/ceiling_geometry.h"
#include "gta2ceiling/xr_transport.h"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace gta2ceiling;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void equal(float actual, float expected, const char* message, float tolerance=0.00002f) {
    if (!std::isfinite(actual) || std::abs(actual-expected)>tolerance) {
        std::cerr << message << ": actual=" << actual << " expected=" << expected << '\n';
        throw std::runtime_error(message);
    }
}
Vec4 transform(const Vec3& point, const Mat4& matrix, float w=1) {
    return {point.x*matrix.m[0]+point.y*matrix.m[4]+point.z*matrix.m[8]+w*matrix.m[12],
            point.x*matrix.m[1]+point.y*matrix.m[5]+point.z*matrix.m[9]+w*matrix.m[13],
            point.x*matrix.m[2]+point.y*matrix.m[6]+point.z*matrix.m[10]+w*matrix.m[14],
            point.x*matrix.m[3]+point.y*matrix.m[7]+point.z*matrix.m[11]+w*matrix.m[15]};
}
Vec3 xyz(const Vec4& value) { return {value.x,value.y,value.z}; }
Vec3 project(const Vec3& point, const Mat4& modelView, const Mat4& projection) {
    const auto camera=transform(point,modelView);
    const auto clip=transform(xyz(camera),projection,camera.w);
    require(clip.w>0, "Ceiling point is behind the eye");
    return {clip.x/clip.w,clip.y/clip.w,clip.z/clip.w};
}
float viewportArea(const Vec3& a, const Vec3& b, const Vec3& c) {
    // The D3D viewport flips NDC Y. Positive signed area is clockwise in screen
    // coordinates (Y down), the front faces retained by D3DCULL_CCW.
    return (b.x-a.x)*(-c.y+a.y)-(-b.y+a.y)*(c.x-a.x);
}
XrTrackingFrame lookingUp() {
    XrTrackingFrame frame;
    const float r=std::sqrt(0.5f);
    frame.pose={r,0,0,r,0,0,0};
    frame.ipd_meters=0.064f;
    frame.pose_valid=true;
    return frame;
}
void checkPlacement() {
    // A real-size tile-offset example, independent of matrix coefficients.
    Mat4 model;
    require(TryMakeCeilingCityModel({120,3,200},{0.5f,2.3f,-0.2f},0.08f,model), "Valid placement rejected");
    auto centre=transform({120,3,200},model);
    equal(centre.x,0.5f,"City centre X must equal anchor");
    equal(centre.y,2.3f,"City centre Y must equal anchor");
    equal(centre.z,-0.2f,"City centre Z must equal anchor");
    auto roof=transform({122,5,199},model);
    equal(roof.x,0.66f,"Two east tiles must occupy 16 cm east");
    equal(roof.y,2.14f,"Two storeys must extend 16 cm down towards viewer");
    equal(roof.z,-0.28f,"One south tile must occupy 8 cm south");
    const auto normal=transform({0,1,0},model,0);
    equal(normal.x,0,"Roof outward normal has no X component");
    equal(normal.y,-0.08f,"Roof outward height direction must face viewer below");
    equal(normal.z,0,"Roof outward normal has no Z component");
    require(TryMakeCeilingCityModel({120,3,200},{0.5f,2.3f,-0.2f},0.08f,model,1.57079632679f),
            "Rotated ceiling placement rejected");
    roof=transform({122,5,199},model);
    equal(roof.x,0.42f,"90 degree ceiling yaw must rotate south toward west");
    equal(roof.y,2.14f,"Ceiling yaw changed vertical relief");
    equal(roof.z,-0.36f,"90 degree ceiling yaw must rotate east toward south");
}
void checkRoofVisibility() {
    // Same corner order as an ordinary upstream world_mesh.cpp lid: NW, NE,
    // SE, SW. Explicit coordinates exercise the actual front-face convention.
    const std::array<Vec3,4> roof{{{-1,1,1},{1,1,1},{1,1,-1},{-1,1,-1}}};
    const float r=std::sqrt(0.5f);
    const std::array<XrApiPose,3> poses{{
        {r,0,0,r,0,0,0},                  // straight up
        {0.5f,0,0,std::sqrt(0.75f),0,0,0}, // pitched 30 degrees away from zenith
        {0.5f,0.5f,-0.5f,0.5f,0.1f,0.02f,-0.05f}, // up, turned 90 degrees, shifted
    }};
    Mat4 model;
    require(TryMakeCeilingCityModel({0,0,0},{0,1.6f,0},0.08f,model), "Roof model rejected");
    for (const auto& pose : poses) {
        auto frame=lookingUp();
        frame.pose=pose;
        // Independent asymmetric eye frusta must not reverse the visible face.
        frame.fov_left={-0.6f,0.8f,0.75f,-0.7f};
        frame.fov_right={-0.8f,0.6f,0.7f,-0.75f};
        for (Eye eye : {Eye::Left,Eye::Right}) {
            Mat4 view, projection;
            XrTransport::compute_eye_transforms(frame,eye,view,projection);
            const Mat4 modelView=Mat4::multiply(model,view);
            std::array<Vec3,4> projected;
            for (size_t i=0;i<roof.size();++i) {
                projected[i]=project(roof[i],modelView,projection);
                require(std::abs(projected[i].x)<1 && std::abs(projected[i].y)<1,
                        "Roof fixture fell outside the eye frustum");
                require(projected[i].z>0 && projected[i].z<1,"Roof depth must be in D3D range");
            }
            require(viewportArea(projected[0],projected[1],projected[2])>0,
                    "First roof triangle was back-facing under D3DCULL_CCW");
            require(viewportArea(projected[0],projected[2],projected[3])>0,
                    "Second roof triangle was back-facing under D3DCULL_CCW");
            const auto a=xyz(transform(roof[0],modelView));
            const auto b=xyz(transform(roof[1],modelView));
            const auto c=xyz(transform(roof[2],modelView));
            const Vec3 frontNormal=Vec3::cross(b-a,c-a);
            require(Vec3::dot(frontNormal,a*-1)>0,
                    "Camera-space roof normal must point towards the eye");
            const auto floor=project({0,0,0},modelView,projection);
            const auto top=project({0,1,0},modelView,projection);
            require(top.z<floor.z,"Roof must have nearer depth than the street beneath it");
        }
    }
}
void checkPhysicalEyesAndHeadMotion() {
    const Vec3 cityCentre{100,4,120}, anchor{0,1.6f,0};
    for (float scale : {0.01f,0.1f}) { // tenfold world-scale change
        Mat4 model;
        require(TryMakeCeilingCityModel(cityCentre,anchor,scale,model),"Scale fixture rejected");
        auto frame=lookingUp();
        Mat4 leftView,rightView,leftProjection,rightProjection;
        XrTransport::compute_eye_transforms(frame,Eye::Left,leftView,leftProjection);
        XrTransport::compute_eye_transforms(frame,Eye::Right,rightView,rightProjection);
        const auto leftModelView=Mat4::multiply(model,leftView);
        const auto rightModelView=Mat4::multiply(model,rightView);
        const auto left=transform(cityCentre,leftModelView);
        const auto right=transform(cityCentre,rightModelView);
        equal(left.x,0.032f,"Left physical eye offset was scaled by city scale");
        equal(right.x,-0.032f,"Right physical eye offset was scaled by city scale");
        equal(left.x-right.x,0.064f,"Physical IPD must remain 64 mm");
        equal(left.z,1.6f,"Anchor distance must remain physical metres");
        const auto original=project(cityCentre,leftModelView,leftProjection);
        const auto other=project(cityCentre,rightModelView,rightProjection);
        equal(original.x-other.x,0.04f,"Expected 64mm / 1.6m stereo disparity");
        frame.pose.position_x=0.1f; // physical 10 cm movement, anchor unchanged
        Mat4 movedView,movedProjection;
        XrTransport::compute_eye_transforms(frame,Eye::Left,movedView,movedProjection);
        const auto moved=project(cityCentre,Mat4::multiply(model,movedView),movedProjection);
        equal(moved.x-original.x,-0.0625f,"10 cm head motion was scaled or head-locked");
        equal(moved.y,original.y,"Horizontal movement introduced vertical motion");
    }
}
void checkHud() {
    for (const auto size : {std::array<uint32_t,2>{2048,1024},{640,480},{512,1024}}) {
        std::array<CeilingHudVertex,4> quad;
        require(TryMakeCeilingHudQuad({0,1.6f,0},size[0],size[1],quad),"HUD fixture rejected");
        const float width=quad[1].x-quad[0].x, height=quad[0].z-quad[3].z;
        equal(width/height,static_cast<float>(size[0])/size[1],"HUD texture aspect was distorted");
        equal(height,0.96f,"HUD physical height must be stable across texture aspect ratios");
        for (const auto& vertex : quad) equal(vertex.y,1.42f,"HUD plane offset is wrong");
        equal((quad[0].x+quad[2].x)*0.5f,0,"HUD horizontal centre drifted");
        equal((quad[0].z+quad[2].z)*0.5f,0,"HUD vertical centre drifted");
        require(quad[0].u==0 && quad[0].v==0 && quad[1].u==1 && quad[1].v==0 &&
                quad[2].u==1 && quad[2].v==1 && quad[3].u==0 && quad[3].v==1,
                "HUD UV corner correspondence changed");
        const auto frame=lookingUp();
        for (Eye eye : {Eye::Left,Eye::Right}) {
            Mat4 view,projection;
            XrTransport::compute_eye_transforms(frame,eye,view,projection);
            const auto topLeft=project({quad[0].x,quad[0].y,quad[0].z},view,projection);
            const auto topRight=project({quad[1].x,quad[1].y,quad[1].z},view,projection);
            const auto bottomLeft=project({quad[3].x,quad[3].y,quad[3].z},view,projection);
            require(topLeft.x<topRight.x,"HUD text is horizontally mirrored");
            require(topLeft.y>bottomLeft.y,"HUD text is vertically mirrored");
            equal((topRight.x-topLeft.x)/(topLeft.y-bottomLeft.y),
                  static_cast<float>(size[0])/size[1],"Projected HUD aspect changed under symmetric FOV");
        }
    }
    std::array<CeilingHudVertex,4> shifted;
    require(TryMakeCeilingHudQuad({2,3,-4},640,480,shifted,0.3f,0.2f),"Shifted HUD rejected");
    equal(shifted[0].x,1.6f,"HUD left edge must follow anchor and 4:3 aspect");
    equal(shifted[0].y,2.8f,"HUD offset must follow anchor");
    equal(shifted[0].z,-3.7f,"HUD top must follow anchor + north");
}
void checkInvalidInputs() {
    Mat4 model=Mat4::translation({1,2,3});
    const float nan=std::numeric_limits<float>::quiet_NaN();
    for (float scale : {0.0f,-0.08f,nan,std::numeric_limits<float>::infinity()}) {
        require(!TryMakeCeilingCityModel({0,0,0},{0,1.6f,0},scale,model),"Invalid city scale accepted");
        equal(model.m[12],1,"Failed model generation changed output");
    }
    require(!TryMakeCeilingCityModel({nan,0,0},{0,1,0},0.08f,model),"Invalid city centre accepted");
    require(!TryMakeCeilingCityModel({0,0,0},{0,nan,0},0.08f,model),"Invalid anchor accepted");
    std::array<CeilingHudVertex,4> quad{};
    quad[0].x=123;
    require(!TryMakeCeilingHudQuad({0,1,0},0,480,quad),"Zero texture width accepted");
    require(!TryMakeCeilingHudQuad({0,1,0},640,0,quad),"Zero texture height accepted");
    require(!TryMakeCeilingHudQuad({0,1,0},640,480,quad,-1),"Negative HUD height accepted");
    require(!TryMakeCeilingHudQuad({0,1,0},640,480,quad,0.48f,nan),"NaN HUD offset accepted");
    equal(quad[0].x,123,"Failed HUD generation changed output");
}
}
int main() {
    try {
        checkPlacement();
        checkRoofVisibility();
        checkPhysicalEyesAndHeadMotion();
        checkHud();
        checkInvalidInputs();
        std::cout << "Ceiling placement, roof winding/depth in both eyes, physical IPD/head motion, and HUD geometry passed. CPU geometry only; no headset/GPU claim.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
