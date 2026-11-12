// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <vector>

namespace gcc_host {
// Shared with the host's bounded XR-thread event queue. Keep all change times:
// the stock renderer remembers only the last pending recenter timestamp.
inline bool ReferenceChangeDue(const std::vector<int64_t>& changes,int64_t time) {
    for(const auto change:changes) if(time>=change) return true;
    return false;
}
inline unsigned ConsumeReferenceChanges(std::vector<int64_t>& changes,int64_t time) {
    unsigned count=0;
    auto next=changes.begin();
    while(next!=changes.end()) {
        if(time>=*next) {++count;next=changes.erase(next);}
        else ++next;
    }
    return count;
}

// XR-thread only. The stock renderer counter still drives STAGE recentering.
// This separate client counter also covers manual reference-space offsets.
// Publish once after poses/FrameSync have been captured; marking a later input
// change does not retroactively change the identity of those cached poses.
class ReferenceCounter {
public:
    void Reset(uint32_t runtimeCounter) {
        runtime_=runtimeCounter;published_=0;pending_=false;
    }
    void MarkCoordinatesChanged() { pending_=true; }
    uint32_t Publish(uint32_t runtimeCounter) {
        if(pending_ || runtimeCounter!=runtime_) {
            // HMD_RECENTER travels through a Java float[] in stock XrAPI 0.6.
            // Keep every emitted value exactly representable; consumers compare
            // identity/equality, not arithmetic ordering across a counter wrap.
            published_=(published_+1u)&0x00ffffffu;
        }
        runtime_=runtimeCounter;pending_=false;return published_;
    }
    uint32_t Value() const { return published_; }
private:
    uint32_t runtime_=0,published_=0;
    bool pending_=false;
};
}
