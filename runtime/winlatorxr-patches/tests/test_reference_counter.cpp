// SPDX-License-Identifier: GPL-3.0-or-later
// CPU state-machine test; no OpenXR session, native window or headset is used.
#include "../overlay/gcc_reference_counter.h"
#include <cassert>
#include <cstdio>

int main() {
    gcc_host::ReferenceCounter counter;
    counter.Reset(7);
    assert(counter.Publish(7)==0);
    // Input changes occur after getAxes()/room response in the current frame.
    // Neither channel may relabel those old cached poses retroactively.
    counter.MarkCoordinatesChanged();
    assert(counter.Value()==0);
    counter.MarkCoordinatesChanged();
    assert(counter.Value()==0);
    const auto roomCount=counter.Publish(7);
    const auto legacyPoseCount=counter.Value();
    assert(roomCount==1 && legacyPoseCount==roomCount);
    assert(counter.Publish(7)==1); // No repeated invalidation without a change.
    assert(counter.Publish(8)==2); // Runtime recenter.
    counter.MarkCoordinatesChanged(); // Private STAGE handle rebuild next frame.
    assert(counter.Value()==2);
    assert(counter.Publish(8)==3);
    counter.MarkCoordinatesChanged(); // Simultaneous changes share one identity.
    assert(counter.Publish(9)==4);
    assert(counter.Value()==4);
    counter.Reset(99);
    assert(counter.Publish(99)==0);
    {
        gcc_host::ReferenceCounter published;
        published.Reset(0);
        std::vector<int64_t> times{10,20};
        uint64_t epoch=1;
        uint32_t roomCounter=0;
        auto frame=[&](int64_t time,uint32_t stockCounter) {
            if(gcc_host::ReferenceChangeDue(times,time)) published.MarkCoordinatesChanged();
            const auto legacyCounter=published.Publish(stockCounter);
            if(roomCounter!=legacyCounter) ++epoch;
            roomCounter=legacyCounter;
            epoch+=gcc_host::ConsumeReferenceChanges(times,time);
            assert(roomCounter==published.Value());
        };
        frame(9,0); assert(roomCounter==0 && epoch==1 && times.size()==2);
        // The stock renderer has overwrittenT10 withT20 and still reports0.
        // The shared due-event helper nevertheless changes BOTH channels atT10.
        frame(10,0); assert(roomCounter==1 && epoch==3 && times.size()==1);
        frame(19,0); assert(roomCounter==1 && epoch==3);
        frame(20,1); assert(roomCounter==2 && epoch==5 && times.empty());
        frame(21,1); assert(roomCounter==2 && epoch==5);
    }
    // Entire float-exact domain and wrap; this is arithmetic, not a benchmark.
    for(uint32_t expected=1;expected<=0x01000000u;++expected) {
        counter.MarkCoordinatesChanged();
        const auto emitted=counter.Publish(99);
        assert(emitted==(expected&0x00ffffffu));
        assert(static_cast<uint32_t>(static_cast<float>(emitted))==emitted);
    }
    std::puts("PASS: reference counter frame publication, runtime/manual changes and float-exact wrap.");
}
