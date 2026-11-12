#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib,"ws2_32.lib")
#endif
#include "gta2ceiling/xr_transport.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace gta2ceiling;
using Clock=std::chrono::steady_clock;
static void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
static std::string packet(unsigned sync,unsigned recenter=0,bool valid=true) {
    return "client0 0 0 0 0 0 0 0 0 0.064 100 90 72 "+std::to_string(sync)+" "+
        std::to_string(recenter)+" FFFFFFFFFFFFFFFFFFF 1 0 1 0 1.6 0 0 0 0 "+
        (valid?"1":"0")+" 0 0 0 0 0 0 0 FT";
}
#ifdef _WIN32
static bool receive(SOCKET s,std::string& message,int milliseconds) {
    fd_set set; FD_ZERO(&set); FD_SET(s,&set);
    timeval timeout{milliseconds/1000,(milliseconds%1000)*1000};
    if(select(0,&set,nullptr,nullptr,&timeout)<=0) return false;
    char buffer[2048]; const int n=recv(s,buffer,sizeof(buffer),0);
    if(n<=0) return false;
    message.assign(buffer,n); return true;
}
#endif
int main() {
#ifndef _WIN32
    std::cout<<"UDP transport requires Windows/Wine; this target is unsupported.\n"; return 77;
#else
    WSADATA wsa{};
    if(WSAStartup(MAKEWORD(2,2),&wsa)!=0) return 1;
    SOCKET server=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    try {
        require(server!=INVALID_SOCKET,"Cannot create loopback fixture socket");
        sockaddr_in server_address{};
        server_address.sin_family=AF_INET;
        server_address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        server_address.sin_port=0;
        require(bind(server,reinterpret_cast<sockaddr*>(&server_address),sizeof(server_address))==0,"Fixture bind failed");
        int length=sizeof(server_address);
        require(getsockname(server,reinterpret_cast<sockaddr*>(&server_address),&length)==0,"Fixture getsockname failed");
        XrTransport xr;
        XrTransportOptions options;
        options.receive_port=17872;
        options.control_port=ntohs(server_address.sin_port);
        options.write_version_file=false;
        options.projection_warmup=std::chrono::milliseconds(0);
        std::string error,command;
        require(xr.start(error,options),error.c_str());
        require(receive(server,command,1000) && command==XrTransport::enable_command(),
                "Actual UDP enable command missing or malformed");
        sockaddr_in destination{};
        destination.sin_family=AF_INET;
        destination.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        destination.sin_port=htons(options.receive_port);
        auto send=[&](const std::string& message) {
            require(sendto(server,message.data(),static_cast<int>(message.size()),0,
                           reinterpret_cast<sockaddr*>(&destination),sizeof(destination))==static_cast<int>(message.size()),
                    "Fixture UDP send failed");
        };
        auto wait_sample=[&](uint64_t previous) {
            XrTrackingFrame f;
            const auto deadline=Clock::now()+std::chrono::seconds(1);
            while(Clock::now()<deadline) {
                xr.latest_tracking(f);
                if(f.local_sample_id>previous) return f;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            throw std::runtime_error("Loopback tracking datagram not received");
        };
        send(packet(12));
        auto frame=wait_sample(0);
        require(frame.hmd_sync==12 && xr.has_valid_pose(),"Host sync/pose not delivered");
        require(xr.session_state()==XrSessionState::Unknown,"Transport fabricated lifecycle state");

        // Duplicate datagrams must not keep a frozen host frame alive.
        const auto until=Clock::now()+std::chrono::milliseconds(150);
        while(Clock::now()<until) {
            send(packet(12));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        require(!xr.has_valid_pose(),"Repeated old host-sync bypassed the stale guard");
        send(packet(24,1)); frame=wait_sample(frame.local_sample_id);
        require(xr.has_valid_pose() && frame.recenter_count==1,"New pose did not recover");
        require(xr.consume_recenter() && !xr.consume_recenter(),"Recenter event not delivered exactly once");

        send(packet(36,1,false)); frame=wait_sample(frame.local_sample_id);
        require(!xr.has_valid_pose() && !frame.pose_valid,"Invalid zero quaternion left tracking valid");
        send(packet(48,1)); frame=wait_sample(frame.local_sample_id);
        require(xr.has_valid_pose(),"Valid packet did not recover after invalid pose");
        xr.stop();
        require(!xr.running() && !xr.has_valid_pose(),"Stop retained running/valid state");
        bool disabled=false;
        const auto stop_deadline=Clock::now()+std::chrono::seconds(1);
        while(Clock::now()<stop_deadline && !disabled) {
            if(receive(server,command,50)) disabled=command==XrTransport::disable_command();
        }
        require(disabled,"UDP disable command missing");

        // Restarting with diagnostic screen mode must change the real UDP
        // command, not just a command-builder helper. MODE 0 would stop poses.
        options.immersive_projection=false;
        require(xr.start(error,options),error.c_str());
        require(receive(server,command,1000) && command==XrTransport::enable_command(false),
                "Actual diagnostic UDP command did not select MODE_VR=2");
        send(packet(60,1));
        frame=wait_sample(frame.local_sample_id);
        require(frame.hmd_sync==60 && xr.has_valid_pose(),"Diagnostic mode lost tracking receiver");
        xr.stop();
        require(receive(server,command,1000) && command==XrTransport::disable_command(),
                "Diagnostic mode did not send disable on stop");

        // The warmup must control the actual host, then transition to the
        // requested projection without restarting the pose receiver.
        options.immersive_projection=true;
        options.projection_warmup=std::chrono::milliseconds(50);
        require(xr.start(error,options),error.c_str());
        require(receive(server,command,1000) && command==XrTransport::enable_command(false,false),
                "Warmup must begin in MODE_VR=2/SBS=0");
        send(packet(72,1));
        frame=wait_sample(frame.local_sample_id);
        require(xr.has_valid_pose() && frame.hmd_sync==72,"Warmup lost live pose delivery");
        require(receive(server,command,1000) && command==XrTransport::enable_command(true,true),
                "Warmup did not transition to MODE_VR=1/SBS=1");
        send(packet(84,1));
        frame=wait_sample(frame.local_sample_id);
        require(xr.has_valid_pose() && frame.hmd_sync==84,"Warmup transition lost live tracking");
        xr.stop();
        require(receive(server,command,1000) && command==XrTransport::disable_command(),
                "Warmed projection did not disable on stop");

        closesocket(server); WSACleanup();
        std::cout<<"Windows loopback UDP integration, stale/recenter/invalid-pose/shutdown tests passed.\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<e.what()<<'\n';
        if(server!=INVALID_SOCKET) closesocket(server);
        WSACleanup(); return 1;
    }
#endif
}
