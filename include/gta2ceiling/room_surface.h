#pragma once
#include "gta2ceiling/types.h"
#include <vector>
#include <string>
#include <numeric>

namespace gta2ceiling {
// Physical ceiling vertices must already be located in the SAME runtime space
// and epoch as the eyes. This geometry helper does not align reference spaces.
struct RoomSurface {
    std::vector<Vec3> boundary;
    std::vector<uint32_t> triangles;
    Vec3 minimum,maximum,centre;
    float area=0;
};
namespace room_detail {
inline float Cross(const Vec3& a,const Vec3& b,const Vec3& c) {
    return (b.x-a.x)*(c.z-a.z)-(b.z-a.z)*(c.x-a.x);
}
inline bool OnSegment(const Vec3& a,const Vec3& b,const Vec3& p) {
    constexpr float e=1e-6f;
    return std::abs(Cross(a,b,p))<=e && p.x>=std::min(a.x,b.x)-e && p.x<=std::max(a.x,b.x)+e &&
           p.z>=std::min(a.z,b.z)-e && p.z<=std::max(a.z,b.z)+e;
}
inline bool Intersect(const Vec3& a,const Vec3& b,const Vec3& c,const Vec3& d) {
    const float abC=Cross(a,b,c),abD=Cross(a,b,d),cdA=Cross(c,d,a),cdB=Cross(c,d,b);
    return (abC*abD<0 && cdA*cdB<0) || OnSegment(a,b,c)||OnSegment(a,b,d)||OnSegment(c,d,a)||OnSegment(c,d,b);
}
}
// Conservative horizontal-ceiling scope: up to 8 cm height
// deviation, no fabricated rectangle, no replacement when the scan is invalid.
// Concave room outlines are triangulated; crossing contours fail closed.
inline bool BuildRoomSurface(std::vector<Vec3> points,RoomSurface& output,std::string& error) {
    error.clear();
    auto fail=[&](const char* why){error=why;return false;};
    if(points.size()<3 || points.size()>257) return fail("Ceiling polygon must contain 3 to 256 corners.");
    for(const auto& p:points)
        if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z)||
           std::abs(p.x)>10000 || std::abs(p.y)>10000 || std::abs(p.z)>10000)
            return fail("Invalid physical ceiling coordinate.");
    if((points.front()-points.back()).length()<0.001f) points.pop_back();
    if(points.size()<3 || points.size()>256) return fail("Invalid ceiling corner count.");
    for(size_t i=0;i<points.size();++i) for(size_t j=i+1;j<points.size();++j)
        if((points[i]-points[j]).length()<0.001f) return fail("Repeated ceiling corner.");
    for(size_t i=0;i<points.size();++i) {
        const size_t next=(i+1)%points.size();
        for(size_t j=i+1;j<points.size();++j) {
            const size_t after=(j+1)%points.size();
            if(j==next || after==i) continue;
            if(room_detail::Intersect(points[i],points[next],points[j],points[after]))
                return fail("Ceiling boundary intersects itself.");
        }
    }
    // Remove straight intermediate corners only after validating the contour.
    bool changed=true;
    while(changed && points.size()>3) {
        changed=false;
        for(size_t i=0;i<points.size();++i) {
            const auto& a=points[(i+points.size()-1)%points.size()];
            const auto& b=points[i]; const auto& c=points[(i+1)%points.size()];
            if(room_detail::OnSegment(a,c,b)) {points.erase(points.begin()+i);changed=true;break;}
        }
    }
    RoomSurface result; result.minimum=result.maximum=points[0];
    double signedArea=0,weightedX=0,weightedZ=0,height=0;
    for(size_t i=0;i<points.size();++i) {
        const auto& a=points[i];const auto& b=points[(i+1)%points.size()];
        const double cross=static_cast<double>(a.x)*b.z-static_cast<double>(b.x)*a.z;
        signedArea+=cross;weightedX+=(a.x+b.x)*cross;weightedZ+=(a.z+b.z)*cross;height+=a.y;
        result.minimum={std::min(result.minimum.x,a.x),std::min(result.minimum.y,a.y),std::min(result.minimum.z,a.z)};
        result.maximum={std::max(result.maximum.x,a.x),std::max(result.maximum.y,a.y),std::max(result.maximum.z,a.z)};
    }
    result.area=static_cast<float>(std::abs(signedArea)*0.5);
    if(result.area<0.25f || result.area>2500) return fail("Ceiling area is outside supported bounds.");
    if(result.maximum.y-result.minimum.y>0.08001f) return fail("Sloped ceilings are not supported yet.");
    result.centre={static_cast<float>(weightedX/(3*signedArea)),static_cast<float>(height/points.size()),
                   static_cast<float>(weightedZ/(3*signedArea))};
    // Keep the geometric origin within the room's bounds. The centre of a
    // concave polygon may lie outside the contour; it is NOT a standing point.
    std::vector<uint32_t> remaining(points.size());std::iota(remaining.begin(),remaining.end(),0u);
    const float sign=signedArea>0?1.0f:-1.0f;
    while(remaining.size()>3) {
        bool found=false;
        for(size_t i=0;i<remaining.size();++i) {
            const auto a=remaining[(i+remaining.size()-1)%remaining.size()],b=remaining[i],c=remaining[(i+1)%remaining.size()];
            if(sign*room_detail::Cross(points[a],points[b],points[c])<=1e-7f) continue;
            bool contains=false;
            for(auto p:remaining) if(p!=a&&p!=b&&p!=c) {
                if(sign*room_detail::Cross(points[a],points[b],points[p])>=-1e-7f &&
                   sign*room_detail::Cross(points[b],points[c],points[p])>=-1e-7f &&
                   sign*room_detail::Cross(points[c],points[a],points[p])>=-1e-7f) {contains=true;break;}
            }
            if(contains) continue;
            result.triangles.insert(result.triangles.end(),{a,b,c});
            remaining.erase(remaining.begin()+i);found=true;break;
        }
        if(!found) return fail("Ceiling polygon could not be triangulated safely.");
    }
    result.triangles.insert(result.triangles.end(),remaining.begin(),remaining.end());
    result.boundary=std::move(points); output=std::move(result);return true;
}
}
