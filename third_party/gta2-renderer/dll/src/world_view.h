// The world-space view drawn in place of GTA2's own renderer.
//
// Reads the game's live map and camera, builds true 3D geometry, and draws it
// through D3D9 fixed-function so RTX Remix can path trace it. The game's own
// draw stream is ignored: it is pre-transformed screen space, which Remix skips.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <vector>

#include "../../src/camera.h"
#include "../../src/renderer.h"
#include "game_access.h"
#include "ground.h"
#include "live_geometry.h"
#include "walk_out.h"
#include "overlay.h"
#include "ceiling_bridge.h"
#include "gamepad_bridge.h"
#include "modern_controls.h"

namespace gta2dx9 {

// Whether gta2.exe is patched so its own view rectangle is the shape of the
// frame we draw, which keeps new cars and pedestrians out of a wide frame's
// margins. Off puts every byte of the game back the way it shipped - the sites
// keep their original bytes, so this is reversible while the game is running.
constexpr bool kDefaultSpawnOffscreen = true;
void SetSpawnOffscreen(bool on);
bool SpawnOffscreen();
// Whether the patches are in force right now, for the menu to show.
bool SpawnOffscreenActive();

// Whether pedestrians leave blood pools, which GTA2 has but keeps behind a debug
// flag. See game::kDoBloodFlag. Off hands the flag back to whatever the game had.
constexpr bool kDefaultBlood = true;
void SetBlood(bool on);
bool Blood();

// Where the renderer presents. GTA2 loads a *video* device alongside the render
// device, and that is what owns the screen; these are the ways of getting out
// from under it. own_window in gta2ceiling.ini selects one.
enum class PresentWindow {
    GameWindow = 0,  // share the game's, which deadlocks Remix's first Present
    Child = 1,       // a child of the game's window
    Topmost = 2,     // a separate top-level window in the topmost band
};

class WorldView {
public:
    // pitchDegrees: -90 looks straight down, matching GTA2's own view.
    // useGameTiles: take tile artwork from the running game rather than from our
    // own parse of the style file.
    // ownWindow: present into a window of our own rather than the game's, which
    // DirectDraw has already claimed.
    void Configure(float pitchDegrees, float fovDegrees, bool useGameTiles, PresentWindow present);

    // The size to actually render and present at, which is deliberately not the
    // game's. GTA2's own resolution is whatever its options screen last wrote -
    // 640x480 out of the box, and its manager will not offer much better because
    // the list comes from DirectDraw mode enumeration. That number has no
    // business deciding how big the path traced image is: it is the resolution
    // the HUD is laid out in, and the overlay already scales that to whatever it
    // is drawn into.
    //
    // 0 x 0 means the desktop resolution, which is what you want essentially
    // always. Anything else is taken literally.
    void SetRenderSize(int width, int height);

    bool Initialize(HWND window, int width, int height, std::string* error);
    void Shutdown();
    bool Ready() const { return ready_; }

    // GTA2 hands the renderer the visible tile rectangle every frame, which is
    // exactly its zoom and centre - no need to reverse a projection.
    void SetVisibleTileBounds(float minX, float minY, float maxX, float maxY);

    // Called when the game starts a level, so the next frame rebuilds.
    void InvalidateWorld() { loadedMapObject_ = nullptr; }

    // The game cleared its screen. gta2.exe!0x004619BC does this only when one
    // of its repaint flags is set, so it is the one statement that the 2D layer
    // may be thrown away.
    void NoteScreenClear() { renderer_.NoteScreenClear(); }

    void BeginFrame() {
        // The first scene after the intro starts on a clean 2D layer: the menu
        // only paints what it changes, and would otherwise leave the movie's last
        // frame showing through wherever it does not paint.
        if (movieShown_) {
            movieShown_ = false;
            overlay_.EndMovie();
            NoteScreenClear();
        }
        MatchGameViewToFrame();
        ApplyBlood();
        overlay_.BeginFrame();
        live_.BeginFrame();
    }
    void RenderFrame();

    // One frame of the intro movie, which the game plays without a scene of its
    // own: drawn over the screen and presented here. See intro_movie.cpp.
    void ShowMovieFrame(const void* pixels, int width, int height, int pitch) {
        overlay_.BeginFrame();
        overlay_.MovieFrame(pixels, width, height, pitch);
        movieShown_ = true;
        RenderFrame();
    }

    // The HUD and menus arrive as screen-space draws and are replayed on top of
    // the world rather than dropped.
    Overlay& GetOverlay() { return overlay_; }

    // Sprites and the block shapes the static mesh does not build, placed as
    // real 3D geometry from the world coordinates the game itself computed.
    LiveGeometry& Live() { return live_; }


    // Compares a tile the game just drew against what our own map walk says
    // belongs at that cell, to locate a placement mismatch.
    void CheckDrawnTile(const void* textureRecord);

    // Logs the world-space corners the game itself computed for a face of an
    // awkward block shape, so the mesh builder's idea of that shape can be
    // checked against the game rather than against a reading of its code.
    void CheckDrawnShape(int corners);

private:
    bool EnsureDevice();
    bool EnsureWorldLoaded();
    void RefreshAnimatedTiles();
    void UpdateCamera();
    bool RenderStereo(bool captureUi);
    void WaitForPlacement();
    void DumpCameraStruct() const;
    float GroundHeightAt(float tileX, float tileY) const;

    // Widen GTA2's own view rectangle to the frame's aspect. See the .cpp.
    void MatchGameViewToFrame();
    // Keep GTA2's blood flag set while the blood setting is on.
    void ApplyBlood();

    gta2::Renderer renderer_;
    Overlay overlay_;
    bool movieShown_ = false;
    LiveGeometry live_;
    gta2::Camera camera_;
    gta2::Vec3 cityCenter_{};
    gta2ceiling::CeilingBridge ceiling_;
    gta2ceiling::GamepadBridge gamepad_;
    uint64_t lastXrSample_ = 0;
    bool staleXrFramePresented_ = false;
    double xrRenderSeconds_ = 0.004;
    gta2::Map map_;
    // The floor the sprite pass stands sprites on, rebuilt with the world. It
    // reads map_ directly, so the two can never describe different ground.
    GroundSampler ground_;
    const void* loadedMapObject_ = nullptr;
    // Whether a level's geometry is currently uploaded. Distinct from
    // loadedMapObject_, which gbh_EndLevel clears to force a rebuild while the
    // old mesh is still sitting in the vertex buffer.
    bool worldUploaded_ = false;
    std::string dataDir_;

    float minX_ = 0.0f, minY_ = 0.0f, maxX_ = 0.0f, maxY_ = 0.0f;
    bool haveBounds_ = false;
    bool ready_ = false;
    int frameCount_ = 0;

    // Where the device is to be created, kept because creating it can take more
    // than one attempt; roughly ten seconds of frames before giving up.
    static constexpr int kMaxDeviceAttempts = 600;
    HWND window_ = nullptr;      // what we present into
    HWND gameWindow_ = nullptr;  // what the game owns, and its video device with it
    PresentWindow present_ = PresentWindow::Child;
    int width_ = 0;
    int height_ = 0;
    // 0 means "the desktop", resolved in Initialize.
    int requestedWidth_ = 0;
    int requestedHeight_ = 0;
    int deviceAttempts_ = 0;

    float smoothedHeight_ = 0.0f;
    bool heightSettled_ = false;
    // GTA2Ceiling: the street level is the lowest ground with a real share of
    // the play field around the camera (see the .cpp). Anything below it would
    // lie behind the physical floor or ceiling and vanish.
    float StreetLevel(float tileX, float tileY);
    // Ground levels that receive building shadows (refreshed per city tile).
    static constexpr int kMaxShadowLevels = 1;  // the street level only (GPU)
    float shadowLevels_[kMaxShadowLevels]{};
    int shadowLevelCount_ = 0, shadowLevelsX_ = -1, shadowLevelsY_ = -1, shadowLevelsRadius_ = 0;
    void UpdateShadowLevels();
    bool appliedHighEnd_ = false;
    uint64_t lastPlacementRenderMs_ = 0;
    // GTA2Ceiling "walk out" (frozen city on the round field, see walk_out.h).
    bool worldWasLocked_ = false;
    // Released: the city eases from where it was frozen back onto the player.
    static constexpr float kReleaseSeconds = 1.5f;
    uint64_t releaseStartMs_ = 0;
    gta2::Vec3 releaseFrom_{};
    // The player is drawn without the disc's clip (frozen, or easing back).
    bool WalkOutFree() const { return releaseStartMs_ != 0 || ceiling_.WorldLocked(); }
    // Once per game frame from UpdateCamera: flattens the game's map outside
    // the disc while frozen and puts it back afterwards.
    void UpdateWalkOut();
    // Easter egg: freezing the city puts a tank on the real floor under the
    // person wearing the headset, outside the disc, for the player to fetch
    // and drive back into the city. Once per freeze, at most once a minute,
    // and only through the game's own car creation (see game_access.h).
    void UpdateTankEgg(std::vector<walkout::Point>* around, bool afterTrack);
    bool tankTried_ = false, tankPending_ = false;
    float tankX_ = 0.0f, tankY_ = 0.0f;   // cells, x east, y south
    uint64_t tankDueMs_ = 0, tankGiveUpMs_ = 0, tankLastMs_ = 0, tankKeepFlatMs_ = 0;
    // The tank the game made for us, while it is still a live tank (cells).
    const void* tankCar_ = nullptr;
    bool TankPosition(float* x, float* y);
    void DropWalkOut(const char* why);
    // After the first eye's sprite pass: follows which sprite is the player.
    void UpdateWalkTrack();
    walkout::Flattening flat_;
    // Holo cut (X in the menu): closes buildings cut open by the round field's
    // edge with a holographic surface on the cutting plane. See the .cpp.
    void DrawHoloCut(IDirect3DDevice9* device, const gta2::Camera& eye, float aspect);
    // Where the holo cut runs: the field's circle, pulled in to the inradius of
    // the 48-gon its stencil mask is drawn with, so it never pokes outside it.
    float HoloCutRadius() const { return ceiling_.FieldRadiusTiles() * 0.99786f; }
    IDirect3DTexture9* holoTexture_ = nullptr;
    struct HoloVertex { float x, y, z, u, v; };
    std::vector<HoloVertex> holoVertices_;
    IDirect3DDevice9* holoDevice_ = nullptr;
    void* flatMemory_ = nullptr;  // our own flat column and block (see MakeOwnFlat)
    void FreeOwnFlat();
    const void* flatMapObject_ = nullptr;
    float flatRadius_ = 0.0f;
    bool flatFailed_ = false, flatRestorePending_ = false;
    int flatWaitFrames_ = 0;
    struct WalkTrack {
        bool have = false;
        float x = 0, y = 0, z = 0, vx = 0, vz = 0, area = 0;
        int misses = 0;
    } walkTrack_;
    // The game's own player object, found at run time: a pointer reachable
    // from the camera struct (directly or one step on) to a sprite or ped
    // object that, over seconds of play, always sits on a drawn sprite close
    // to the camera while it moves around. A ped's car comes from the
    // vehicle list (its driver). See ProbeFollow in the .cpp.
    struct FollowCandidate {
        int a = 0, b = -1, kind = 0;   // camera + a (-> + b); kind 0 sprite, 1 ped
        int hits = 0;
        float score = 0.5f, moved = 0.0f, lastX = 0.0f, lastY = 0.0f;
        bool haveLast = false, good = false, trusted = false;  // trusted: was good once
    };
    std::vector<FollowCandidate> follow_;
    uint64_t followProbeMs_ = 0;
    int followRounds_ = 0;
    void ProbeFollow(float cameraX, float cameraY);
    // The player's exact position this game frame (cells, x east, y south).
    // `face`: where it faces on foot (two floats, both 0 when unreadable).
    bool FollowedPlayer(float* x, float* y, bool* inCar, float* face = nullptr);
    // Modern controls (RT in the menu): the pad sample rewritten for the
    // classic key mapping, from where the player is, faces and is seen.
    void ModernRemap(gta2ceiling::GamepadState& state);
    void RememberFollow(const FollowCandidate& found);
    void RecallFollow();
    gta2ceiling::ModernControls modern_;
    gta2::Mat4 stickView_{}, stickProjection_{};
    bool stickViewValid_ = false, modernApplied_ = false;
    bool walkExact_ = false;
    float walkCameraDistanceMax_ = 0.0f;
    uint64_t walkLogMs_ = 0;
    // Pacing diagnostics, logged once per second (gta2dx9.log "pacing").
    struct Pacing {
        uint64_t windowStart = 0;
        int gameFrames = 0, poseRefreshes = 0, skippedStill = 0, skippedSameSample = 0, untracked = 0;
        double renderSum = 0, renderMax = 0, ageSum = 0, ageMax = 0, gapMax = 0, turnDegrees = 0;
        double lastPresentMs = 0;
        float lastYaw = 0, lastPitch = 0;
        bool haveAngles = false;
    } pacing_;
    void NotePacing(bool rendered, bool captureUi, bool skippedStill, bool sameSample, bool tracked);
    float loggedStreet_ = -2.0f;
    float visibleTiles_ = 0.0f;
    bool useGameTiles_ = true;
    int tileChecks_ = 0;
    int shapeChecks_ = 0;

    // What the artwork for each tile was last taken from, so an animated tile
    // can be spotted and re-uploaded.
    struct TileSource {
        const void* record = nullptr;
        uint16_t revision = 0;
    };
    std::vector<TileSource> tileSources_;
};

}  // namespace gta2dx9
