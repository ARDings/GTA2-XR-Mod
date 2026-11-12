#include "../src/mod/ceiling_mask.h"
#include "gta2ceiling/ceiling_geometry.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace gta2ceiling;
namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool Inside(const CityClipPlanes& planes, const Vec3& point, float epsilon=0) {
    for (const auto& p : planes)
        if (p[0]*point.x+p[1]*point.y+p[2]*point.z+p[3] < -epsilon) return false;
    return true;
}
Vec3 Transform(const Vec3& v, const Mat4& m) {
    return {v.x*m.m[0]+v.y*m.m[4]+v.z*m.m[8]+m.m[12],
            v.x*m.m[1]+v.y*m.m[5]+v.z*m.m[9]+m.m[13],
            v.x*m.m[2]+v.y*m.m[6]+v.z*m.m[10]+m.m[14]};
}
void CheckPhysicalBounds() {
    const Vec3 center{128,3,200}, anchor{0.5f,2.2f,-0.3f};
    for (float scale : {0.01f,0.08f,0.2f}) {
        CityClipPlanes planes;
        Mat4 model;
        Require(TryMakeCityClipPlanes(center,scale,2.4f,1.6f,planes), "Clip construction failed");
        Require(TryMakeCeilingCityModel(center,anchor,scale,model), "City construction failed");
        // Check all four independent boundaries in physical metres against the
        // existing city transform. Height must not change horizontal clipping.
        for (int axis : {0,2}) for (float side : {-1.0f,1.0f}) {
            const float half=axis==0?1.2f:0.8f;
            for (float inset : {-0.02f,0.0f,0.02f}) for (float height : {-2.0f,0.0f,10.0f}) {
                Vec3 point=center;
                point.y+=height;
                if (axis==0) point.x+=side*(half-inset)/scale;
                else point.z+=side*(half-inset)/scale;
                const auto placed=Transform(point,model);
                const float displacement=axis==0?std::abs(placed.x-anchor.x):std::abs(placed.z-anchor.z);
                Require(std::abs(displacement-(half-inset))<0.00002f,
                        "Game boundary must match physical board dimensions");
                Require(Inside(planes,point,0.0001f)==(inset>=0),
                        "Clip half-space rejected inside or accepted outside city");
            }
        }
        Require(Inside(planes,center), "City centre must remain visible");
    }
}
void CheckBoardAndInvalidInput() {
    std::array<CeilingBaseVertex,4> board{};
    Require(TryMakeCeilingBaseQuad({0.5f,2.2f,-0.3f},2.4f,1.6f,board), "Board construction failed");
    Require(std::abs(board[1].x-board[0].x-2.4f)<0.00001f, "Board width incorrect");
    Require(std::abs(board[0].z-board[2].z-1.6f)<0.00001f, "Board depth incorrect");
    for (const auto& v : board) {
        Require(std::abs(v.y-2.21f)<0.00001f, "Board must sit 1 cm above city anchor");
        Require((v.color>>24)==255, "Board must be opaque");
        Require(v.color!=kCeilingChromaPink, "Board must not use passthrough key");
    }
    const auto previous=board;
    Require(!TryMakeCeilingBaseQuad({0,0,0},0,1,board), "Zero-sized board accepted");
    Require(board[0].x==previous[0].x && board[0].y==previous[0].y,
            "Invalid board modified output");
    CityClipPlanes planes{};
    Require(TryMakeCityClipPlanes({128,0,200},0.08f,2.4f,2.4f,planes), "Fixture failed");
    const auto previousPlanes=planes;
    for (float bad : {0.0f,-1.0f,std::numeric_limits<float>::quiet_NaN(),
                      std::numeric_limits<float>::infinity()}) {
        Require(!TryMakeCityClipPlanes({128,0,200},bad,2.4f,2.4f,planes), "Invalid scale accepted");
        Require(planes==previousPlanes, "Invalid clip inputs modified output");
    }
    Require(!TryMakeCityClipPlanes({0,0,0},std::numeric_limits<float>::min(),
                                   std::numeric_limits<float>::max(),2.4f,planes),
            "Overflowing clip extent accepted");
}
}
int main() {
    CheckPhysicalBounds();
    CheckBoardAndInvalidInput();
    std::cout << "Ceiling mask geometry passed: physical bounds, clip signs, board and invalid inputs. "
                 "No headset or passthrough measurement.\n";
}
