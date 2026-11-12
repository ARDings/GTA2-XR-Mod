#include <cmath>
#include <iostream>
#include <stdexcept>
#include "gta2ceiling/xr_transport.h"
using namespace gta2ceiling;
static void require(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}
static std::string packet(const std::string& pose="0.1 1.6 -0.2 0 0 0 1") {
    return "client0 0.25 -0.5 0 0 0.1 0.2 0.3 0.4 0.064 100 90 72 24 3 "
           "TFFFFFFFFFFFFFFFFFF 1 0 1 "+pose+" 3 0 0 0 0 0 0 FT";
}
int main() {
    try {
        XrTrackingFrame f;
        std::string error;
        require(XrTransport::parse_packet(packet(),f,error),error.c_str());
        require(f.pose_valid && f.hmd_sync==24 && f.recenter_count==3,"Host metadata was changed");
        require(std::abs(f.pose.position_y-1.6f)<0.00001f,"Position order is wrong");
        require(f.refresh_hz==72 && f.sbs && !f.immersive_screen,"Refresh/trailing flags wrong");
        require(f.controller_buttons[0] && !f.controller_buttons[18],"Controller boolean layout wrong");
        require(f.fov_is_symmetric_approximation,"0.6 aggregate FOV must be labelled approximate");
        require(std::abs(f.fov_left.angle_right-100*3.14159265f/360)<0.00001f,"FOV degrees/radians mismatch");

        std::string old=packet();
        old.erase(old.find("72 24"),3);
        require(XrTransport::parse_packet(old,f,error) && f.refresh_hz==0,"Documented packet without refresh not supported");
        require(XrTransport::parse_packet(packet("0 0 0 0 0 0 0"),f,error) && !f.pose_valid,
                "Unavailable zero quaternion must invalidate tracking");
        require(!XrTransport::parse_packet(packet("0 0 0 nan 0 0 1"),f,error),"Non-finite quaternion accepted");
        require(!XrTransport::parse_packet(packet().substr(0,60),f,error),"Truncated datagram accepted");
        std::string bad=packet(); bad.replace(bad.find("24 3"),4,"256 3");
        require(!XrTransport::parse_packet(bad,f,error),"Out-of-range host sync accepted");
        bad=packet(); bad.replace(bad.find("1 0 1 0.1"),5,"1 0 9");
        require(!XrTransport::parse_packet(bad,f,error),"Unrequested reference space accepted");
        require(!XrTransport::parse_packet(packet()+" junk",f,error),"Trailing malformed fields accepted");
        require(!XrTransport::parse_packet(std::string(8193,'0'),f,error),"Oversized datagram accepted");

        const std::string command="0 0 1 1 0 0 1 1 2 0 0 0 0 0 0 1 0 1 0 1";
        require(XrTransport::enable_command()==command,"Explicit LOCAL head locate command wrong");
        require(XrTransport::enable_command(true)==command,"Explicit projection mode command wrong");
        require(XrTransport::enable_command(false)=="0 0 2 1 0 0 1 1 2 0 0 0 0 0 0 1 0 1 0 1",
                "Diagnostic screen must retain UDP using MODE_VR=2 and SBS=1");
        require(XrTransport::enable_command(false,false)=="0 0 2 0 0 0 1 1 2 0 0 0 0 0 0 1 0 1 0 1",
                "Startup screen must retain LOCAL tracking while SBS stays off");
        for(unsigned sync=0;sync<=252;sync+=12) {
            const uint32_t argb=XrTransport::encode_sync_pixel(static_cast<uint8_t>(sync));
            require((argb>>24)==255 && ((argb>>16)&255)==sync && (argb&65535)==0,
                    "Pixel must be opaque ARGB with exact host-sync red, no fabricated flags");
        }
        XrTransport xr;
        require(xr.session_state()==XrSessionState::Unknown && !xr.is_focused(),
                "API 0.6 must not fabricate focus events");
        std::cout<<"XrAPI 0.6 parser/command/pixel tests passed; no Quest compatibility claim.\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
