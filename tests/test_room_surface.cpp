#include "gta2ceiling/room_surface.h"
#include <cassert>
#include <iostream>
#include <limits>
using namespace gta2ceiling;
int main() {
    RoomSurface room; std::string error;
    const std::vector<Vec3> lShape{{0,2.4f,0},{4,2.4f,0},{4,2.4f,1},{1,2.4f,1},{1,2.4f,4},{0,2.4f,4}};
    for(bool reverse:{false,true}) {
        auto points=lShape;if(reverse) std::reverse(points.begin(),points.end());
        assert(BuildRoomSurface(points,room,error));assert(error.empty());
        assert(room.triangles.size()==12);assert(std::abs(room.area-7)<1e-5);
        float triangulatedArea=0;
        for(size_t i=0;i<room.triangles.size();i+=3) {
            const auto& a=room.boundary[room.triangles[i]];
            const auto& b=room.boundary[room.triangles[i+1]];
            const auto& c=room.boundary[room.triangles[i+2]];
            triangulatedArea+=std::abs(room_detail::Cross(a,b,c))/2;
            const Vec3 centre=(a+b+c)*(1.0f/3);
            assert(centre.x<=1 || centre.z<=1); // No triangle covers the missing corner.
        }
        assert(std::abs(triangulatedArea-7)<1e-5);
    }
    auto closed=lShape;closed.push_back(closed.front());assert(BuildRoomSurface(closed,room,error));
    assert(BuildRoomSurface({{0,2,0},{2,2,0},{4,2,0},{4,2,4},{0,2,4}},room,error));
    assert(room.boundary.size()==4 && std::abs(room.area-16)<1e-5);
    const auto good=room.boundary;
    for(auto invalid:std::vector<std::vector<Vec3>>{
        {},{{0,2,0},{1,2,0}},{{0,2,0},{4,2,4},{0,2,4},{4,2,0}},
        {{0,2,0},{4,2,0},{4,2,4},{0,2,0},{0,2,4}},
        {{0,2,0},{4,2.2f,0},{4,2,4},{0,2,4}},
        {{0,2,0},{.1f,2,0},{.1f,2,.1f},{0,2,.1f}},
        {{0,2,0},{std::numeric_limits<float>::quiet_NaN(),2,0},{4,2,4}}}) {
        assert(!BuildRoomSurface(invalid,room,error));assert(!error.empty());assert(room.boundary.size()==good.size());
        assert(room.boundary[0].x==good[0].x && room.area==16);
    }
    std::cout<<"Physical ceiling polygon validation and concave triangulation passed; synthetic geometry, no room-scan device claim.\n";
}
