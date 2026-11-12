#include "../src/mod/placement_controls.h"

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace gta2ceiling;
namespace {
constexpr uint16_t Chord = 0x00c0, A = 0x1000, B = 0x2000;
bool Near(float a, float b, float epsilon = 0.00001f) { return std::abs(a-b) < epsilon; }
bool Same(const PlacementSettings& a, const PlacementSettings& b, float epsilon = 0.00001f) {
    return Near(a.distance_m,b.distance_m,epsilon) && Near(a.width_m,b.width_m,epsilon) &&
        Near(a.depth_m,b.depth_m,epsilon) && Near(a.meters_per_tile,b.meters_per_tile,epsilon) &&
        Near(a.yaw_radians,b.yaw_radians,epsilon) && Near(a.offset_x_m,b.offset_x_m,epsilon) &&
        Near(a.offset_z_m,b.offset_z_m,epsilon);
}
struct FakeStore final : PlacementPersistence {
    bool fail = false, present = false;
    int saves = 0;
    PlacementSettings saved;
    bool Save(const PlacementSettings& value, std::string& error) override {
        ++saves;
        if (fail) { error = "Injected persistence failure"; return false; }
        error.clear(); saved = value; present = true; return true;
    }
    bool Load(PlacementSettings& value, std::string& error) override {
        error.clear();
        if (!present) return false;
        value = saved; return true;
    }
};
void Open(PlacementControls& control, uint16_t extra_buttons = 0) {
    GamepadState sample{};
    sample.buttons = Chord | extra_buttons;
    assert(control.ConsumeInput(sample, 0.01));
    assert(control.Active() && control.TakeEvent() == PlacementEvent::Opened);
}
void Button(PlacementControls& control, uint16_t mask) {
    assert(control.ConsumeInput({}, 0.01));
    GamepadState sample{}; sample.buttons = mask;
    assert(control.ConsumeInput(sample, 0.01));
}

void TestPreviewConfirmCancelAndRelease() {
    FakeStore store;
    PlacementControls control({}, store);
    assert(!control.ConsumeInput({}, 0.01));
    Open(control, A); // A was already held as the two-stick chord opened.
    GamepadState sample{}; sample.buttons = A;
    assert(control.ConsumeInput(sample, 0.01));
    assert(control.Active() && store.saves == 0);
    sample = {}; sample.left_x = 32767;
    assert(control.ConsumeInput(sample, 0.2));
    assert(Near(control.GetSettings().offset_x_m, 0.1f));
    assert(Near(control.CommittedSettings().offset_x_m, 0));
    Button(control, A);
    assert(!control.Active() && store.saves == 1);
    assert(control.TakeEvent() == PlacementEvent::Committed);
    assert(control.TakeEvent() == PlacementEvent::None);
    assert(Near(store.saved.offset_x_m, 0.1f));
    sample = {}; sample.buttons = A;
    for (int i=0;i<20;++i) assert(control.ConsumeInput(sample, 0.01));
    assert(store.saves == 1); // Do not save repeatedly or leak Return to GTA2.
    assert(control.ConsumeInput({}, 0.01)); // Own the release too.
    assert(!control.ConsumeInput({}, 0.01));
    Open(control);
    sample = {}; sample.right_y = 32767;
    control.ConsumeInput(sample, 0.2);
    assert(control.GetSettings().distance_m > control.CommittedSettings().distance_m);
    Button(control, A | B); // Cancel wins an ambiguous simultaneous confirmation.
    assert(!control.Active() && store.saves == 1);
    assert(control.TakeEvent() == PlacementEvent::Cancelled);
    assert(Same(control.GetSettings(), store.saved));
}

void TestMotionTimeAndDiscreteButtons() {
    FakeStore one, two;
    PlacementControls slow({}, one), fast({}, two);
    Open(slow); Open(fast);
    GamepadState sample{};
    sample.left_x=32767; sample.left_y=32767; sample.right_x=32767; sample.right_y=32767;
    for(int i=0;i<12;++i) slow.ConsumeInput(sample,0.1);
    for(int i=0;i<120;++i) fast.ConsumeInput(sample,0.01);
    assert(Same(slow.GetSettings(),fast.GetSettings()));
    const auto moved=slow.GetSettings();
    assert(Near(std::hypot(moved.offset_x_m,moved.offset_z_m),0.6f));
    assert(moved.offset_x_m>0 && moved.offset_z_m<0);
    assert(Near(moved.distance_m,2.02f));
    assert(Near(moved.yaw_radians,-0.9424778f));
    slow.ConsumeInput(sample,-1);
    slow.ConsumeInput(sample,std::numeric_limits<double>::quiet_NaN());
    slow.ConsumeInput(sample,std::numeric_limits<double>::infinity());
    assert(Same(moved,slow.GetSettings()));
    sample={}; sample.left_x=32767;
    slow.ConsumeInput(sample,600); // A debugger/stall cannot shift metres at once.
    assert(Near(slow.GetSettings().offset_x_m-moved.offset_x_m,0.125f));
    const auto before=slow.GetSettings();
    sample={}; sample.left_x=7849; sample.left_y=-7849; sample.right_x=-7849; sample.right_y=7849;
    slow.ConsumeInput(sample,0.2);
    assert(Same(before,slow.GetSettings()));
    Button(slow,0x0008|0x0001|0x0200); // Wider, deeper, larger city once.
    const auto changed=slow.GetSettings();
    assert(Near(changed.width_m,before.width_m+0.1f));
    assert(Near(changed.depth_m,before.depth_m+0.1f));
    assert(Near(changed.meters_per_tile,before.meters_per_tile+0.005f));
    sample={}; sample.buttons=0x0008|0x0001|0x0200;
    for(int i=0;i<100;++i) slow.ConsumeInput(sample,0.01);
    assert(Same(changed,slow.GetSettings()));
    Button(slow,0x0008|0x0001|0x0200);
    assert(Near(slow.GetSettings().width_m,changed.width_m+0.1f));
    const auto opposite=slow.GetSettings();
    Button(slow,0x000f|0x0300);
    assert(Same(opposite,slow.GetSettings()));
    Button(slow,0x0004|0x0100);
    const auto staggered=slow.GetSettings();
    sample={}; sample.buttons=0x0004|0x0008|0x0100|0x0200;
    slow.ConsumeInput(sample,0.01);
    assert(Same(staggered,slow.GetSettings())); // Opposite held controls cancel too.
}

void TestBoundsAndPersistenceFailure() {
    PlacementSettings invalid;
    invalid.distance_m=-100; invalid.width_m=100; invalid.depth_m=0;
    invalid.meters_per_tile=999; invalid.offset_x_m=100; invalid.offset_z_m=-100;
    invalid.yaw_radians=std::numeric_limits<float>::infinity();
    FakeStore store;
    PlacementControls control(invalid,store);
    const auto start=control.GetSettings();
    assert(Near(start.distance_m,0.7f) && Near(start.width_m,10) && Near(start.depth_m,0.5f));
    assert(Near(start.meters_per_tile,0.3f) && Near(start.offset_x_m,5) && Near(start.offset_z_m,-5));
    assert(start.yaw_radians==0);
    Open(control);
    GamepadState sample{}; sample.left_x=32767; sample.left_y=32767; sample.right_y=-32768;
    control.ConsumeInput(sample,0.25);
    assert(Same(control.GetSettings(),start));
    Button(control,0x0008|0x0002|0x0200);
    assert(Same(control.GetSettings(),start));
    sample={}; sample.right_x=-32768;
    for(int i=0;i<500;++i) control.ConsumeInput(sample,0.25);
    assert(std::abs(control.GetSettings().yaw_radians)<=3.141593f);
    store.fail=true;
    Button(control,A);
    assert(control.Active() && store.saves==1 && !control.LastError().empty());
    assert(control.TakeEvent()==PlacementEvent::SaveFailed);
    assert(Same(control.CommittedSettings(),start));
    sample={}; sample.buttons=A;
    control.ConsumeInput(sample,0.1);
    assert(store.saves==1); // Retry requires release and a fresh A press.
    Button(control,B);
    assert(!control.Active() && Same(control.GetSettings(),start) && store.saves==1);
    invalid={}; invalid.distance_m=std::numeric_limits<float>::quiet_NaN();
    assert(Near(ClampPlacement(invalid).distance_m,1.6f));
}

void TestLongChordOnlyOpens() {
    FakeStore store;
    PlacementControls control({}, store);
    GamepadState sample{}; sample.buttons = Chord;
    assert(control.ConsumeInput(sample, 0.01));
    assert(control.Active() && control.TakeEvent() == PlacementEvent::Opened);
    for (int i = 0; i < 10; ++i) assert(control.ConsumeInput(sample, 0.25));
    // Holding the sticks longer only keeps the menu open; nothing else happens.
    assert(control.Active() && control.TakeEvent() == PlacementEvent::None && store.saves == 0);
    assert(control.ConsumeInput({}, 0.01) && control.Active());
}

struct Input final : GamepadInput {
    GamepadState sample;
    bool foreground=true, connected=true, lose_focus_on_read=false;
    bool IsForeground() const override { return foreground; }
    bool Read(GamepadState& output) override {
        output=sample;
        if(lose_focus_on_read) foreground=false;
        return connected;
    }
};
struct Sink final : KeyboardSink {
    std::vector<KeyboardEvent> events;
    bool Send(const KeyboardEvent& event) override { events.push_back(event); return true; }
};
struct FilterContext {
    PlacementControls& placement;
    GamepadState last;
    static bool Filter(const GamepadState& state,void* opaque) {
        auto& self=*static_cast<FilterContext*>(opaque);
        self.last=state;
        return self.placement.ConsumeInput(state,0.1);
    }
};
void TestBridgeModalFocusAndDisconnect() {
    FakeStore store; PlacementControls control({},store);
    Input input; Sink sink; FilterContext context{control,{}};
    GamepadBridge bridge(input,sink);
    bridge.SetFilter(FilterContext::Filter,&context);
    input.sample.buttons=A;
    bridge.Tick();
    assert(bridge.PendingKeys()==1 && sink.events.size()==1 && sink.events[0].down);
    input.sample.buttons=Chord;
    bridge.Tick();
    assert(control.Active() && bridge.PendingKeys()==0);
    assert(sink.events.size()==2 && !sink.events.back().down);
    input.sample={}; input.sample.right_y=32767; input.sample.right_x=12345;
    bridge.Tick();
    assert(control.GetSettings().distance_m>1.6f);
    assert(context.last.right_x==12345 && context.last.right_y==32767);
    input.connected=false;
    bridge.Tick();
    assert(!context.last.available && context.last.buttons==0 && context.last.right_y==0);
    assert(!control.Active() && Near(control.GetSettings().distance_m,1.6f) && store.saves==0);
    input.connected=true; input.sample={}; input.sample.buttons=A|Chord;
    bridge.Tick();
    assert(!control.Active() && bridge.PendingKeys()==0 && store.saves==0);
    input.sample={}; bridge.Tick();
    input.sample.buttons=Chord; bridge.Tick();
    assert(control.Active());
    input.sample={}; input.sample.buttons=A;
    input.lose_focus_on_read=true;
    bridge.Tick();
    assert(!control.Active() && !context.last.available && store.saves==0);
    input.foreground=true; input.lose_focus_on_read=false;
    bridge.Tick(); // A still held after foreground returns must not hit GTA2.
    assert(bridge.PendingKeys()==0 && sink.events.size()==2);
    input.sample={}; bridge.Tick(); bridge.Tick();
    input.sample.buttons=A; bridge.Tick();
    assert(bridge.PendingKeys()==1);
    assert(bridge.Shutdown() && !context.last.available && bridge.PendingKeys()==0);
    bridge.SetFilter(nullptr); // Make the lifetime rule explicit.
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream stream(path,std::ios::binary);
    assert(stream);
    return {std::istreambuf_iterator<char>(stream),{}};
}
void WriteFileText(const std::filesystem::path& path,const std::string& text) {
    std::ofstream stream(path,std::ios::binary|std::ios::trunc);
    assert(stream); stream<<text; stream.close(); assert(stream);
}
void TestRealFileStoreAtomicFailureAndProtectedFiles() {
#ifdef _WIN32
    const wchar_t* workRoot=_wgetenv(L"GTA2CEILING_WORK_ROOT");
    const std::filesystem::path parent=std::filesystem::path(workRoot&&*workRoot?workRoot:L"D:/GTA2Ceiling-work")/L"test-artifacts";
    std::filesystem::create_directories(parent);
    const auto root=parent/(L"placement-"+std::to_wstring(GetCurrentProcessId()));
    assert(!std::filesystem::exists(root));
    assert(std::filesystem::create_directory(root));
    const auto target=root/L"gta2ceiling_user.ini";
    {
        PlacementFileStore fresh(root);
        PlacementControls control({},fresh);
        assert(!control.LoadSaved() && control.LastError().empty());
        Open(control);
        Button(control,0x0008);
        assert(!control.LoadSaved() && control.Active());
        Button(control,B);
        assert(!std::filesystem::exists(target) && std::filesystem::is_empty(root));
    }
    WriteFileText(root/L"gta2ceiling.ini","original configuration sentinel");
    std::filesystem::create_directory(root/L"player");
    WriteFileText(root/L"player/save.dat","synthetic player sentinel");
    PlacementFileStore store(root);
    PlacementSettings settings; settings.offset_x_m=1.234f; settings.offset_z_m=-0.456f; settings.yaw_radians=1.123f;
    PlacementSettings loaded; std::string error;
    assert(!store.Load(loaded,error) && error.empty() && Same(loaded,PlacementSettings{}));
    assert(store.Save(settings,error) && error.empty());
    assert(store.Load(loaded,error) && Same(loaded,settings,0.00051f));
    PlacementControls restored({},store);
    assert(restored.LoadSaved() && Same(restored.GetSettings(),settings,0.00051f));
    const auto original=ReadFile(target);
    HANDLE lock=CreateFileW(target.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    assert(lock!=INVALID_HANDLE_VALUE);
    settings.width_m=3.1f;
    assert(!store.Save(settings,error) && !error.empty());
    CloseHandle(lock);
    assert(ReadFile(target)==original); // Failed MoveFileEx kept the prior bytes.
    for(const auto& entry:std::filesystem::directory_iterator(root))
        assert(entry.path().filename().wstring().find(L".tmp.")==std::wstring::npos);
    assert(store.Save(settings,error));
    assert(store.Load(loaded,error) && Near(loaded.width_m,3.1f));
    settings.distance_m=std::numeric_limits<float>::quiet_NaN();
    const auto valid=ReadFile(target);
    assert(!store.Save(settings,error) && ReadFile(target)==valid);
    for(const auto& malformed: {valid+"width_millimeters=999\n",
        std::string("[ceiling]\ndistance_millimeters=nan\n"),
        std::string(5000,'x')}) {
        WriteFileText(target,malformed);
        const auto before=loaded;
        assert(!store.Load(loaded,error) && !error.empty() && Same(loaded,before));
    }
    WriteFileText(target,"[ceiling]\nformat_version=1\ndistance_millimeters=1600\nwidth_millimeters=2400\n"
        "depth_millimeters=2400\nscale_millionths=80000\nyaw_milliradians=0\n"
        "offset_x_millimeters=0\noffset_z_millimeters=0\n");
    assert(store.Load(loaded,error) && Near(loaded.meters_per_tile,0.12f)); // Older small city enlarged once.
    WriteFileText(target,valid);
    {
        PlacementControls editing({},store);
        Open(editing);
        GamepadState sample{}; sample.left_x=32767;
        editing.ConsumeInput(sample,0.2);
        Button(editing,B);
        assert(ReadFile(target)==valid); // Cancel did not touch the real file.
    }
    assert(ReadFile(root/L"gta2ceiling.ini")=="original configuration sentinel");
    assert(ReadFile(root/L"player/save.dat")=="synthetic player sentinel");
    PlacementFileStore escaped(root/L".."/L"elsewhere");
    assert(!escaped.Save({},error));
    assert(!std::filesystem::exists(parent/L"elsewhere"));
    std::filesystem::remove(target);
    std::filesystem::create_directory(target);
    assert(!store.Save({},error) && std::filesystem::is_directory(target));
    // Resolve and verify the generated D: child before recursive fixture cleanup.
    assert(std::filesystem::canonical(root).parent_path()==std::filesystem::canonical(parent));
    assert(root.filename().wstring().starts_with(L"placement-"));
    std::filesystem::remove_all(root);
#endif
}
} // namespace

int main() {
    TestPreviewConfirmCancelAndRelease();
    TestMotionTimeAndDiscreteButtons();
    TestBoundsAndPersistenceFailure();
    TestBridgeModalFocusAndDisconnect();
    TestLongChordOnlyOpens();
    TestRealFileStoreAtomicFailureAndProtectedFiles();
    std::cout<<"Placement state, filter and atomic-file tests passed; no headset acceptance.\n";
}
