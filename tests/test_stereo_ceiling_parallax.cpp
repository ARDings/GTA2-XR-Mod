#include <cmath>
#include <iostream>
#include <stdexcept>
#include "gta2ceiling/xr_transport.h"
using namespace gta2ceiling;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static Vec4 transform(const Vec4& v, const Mat4& m) {
    return {v.x*m.m[0]+v.y*m.m[4]+v.z*m.m[8]+v.w*m.m[12],
            v.x*m.m[1]+v.y*m.m[5]+v.z*m.m[9]+v.w*m.m[13],
            v.x*m.m[2]+v.y*m.m[6]+v.z*m.m[10]+v.w*m.m[14],
            v.x*m.m[3]+v.y*m.m[7]+v.z*m.m[11]+v.w*m.m[15]};
}
static Vec3 ndc(const Vec4& p, const Mat4& v, const Mat4& projection) {
    const auto clip = transform(transform(p,v),projection);
    require(clip.w > 0, "A point in front of the eyes must have positive clip W");
    return {clip.x/clip.w, clip.y/clip.w, clip.z/clip.w};
}
static bool near(float a, float b, float eps=0.00001f) { return std::abs(a-b)<eps; }
int main() {
    try {
        XrTrackingFrame f;
        f.pose_valid = true; f.ipd_meters = 0.064f;
        Mat4 vl, vr, pl, pr;
        XrTransport::compute_eye_transforms(f, Eye::Left, vl, pl);
        XrTransport::compute_eye_transforms(f, Eye::Right, vr, pr);
        for (int row=0;row<3;++row) for(int col=0;col<3;++col)
            require(near(vl.m[row*4+col],vr.m[row*4+col]), "Eye rotation differs: toe-in is forbidden");
        auto l=ndc({0,0,-2,1},vl,pl), r=ndc({0,0,-2,1},vr,pr);
        require(l.x>0 && r.x<0 && near(l.y,r.y), "Stereo disparity/eye order is wrong");
        require(near(l.x-r.x,0.032f), "Measured geometric disparity does not match IPD/depth");
        require(near(ndc({-0.032f,0,-0.05f,1},vl,pl).z,0), "D3D near plane must map to zero");
        require(near(ndc({-0.032f,0,-100,1},vl,pl).z,1), "D3D far plane must map to one");
        f.pose.position_x=0.10f;
        Mat4 shifted, ps;
        XrTransport::compute_eye_transforms(f,Eye::Left,shifted,ps);
        auto shifted_point=ndc({0,0,-2,1},shifted,ps);
        require(near(shifted_point.x-l.x,-0.05f), "World-fixed point failed 10cm head-translation test");

        const float q=std::sqrt(0.5f);
        f.pose = {0,q,0,q,0,0,0}; // yaw +90: look toward world -X
        XrTransport::compute_eye_transforms(f,Eye::Left,vl,pl);
        XrTransport::compute_eye_transforms(f,Eye::Right,vr,pr);
        l=ndc({-2,0,0,1},vl,pl); r=ndc({-2,0,0,1},vr,pr);
        require(near((l.x+r.x)*0.5f,0) && near(l.x-r.x,0.032f), "Head yaw or rotated eye baseline ignored");

        f.pose = {0,0,q,q,0,0,0}; // roll +90: baseline rotates toward world +Y
        XrTransport::compute_eye_transforms(f,Eye::Left,vl,pl);
        XrTransport::compute_eye_transforms(f,Eye::Right,vr,pr);
        l=ndc({0,0,-2,1},vl,pl); r=ndc({0,0,-2,1},vr,pr);
        require(near(vl.m[0],0) && near(vl.m[4],1), "Head roll did not rotate the eye basis");
        require(near(l.y,r.y) && near(l.x-r.x,0.032f), "Head roll introduced vertical disparity");

        f.pose={}; f.fov_left={-0.60f,0.90f,0.80f,-0.70f};
        XrTransport::compute_eye_transforms(f,Eye::Left,vl,pl);
        const float eye_x=-f.ipd_meters*0.5f;
        require(near(ndc({eye_x+std::tan(-0.60f)*2,0,-2,1},vl,pl).x,-1), "Asymmetric left frustum edge wrong");
        require(near(ndc({eye_x+std::tan(0.90f)*2,0,-2,1},vl,pl).x,1), "Asymmetric right frustum edge wrong");
        require(near(ndc({eye_x,std::tan(0.80f)*2,-2,1},vl,pl).y,1), "Asymmetric upper frustum edge wrong");
        require(near(ndc({eye_x,std::tan(-0.70f)*2,-2,1},vl,pl).y,-1), "Asymmetric lower frustum edge wrong");
        std::cout << "Stereo matrix geometry tests passed (CPU mathematics; no headset/rendering claim).\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}

