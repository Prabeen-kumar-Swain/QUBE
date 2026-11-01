// QUBE - isometric 3D waiting-room simulation (raylib).
// Rendering reads the engine; all scheduling logic lives in src/engine (user actions call its public API).
//
// Settings window (top bar / key S): arrivals, scheduling and counters; saved to qube_settings.txt.
// Three priority lanes (P3 | P2 | P1), counters with assigned classes (click the badge to change),
// customers never re-sort - a free counter picks a lane front via the engine's selection rule.
// Keys:  SPACE start/pause/resume | A add | B rush | UP/DOWN speed | 1-4 policy | TAB panel tab | G auto | R reset | F fullscreen

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <fstream>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/Engine.hpp"

namespace {

// ============================================================ layout (world units ~ metres)
namespace lay {
constexpr float kFloorX0 = -12.f, kFloorX1 = 12.f, kFloorZ0 = -8.f, kFloorZ1 = 10.f;
constexpr float kDeskZ = -5.3f, kStaffZ = -6.5f, kStandZ = -3.7f, kLaneZ = -1.5f;
// Three lanes, index = priority class - 1 (P1 wide because it feeds two counters).
constexpr int kLaneRows = 6;
constexpr float kRowDz = 1.6f, kLaneZ0 = 0.8f;
constexpr int kLaneCols[3] = {4, 2, 2};
constexpr float kLaneX[3][4] = {{2.4f, 4.3f, 6.2f, 8.1f}, {-4.0f, -2.0f, 0.f, 0.f}, {-9.0f, -7.0f, 0.f, 0.f}};
constexpr float kEntryZ = 10.8f; // entrance corridor: customers walk along it, then up their lane
constexpr float kCounterSpacing = 5.2f;
constexpr float kTopH = 46.f, kPanelW = 330.f, kDockH = 84.f;
const Vector3 kSpawn{0.f, 0.f, 12.6f};
const Vector3 kExit{13.5f, 0.f, kLaneZ};
} // namespace lay

float CounterSpacing(int n) { return std::min(lay::kCounterSpacing, 22.f / static_cast<float>(std::max(1, n))); }
float CounterX(int i, int n) { return (static_cast<float>(i) - (n - 1) * 0.5f) * CounterSpacing(n); }

Vector3 SlotPos(int cls, int rank) {
    const int ci = std::clamp(cls, 1, 3) - 1;
    const int cols = lay::kLaneCols[ci];
    const int row = std::min(rank / cols, lay::kLaneRows - 1);
    return Vector3{lay::kLaneX[ci][rank % cols], 0.f, lay::kLaneZ0 + row * lay::kRowDz};
}
int LaneCapacity(int cls) { return lay::kLaneCols[cls - 1] * lay::kLaneRows; }
float LaneCenterX(int cls) {
    const int ci = cls - 1;
    return (lay::kLaneX[ci][0] + lay::kLaneX[ci][lay::kLaneCols[ci] - 1]) * 0.5f;
}
float LaneHalfW(int cls) {
    const int ci = cls - 1;
    return (lay::kLaneX[ci][lay::kLaneCols[ci] - 1] - lay::kLaneX[ci][0]) * 0.5f + 1.3f;
}
int NextClass(int c) { return c % 3 + 1; } // P1 -> P2 -> P3 -> P1

// ============================================================ colour helpers
Color Rgb(int r, int g, int b, int a = 255) {
    return Color{(unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a};
}

Color Mix(Color a, Color b, float t) {
    return Rgb((int)(a.r + (b.r - a.r) * t), (int)(a.g + (b.g - a.g) * t), (int)(a.b + (b.b - a.b) * t), 255);
}

// Wait-time colour coding (simulation seconds). Stage 3 will tie the last band to starvation.
constexpr double kWaitGreen = 10.0, kWaitYellow = 20.0, kWaitOrange = 30.0;
Color WaitColor(double w) {
    if (w < kWaitGreen) return Rgb(80, 200, 120);
    if (w < kWaitYellow) return Rgb(240, 200, 60);
    if (w < kWaitOrange) return Rgb(240, 140, 50);
    return Rgb(235, 70, 70);
}

Color PriorityColor(int p) {
    if (p >= 3) return Rgb(255, 98, 82);
    if (p == 2) return Rgb(240, 176, 52);
    return Rgb(140, 164, 214);
}

const char* PolicyName(qube::Policy p) {
    switch (p) {
        case qube::Policy::FCFS: return "FCFS";
        case qube::Policy::Priority: return "PRIORITY";
        case qube::Policy::SJF: return "SJF";
        default: return "AGING";
    }
}
const char* PolicyDesc(qube::Policy p) {
    switch (p) {
        case qube::Policy::FCFS: return "oldest first, ignores lanes";
        case qube::Policy::Priority: return "home lane, borrow by class";
        case qube::Policy::SJF: return "shortest job, ignores lanes";
        default: return "home lane + aging override";
    }
}

Color CounterColor(qube::CounterPhase p) {
    switch (p) {
        case qube::CounterPhase::Idle: return Rgb(70, 200, 120);
        case qube::CounterPhase::Approaching: return Rgb(240, 190, 60);
        default: return Rgb(235, 85, 85);
    }
}

// ============================================================ fonts / text
Font gFont, gBold;
bool gDefaultFont = true;

void LoadFonts() {
    gFont = GetFontDefault();
    gBold = gFont;
    gDefaultFont = true;
    const char* reg = "C:/Windows/Fonts/segoeui.ttf";
    const char* bold = "C:/Windows/Fonts/seguisb.ttf";
    if (FileExists(reg)) {
        Font f = LoadFontEx(reg, 36, nullptr, 0);
        if (f.texture.id != 0) {
            SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
            gFont = f;
            gBold = f;
            gDefaultFont = false;
        }
    }
    if (!gDefaultFont && FileExists(bold)) {
        Font b = LoadFontEx(bold, 36, nullptr, 0);
        if (b.texture.id != 0) {
            SetTextureFilter(b.texture, TEXTURE_FILTER_BILINEAR);
            gBold = b;
        }
    }
}

float Spacing(float size) { return gDefaultFont ? size / 10.f : 0.f; }
void Txt(const char* s, float x, float y, float size, Color c, bool bold = false) {
    DrawTextEx(bold ? gBold : gFont, s, Vector2{x, y}, size, Spacing(size), c);
}
float TxtW(const char* s, float size, bool bold = false) {
    return MeasureTextEx(bold ? gBold : gFont, s, size, Spacing(size)).x;
}
void TxtCentered(const char* s, float cx, float y, float size, Color c, bool bold = false) {
    Txt(s, cx - TxtW(s, size, bold) * 0.5f, y, size, c, bold);
}

// ============================================================ fake lighting for boxes
// raylib's DrawCube is unlit (all faces identical). We draw our own boxes with
// per-face shading so the scene reads as 3D. Only the 3 camera-facing faces are drawn.
Vector3 gToCam{0, 1, 0}, gLight{0, 1, 0};

Vector3 RotY(Vector3 v, float yawDeg) {
    const float a = yawDeg * DEG2RAD, c = cosf(a), s = sinf(a);
    return Vector3{v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}

void DrawBox(Vector3 center, Vector3 size, float yawDeg, Color col) {
    static const int ax[3][2] = {{1, 2}, {0, 2}, {0, 1}};
    static const float su[4] = {1, -1, -1, 1}, sv[4] = {1, 1, -1, -1};
    const float hh[3] = {size.x * 0.5f, size.y * 0.5f, size.z * 0.5f};
    for (int a = 0; a < 3; ++a) {
        for (int sg = -1; sg <= 1; sg += 2) {
            float nl[3] = {0, 0, 0};
            nl[a] = static_cast<float>(sg);
            const Vector3 nw = RotY(Vector3{nl[0], nl[1], nl[2]}, yawDeg);
            if (Vector3DotProduct(nw, gToCam) <= 0.02f) continue; // face points away
            const float k = 0.42f + 0.58f * std::max(0.f, Vector3DotProduct(nw, gLight));
            const Color fc = Rgb((int)std::min(255.f, col.r * k), (int)std::min(255.f, col.g * k),
                                 (int)std::min(255.f, col.b * k), col.a);
            const int u = ax[a][0], v = ax[a][1];
            Vector3 p[4];
            for (int i = 0; i < 4; ++i) {
                float l[3];
                l[a] = sg * hh[a];
                l[u] = su[i] * hh[u];
                l[v] = sv[i] * hh[v];
                const Vector3 w = RotY(Vector3{l[0], l[1], l[2]}, yawDeg);
                p[i] = Vector3{center.x + w.x, center.y + w.y, center.z + w.z};
            }
            DrawTriangle3D(p[0], p[1], p[2], fc);
            DrawTriangle3D(p[0], p[2], p[3], fc);
        }
    }
}

// ============================================================ characters
struct Look { Color shirt, pants, hair, skin; };

const Color kShirts[] = {Rgb(214, 84, 84), Rgb(80, 140, 210), Rgb(232, 170, 60), Rgb(110, 180, 120),
                         Rgb(170, 110, 200), Rgb(230, 130, 160), Rgb(90, 190, 190), Rgb(205, 205, 215)};
const Color kPants[] = {Rgb(50, 56, 78), Rgb(70, 62, 56), Rgb(40, 44, 52), Rgb(86, 92, 110)};
const Color kHairs[] = {Rgb(36, 28, 24), Rgb(90, 60, 40), Rgb(190, 150, 80), Rgb(25, 25, 30), Rgb(140, 140, 150)};
const Color kSkins[] = {Rgb(238, 200, 170), Rgb(214, 164, 126), Rgb(168, 118, 84), Rgb(120, 82, 58)};

void DrawPerson(Vector3 p, float yaw, const Look& lk, float legSwing, float armSwing) {
    auto at = [&](float x, float y, float z) {
        const Vector3 r = RotY(Vector3{x, y, z}, yaw);
        return Vector3{p.x + r.x, p.y + r.y, p.z + r.z};
    };
    DrawBox(at(-0.15f, 0.36f, legSwing * 0.20f), Vector3{0.21f, 0.72f, 0.24f}, yaw, lk.pants);
    DrawBox(at(0.15f, 0.36f, -legSwing * 0.20f), Vector3{0.21f, 0.72f, 0.24f}, yaw, lk.pants);
    DrawBox(at(0.f, 1.08f, 0.f), Vector3{0.56f, 0.62f, 0.30f}, yaw, lk.shirt);
    DrawBox(at(-0.38f, 1.06f, -armSwing * 0.20f), Vector3{0.13f, 0.58f, 0.16f}, yaw, lk.shirt);
    DrawBox(at(0.38f, 1.06f, armSwing * 0.20f), Vector3{0.13f, 0.58f, 0.16f}, yaw, lk.shirt);
    DrawBox(at(-0.38f, 0.72f, -armSwing * 0.20f), Vector3{0.12f, 0.12f, 0.14f}, yaw, lk.skin);
    DrawBox(at(0.38f, 0.72f, armSwing * 0.20f), Vector3{0.12f, 0.12f, 0.14f}, yaw, lk.skin);
    DrawSphereEx(at(0.f, 1.60f, 0.f), 0.21f, 8, 12, lk.skin);
    DrawBox(at(0.f, 1.75f, -0.03f), Vector3{0.38f, 0.11f, 0.38f}, yaw, lk.hair);
    DrawBox(at(-0.07f, 1.62f, 0.19f), Vector3{0.05f, 0.05f, 0.03f}, yaw, Rgb(30, 30, 40));
    DrawBox(at(0.07f, 1.62f, 0.19f), Vector3{0.05f, 0.05f, 0.03f}, yaw, Rgb(30, 30, 40));
}

struct Actor {
    Vector3 pos{0, 0, 0};
    float heading = 180.f; // degrees; 180 = facing the counters (-Z)
    float walkPhase = 0.f;
    bool moving = false;
    bool leaving = false;
    bool entered = false; // has walked in through the entrance corridor
    qube::CustomerState lastState = qube::CustomerState::Waiting;
    std::deque<Vector3> path;
    Look look{};
};

Actor MakeActor(int id, Vector3 pos) {
    Actor a;
    a.pos = pos;
    std::mt19937 r(static_cast<unsigned>(id) * 7919u + 13u);
    a.look = Look{kShirts[r() % 8], kPants[r() % 4], kHairs[r() % 5], kSkins[r() % 4]};
    return a;
}

float PathLength(const Actor& a) {
    float len = 0.f;
    Vector3 cur = a.pos;
    for (const Vector3& n : a.path) {
        len += hypotf(n.x - cur.x, n.z - cur.z);
        cur = n;
    }
    return len;
}

// ============================================================ actor simulation (visual layer)
void UpdateActors(const qube::Engine& eng, std::unordered_map<int, Actor>& actors, int& nextId, float dt,
                  float wallDt) {
    using qube::CustomerState;
    const auto& cs = eng.customers();
    const int nCounters = static_cast<int>(eng.counters().size());

    // where is each waiting customer? (lane class + position in that lane; lanes are never re-sorted)
    struct LanePos { int cls; int rank; };
    std::unordered_map<int, LanePos> where;
    for (int cls = 1; cls <= 3; ++cls) {
        const auto& l = eng.lane(cls);
        for (size_t i = 0; i < l.size(); ++i) where[l[i]] = LanePos{cls, static_cast<int>(i)};
    }

    while (nextId <= static_cast<int>(cs.size())) {
        const auto& c = cs[static_cast<size_t>(nextId - 1)];
        // customers present at t=0 start already standing in their lane; later ones walk in
        const auto w = where.find(nextId);
        const bool seeded = c.arrival <= 0.0 && w != where.end();
        const Vector3 p = seeded ? SlotPos(w->second.cls, w->second.rank)
                                 : Vector3{lay::kSpawn.x + ((nextId % 5) - 2) * 0.5f, 0.f,
                                           lay::kSpawn.z + (nextId % 3) * 0.6f};
        Actor a = MakeActor(nextId, p);
        a.entered = seeded;
        actors.emplace(nextId, a);
        ++nextId;
    }

    for (auto it = actors.begin(); it != actors.end();) {
        Actor& a = it->second;
        const qube::Customer& c = eng.customer(it->first);

        // ---- state transitions set up walking routes
        if (c.state != a.lastState) {
            const float cx = c.counter >= 0 ? CounterX(c.counter, nCounters) : a.pos.x;
            if (c.state == CustomerState::Approaching) {
                a.path.clear();
                a.path.push_back(Vector3{a.pos.x, 0.f, lay::kLaneZ});
                a.path.push_back(Vector3{cx, 0.f, lay::kLaneZ});
                a.path.push_back(Vector3{cx, 0.f, lay::kStandZ});
            } else if (c.state == CustomerState::Done) {
                a.path.clear();
                a.path.push_back(Vector3{a.pos.x, 0.f, lay::kLaneZ});
                a.path.push_back(Vector3{lay::kExit.x, 0.f, lay::kLaneZ});
                a.leaving = true;
            }
            a.lastState = c.state;
        }

        // ---- continuous targets
        float speed = 3.4f;
        if (c.state == CustomerState::Waiting) {
            const auto w = where.find(it->first);
            if (w != where.end()) {
                const Vector3 slot = SlotPos(w->second.cls, w->second.rank);
                a.path.clear();
                if (!a.entered) {
                    // walk along the entrance corridor to this lane, then up the lane
                    if (a.pos.z <= lay::kEntryZ + 0.05f) a.entered = true;
                    else a.path.push_back(Vector3{slot.x, 0.f, lay::kEntryZ});
                }
                a.path.push_back(slot);
            }
        } else if (c.state == CustomerState::Approaching) {
            // arrive exactly when the engine starts the service
            const double left = eng.counters()[static_cast<size_t>(c.counter)].timer;
            speed = std::max(speed, PathLength(a) / static_cast<float>(std::max(0.05, left)));
        } else if (c.state == CustomerState::Serving && a.path.empty()) {
            const Vector3 stand{CounterX(c.counter, nCounters), 0.f, lay::kStandZ};
            if (hypotf(stand.x - a.pos.x, stand.z - a.pos.z) > 0.05f) a.path.push_back(stand);
        }

        // ---- move along path
        const Vector3 before = a.pos;
        // waiting customers keep closing up the lane even while paused
        const float stepDt = c.state == CustomerState::Waiting ? std::max(dt, wallDt) : dt;
        float budget = speed * stepDt;
        float dirX = 0.f, dirZ = 0.f;
        while (budget > 0.f && !a.path.empty()) {
            const Vector3 t = a.path.front();
            const float dx = t.x - a.pos.x, dz = t.z - a.pos.z;
            const float dist = hypotf(dx, dz);
            if (dist <= budget) {
                a.pos.x = t.x;
                a.pos.z = t.z;
                budget -= dist;
                a.path.pop_front();
                if (dist > 1e-5f) { dirX = dx; dirZ = dz; }
            } else {
                a.pos.x += dx / dist * budget;
                a.pos.z += dz / dist * budget;
                dirX = dx;
                dirZ = dz;
                budget = 0.f;
            }
        }
        const float moved = hypotf(a.pos.x - before.x, a.pos.z - before.z);
        a.moving = moved > 1e-4f;
        if (a.moving) a.walkPhase += moved * 3.4f;

        // ---- smooth heading
        float target = 180.f;
        if (a.moving || !a.path.empty()) {
            if (fabsf(dirX) + fabsf(dirZ) > 1e-5f) target = atan2f(dirX, dirZ) * RAD2DEG;
            else target = a.heading;
        }
        const float diff = fmodf(target - a.heading + 540.f, 360.f) - 180.f;
        a.heading += diff * std::min(1.f, wallDt * 12.f);

        if (a.leaving && a.path.empty()) it = actors.erase(it);
        else ++it;
    }
}

// ============================================================ UI state shared by HUD + controls
enum class RunState { Ready, Running, Paused };
constexpr float kSpeeds[6] = {0.25f, 0.5f, 1.f, 2.f, 4.f, 8.f};
Vector2 gMouse{0, 0};
bool gClick = false, gDown = false, gHoverAny = false;
bool gModalOn = false, gInModal = false; // while the settings window is open, clicks under it are ignored
Rectangle gModal{0, 0, 0, 0};

// ---- user-tunable settings (saved to qube_settings.txt next to where you launch the program)
struct Settings {
    float arrivalMean = 5.5f; // average seconds between random arrivals
    float rushEvery = 45.f;   // average seconds between rushes (0 = off)
    int rushSize = 6;         // people per rush (+/- 1.5)
    int svcMin = 4, svcMax = 12;       // service time range of random customers (s)
    int p3Share = 15, p2Share = 25;    // % of arrivals; P1 gets the rest
    double agingOverride = 6.0;        // s a lane front waits before any counter may take it
    double agingInterval = 15.0;       // s of waiting worth one priority level
    double starvation = 60.0;          // s; a wait longer than this counts as starvation
    double approach = 3.0;             // s to walk from the lane to the desk
    int counters = 4;                  // 2..6 (changing it restarts the simulation)
};
Settings gSet;

void ClampSettings() {
    gSet.arrivalMean = std::clamp(gSet.arrivalMean, 1.f, 20.f);
    gSet.rushEvery = gSet.rushEvery < 5.f ? 0.f : std::min(gSet.rushEvery, 300.f);
    gSet.rushSize = std::clamp(gSet.rushSize, 2, 20);
    gSet.svcMin = std::clamp(gSet.svcMin, 1, 60);
    gSet.svcMax = std::clamp(gSet.svcMax, 1, 60);
    if (gSet.svcMax < gSet.svcMin) gSet.svcMax = gSet.svcMin;
    gSet.p3Share = std::clamp(gSet.p3Share, 0, 100);
    gSet.p2Share = std::clamp(gSet.p2Share, 0, 100 - gSet.p3Share);
    gSet.agingOverride = std::clamp(gSet.agingOverride, 0.0, 300.0);
    gSet.agingInterval = std::clamp(gSet.agingInterval, 1.0, 120.0);
    gSet.starvation = std::clamp(gSet.starvation, 5.0, 600.0);
    gSet.approach = std::clamp(gSet.approach, 0.5, 10.0);
    gSet.counters = std::clamp(gSet.counters, 2, 6);
}

void ApplySettings(qube::Engine& eng) { // everything except the counter count, which restarts the simulation
    eng.setAgingOverrideWait(gSet.agingOverride);
    eng.setAgingInterval(gSet.agingInterval);
    eng.setStarvationThreshold(gSet.starvation);
    eng.setApproachTime(gSet.approach);
}

const char* kSettingsFile = "qube_settings.txt";

void SaveSettings() {
    std::ofstream f(kSettingsFile);
    if (!f) return;
    f << "arrivalMean " << gSet.arrivalMean << "\nrushEvery " << gSet.rushEvery << "\nrushSize " << gSet.rushSize
      << "\nsvcMin " << gSet.svcMin << "\nsvcMax " << gSet.svcMax << "\np3Share " << gSet.p3Share
      << "\np2Share " << gSet.p2Share << "\nagingOverride " << gSet.agingOverride << "\nagingInterval "
      << gSet.agingInterval << "\nstarvation " << gSet.starvation << "\napproach " << gSet.approach
      << "\ncounters " << gSet.counters << "\n";
}

void LoadSettings() {
    std::ifstream f(kSettingsFile);
    std::string k;
    double v;
    while (f >> k >> v) {
        if (k == "arrivalMean") gSet.arrivalMean = (float)v;
        else if (k == "rushEvery") gSet.rushEvery = (float)v;
        else if (k == "rushSize") gSet.rushSize = (int)std::lround(v);
        else if (k == "svcMin") gSet.svcMin = (int)std::lround(v);
        else if (k == "svcMax") gSet.svcMax = (int)std::lround(v);
        else if (k == "p3Share") gSet.p3Share = (int)std::lround(v);
        else if (k == "p2Share") gSet.p2Share = (int)std::lround(v);
        else if (k == "agingOverride") gSet.agingOverride = v;
        else if (k == "agingInterval") gSet.agingInterval = v;
        else if (k == "starvation") gSet.starvation = v;
        else if (k == "approach") gSet.approach = v;
        else if (k == "counters") gSet.counters = (int)std::lround(v);
    }
    ClampSettings();
}

Color Lighten(Color c, float t) {
    return Rgb((int)(c.r + (255 - c.r) * t), (int)(c.g + (255 - c.g) * t), (int)(c.b + (255 - c.b) * t), c.a);
}

bool Button(Rectangle r, const char* label, Color base, float fs = 13.f) {
    const bool covered = gModalOn && !gInModal && CheckCollisionPointRec(gMouse, gModal);
    const bool hover = !covered && CheckCollisionPointRec(gMouse, r);
    if (hover) gHoverAny = true;
    DrawRectangleRounded(r, 0.3f, 8, hover ? Lighten(base, 0.18f) : base);
    TxtCentered(label, r.x + r.width * 0.5f, r.y + (r.height - fs * 1.3f) * 0.5f, fs, Rgb(244, 246, 252), true);
    return hover && gClick;
}

// ============================================================ 3D scene
void DrawPost(Vector3 base) {
    DrawCylinder(base, 0.07f, 0.07f, 0.95f, 8, Rgb(190, 195, 205));
    DrawCylinder(Vector3{base.x, 0.f, base.z}, 0.2f, 0.2f, 0.05f, 12, Rgb(150, 155, 165));
    DrawSphereEx(Vector3{base.x, 0.98f, base.z}, 0.1f, 6, 8, Rgb(215, 190, 90));
}

void DrawRopeLine(Vector3 a, Vector3 b, int posts, Color rope) {
    for (int i = 0; i < posts; ++i) {
        const float t = posts > 1 ? static_cast<float>(i) / (posts - 1) : 0.f;
        DrawPost(Vector3{a.x + (b.x - a.x) * t, 0.f, a.z + (b.z - a.z) * t});
    }
    DrawCylinderEx(Vector3{a.x, 0.85f, a.z}, Vector3{b.x, 0.85f, b.z}, 0.035f, 0.035f, 6, rope);
}

void DrawCounter(float cx, const qube::Counter& k, double simTime, float dw) {
    const float pw = std::min(2.3f, dw - 0.2f); // wall sign width
    const Color status = CounterColor(k.phase);
    const bool serving = k.phase == qube::CounterPhase::Serving;

    // staff member behind the desk, facing the customers (+Z)
    const Look staff{Rgb(40, 76, 150), Rgb(34, 38, 52), kHairs[(k.id * 2 + 1) % 5], kSkins[(k.id + 1) % 4]};
    const float typing = serving ? sinf(static_cast<float>(simTime) * 7.f) * 0.8f : 0.f;
    DrawPerson(Vector3{cx, 0.f, lay::kStaffZ}, 0.f, staff, 0.f, typing);

    // desk
    DrawBox(Vector3{cx, 0.5f, lay::kDeskZ}, Vector3{dw, 1.0f, 1.3f}, 0.f, Rgb(122, 88, 62));
    DrawBox(Vector3{cx, 0.7f, lay::kDeskZ + 0.67f}, Vector3{dw - 0.2f, 0.16f, 0.04f}, 0.f, PriorityColor(k.homeClass));
    DrawBox(Vector3{cx, 1.04f, lay::kDeskZ}, Vector3{dw + 0.2f, 0.08f, 1.5f}, 0.f, Rgb(214, 190, 156));
    DrawBox(Vector3{cx - 0.7f, 1.38f, lay::kDeskZ - 0.15f}, Vector3{0.8f, 0.5f, 0.05f}, 0.f, Rgb(24, 28, 36));
    DrawBox(Vector3{cx - 0.7f, 1.14f, lay::kDeskZ - 0.15f}, Vector3{0.12f, 0.1f, 0.12f}, 0.f, Rgb(40, 44, 54));
    DrawBox(Vector3{cx + 0.9f, 1.18f, lay::kDeskZ + 0.15f}, Vector3{0.36f, 0.16f, 0.30f}, 0.f, status);

    // wall sign plate + status lamp bar
    DrawBox(Vector3{cx, 3.3f, -7.9f}, Vector3{pw, 1.0f, 0.12f}, 0.f, Rgb(22, 26, 36));
    DrawBox(Vector3{cx, 2.68f, -7.88f}, Vector3{pw, 0.14f, 0.10f}, 0.f, status);
    DrawBox(Vector3{cx, 3.86f, -7.88f}, Vector3{pw, 0.14f, 0.10f}, 0.f, PriorityColor(k.homeClass)); // assigned class

    // class-coloured spot where customers stand while being served
    DrawCylinder(Vector3{cx, 0.05f, lay::kStandZ}, 0.7f, 0.7f, 0.03f, 28, PriorityColor(k.homeClass));
    DrawCylinder(Vector3{cx, 0.06f, lay::kStandZ}, 0.56f, 0.56f, 0.03f, 28, Rgb(150, 156, 170));
}

void DrawScene(const qube::Engine& eng, const std::unordered_map<int, Actor>& actors) {
    // ---- floor: grout plane + tiles
    DrawPlane(Vector3{0.f, 0.f, (lay::kFloorZ0 + lay::kFloorZ1) * 0.5f},
              Vector2{lay::kFloorX1 - lay::kFloorX0, lay::kFloorZ1 - lay::kFloorZ0}, Rgb(122, 128, 140));
    for (int ix = (int)lay::kFloorX0; ix < (int)lay::kFloorX1; ++ix) {
        for (int iz = (int)lay::kFloorZ0; iz < (int)lay::kFloorZ1; ++iz) {
            const bool alt = ((ix + iz) & 1) != 0;
            DrawPlane(Vector3{ix + 0.5f, 0.01f, iz + 0.5f}, Vector2{0.97f, 0.97f},
                      alt ? Rgb(176, 183, 195) : Rgb(189, 195, 206));
        }
    }
    // entrance / exit mats
    DrawPlane(Vector3{0.f, 0.02f, 11.4f}, Vector2{18.f, 2.8f}, Rgb(88, 98, 126));  // entry corridor
    DrawPlane(Vector3{0.f, 0.03f, 11.9f}, Vector2{4.4f, 1.6f}, Rgb(70, 88, 130));  // entrance mat
    DrawPlane(Vector3{12.9f, 0.02f, lay::kLaneZ}, Vector2{2.6f, 2.4f}, Rgb(64, 140, 98));
    // three priority lanes: coloured carpet, slot markers, rope barriers
    for (int cls = 1; cls <= 3; ++cls) {
        const float cx = LaneCenterX(cls), hw = LaneHalfW(cls);
        const Color pc = PriorityColor(cls);
        const Color carpet = Mix(Rgb(21, 24, 32), pc, 0.30f);
        const Color border = Mix(Rgb(21, 24, 32), pc, 0.55f);
        DrawPlane(Vector3{cx, 0.02f, 4.9f}, Vector2{2.f * hw + 0.5f, 9.9f}, border);
        DrawPlane(Vector3{cx, 0.03f, 4.9f}, Vector2{2.f * hw, 9.4f}, carpet);
        for (int i = 0; i < LaneCapacity(cls); ++i) {
            const Vector3 sl = SlotPos(cls, i);
            DrawCylinder(Vector3{sl.x, 0.04f, sl.z}, 0.46f, 0.46f, 0.01f, 20, Mix(carpet, pc, 0.22f));
        }
        const Color rope = Mix(Rgb(40, 44, 56), pc, 0.85f);
        DrawRopeLine(Vector3{cx - hw - 0.1f, 0.f, 0.2f}, Vector3{cx - hw - 0.1f, 0.f, 9.8f}, 5, rope);
        DrawRopeLine(Vector3{cx + hw + 0.1f, 0.f, 0.2f}, Vector3{cx + hw + 0.1f, 0.f, 9.8f}, 5, rope);
    }
    // safety lane line in front of the counters
    DrawPlane(Vector3{0.f, 0.03f, -0.55f}, Vector2{24.f, 0.14f}, Rgb(232, 192, 64));

    // ---- walls
    DrawBox(Vector3{0.f, 2.f, -8.25f}, Vector3{24.5f, 4.f, 0.5f}, 0.f, Rgb(72, 82, 108));
    DrawBox(Vector3{0.f, 0.16f, -7.98f}, Vector3{24.f, 0.32f, 0.06f}, 0.f, Rgb(42, 48, 64));
    DrawBox(Vector3{-12.25f, 2.f, 1.f}, Vector3{0.5f, 4.f, 18.5f}, 0.f, Rgb(64, 74, 98));
    DrawBox(Vector3{-11.98f, 0.16f, 1.f}, Vector3{0.06f, 0.32f, 18.f}, 0.f, Rgb(42, 48, 64));

    // ---- counters (desk + staff)
    const int n = static_cast<int>(eng.counters().size());
    for (int i = 0; i < n; ++i) DrawCounter(CounterX(i, n), eng.counters()[static_cast<size_t>(i)], eng.now(),
                                          std::min(3.4f, CounterSpacing(n) - 0.6f));

    // ---- customers
    for (const auto& kv : actors) {
        const Actor& a = kv.second;
        const qube::Customer& c = eng.customer(kv.first);
        Color ring;
        switch (c.state) {
            case qube::CustomerState::Waiting: ring = WaitColor(eng.now() - c.arrival); break;
            case qube::CustomerState::Approaching: ring = Rgb(90, 170, 255); break;
            default: ring = Rgb(170, 180, 200); break;
        }
        DrawCylinder(Vector3{a.pos.x, 0.05f, a.pos.z}, 0.56f, 0.56f, 0.03f, 24, ring);
        DrawCylinder(Vector3{a.pos.x, 0.06f, a.pos.z}, 0.42f, 0.42f, 0.03f, 24, Rgb(34, 40, 56));
        const float swing = a.moving ? sinf(a.walkPhase) : 0.f;
        DrawPerson(a.pos, a.heading, a.look, swing, -swing);
    }
}

// ============================================================ 2D overlays on the scene
void DrawLabelBox(Vector2 anchor, const char* id, const char* prio, Color prioCol, const char* l2, Color accent) {
    const float gap = 6.f;
    const float line1W = TxtW(id, 12.5f, true) + gap + TxtW(prio, 12.5f, true);
    const float w = std::max(line1W, TxtW(l2, 11.f)) + 12.f;
    const float h = 31.f;
    const Rectangle outer{anchor.x - w * 0.5f - 1.5f, anchor.y - h - 1.5f, w + 3.f, h + 3.f};
    const Rectangle inner{anchor.x - w * 0.5f, anchor.y - h, w, h};
    DrawRectangleRounded(outer, 0.3f, 6, accent);
    DrawRectangleRounded(inner, 0.28f, 6, Rgb(16, 19, 27, 235));
    const float x0 = anchor.x - line1W * 0.5f;
    Txt(id, x0, inner.y + 2.f, 12.5f, Rgb(240, 243, 250), true);
    Txt(prio, x0 + TxtW(id, 12.5f, true) + gap, inner.y + 2.f, 12.5f, prioCol, true);
    TxtCentered(l2, anchor.x, inner.y + 16.f, 11.f, accent);
}

void DrawLaneTag(Vector2 sp, int cls, size_t n) {
    static const char* names[3] = {"NORMAL", "HIGH", "URGENT"};
    char t[48];
    std::snprintf(t, sizeof t, "P%d %s   %zu / %d", cls, names[cls - 1], n, LaneCapacity(cls));
    const Color pc = PriorityColor(cls);
    const float w = TxtW(t, 12.f, true) + 20.f, h = 22.f, y = sp.y + 6.f;
    DrawRectangleRounded(Rectangle{sp.x - w * 0.5f - 1.5f, y - 1.5f, w + 3.f, h + 3.f}, 0.4f, 6, pc);
    DrawRectangleRounded(Rectangle{sp.x - w * 0.5f, y, w, h}, 0.4f, 6, Rgb(16, 19, 27, 240));
    TxtCentered(t, sp.x, y + 3.f, 12.f, Rgb(240, 243, 250), true);
}

void DrawSceneLabels(qube::Engine& eng, const std::unordered_map<int, Actor>& actors, const Camera3D& cam) {
    std::vector<const std::pair<const int, Actor>*> list;
    list.reserve(actors.size());
    for (const auto& kv : actors) list.push_back(&kv);
    std::sort(list.begin(), list.end(), [](auto* a, auto* b) {
        return a->second.pos.x + a->second.pos.z < b->second.pos.x + b->second.pos.z; // far first
    });

    for (const auto* kv : list) {
        const Actor& a = kv->second;
        const qube::Customer& c = eng.customer(kv->first);
        char l1[32], l1b[8], l2[32];
        std::snprintf(l1, sizeof l1, "#%d", c.id);
        std::snprintf(l1b, sizeof l1b, "P%d", c.priority);
        Color accent;
        switch (c.state) {
            case qube::CustomerState::Waiting: {
                const double w = eng.now() - c.arrival;
                std::snprintf(l2, sizeof l2, "%.0fs", w);
                accent = WaitColor(w);
                break;
            }
            case qube::CustomerState::Approaching:
                std::snprintf(l2, sizeof l2, "to C%d", c.counter + 1);
                accent = Rgb(90, 170, 255);
                break;
            default: {
                const double left = eng.counters()[static_cast<size_t>(c.counter)].timer;
                std::snprintf(l2, sizeof l2, "C%d  %.0fs", c.counter + 1, std::max(0.0, left));
                accent = Rgb(170, 180, 200);
                break;
            }
        }
        const Vector2 sp = GetWorldToScreen(Vector3{a.pos.x, 2.15f, a.pos.z}, cam);
        DrawLabelBox(sp, l1, l1b, PriorityColor(c.priority), l2, accent);
    }

    // counter sign text
    const int n = static_cast<int>(eng.counters().size());
    for (int i = 0; i < n; ++i) {
        const qube::Counter& k = eng.counters()[static_cast<size_t>(i)];
        const float cx = CounterX(i, n);
        const Vector2 sp = GetWorldToScreen(Vector3{cx, 3.3f, -7.8f}, cam);
        char t1[16], t2[32];
        const bool many = n > 4;
        std::snprintf(t1, sizeof t1, many ? "C%d" : "COUNTER %d", i + 1);
        if (k.phase == qube::CounterPhase::Idle) std::snprintf(t2, sizeof t2, "OPEN");
        else if (k.phase == qube::CounterPhase::Approaching) std::snprintf(t2, sizeof t2, many ? "> #%d" : "CALLING #%d", k.customerId);
        else std::snprintf(t2, sizeof t2, many ? "#%d" : "SERVING #%d", k.customerId);
        TxtCentered(t1, sp.x, sp.y - 17.f, 15.f, Rgb(240, 243, 250), true);
        TxtCentered(t2, sp.x, sp.y + 2.f, 12.f, CounterColor(k.phase), true);

        // assigned class: click to cycle P1 -> P2 -> P3
        char t3[16];
        std::snprintf(t3, sizeof t3, many ? "P%d" : "HOME  P%d", k.homeClass);
        const float bw = many ? 40.f : 76.f;
        const Rectangle br{sp.x - bw * 0.5f, sp.y + 24.f, bw, 20.f};
        if (Button(br, t3, Mix(PriorityColor(k.homeClass), Rgb(16, 19, 27), 0.45f), 12.f))
            eng.setCounterClass(i, NextClass(k.homeClass));
        if (CheckCollisionPointRec(gMouse, br))
            TxtCentered("click to change class", sp.x, sp.y + 47.f, 11.f, Rgb(210, 216, 232));
    }

    for (int cls = 1; cls <= 3; ++cls)
        DrawLaneTag(GetWorldToScreen(Vector3{LaneCenterX(cls), 0.1f, 10.0f}, cam), cls, eng.lane(cls).size());

    const Vector2 en = GetWorldToScreen(Vector3{0.f, 0.1f, 13.4f}, cam);
    TxtCentered("ENTRANCE", en.x, en.y, 12.f, Rgb(150, 170, 220), true);
    const Vector2 ex = GetWorldToScreen(Vector3{12.9f, 0.1f, lay::kLaneZ + 1.6f}, cam);
    TxtCentered("EXIT", ex.x, ex.y, 12.f, Rgb(120, 210, 150), true);
}


float Chip(float xRight, float y, const char* s, Color fg, Color bg) {
    const float w = TxtW(s, 12.5f, true) + 22.f;
    const float x = xRight - w;
    DrawRectangleRounded(Rectangle{x, y, w, 26.f}, 0.45f, 8, bg);
    Txt(s, x + 11.f, y + 5.f, 12.5f, fg, true);
    return x - 8.f;
}

void DrawTopBar(qube::Engine& eng, float speed, RunState rs, bool autoSpawn, bool& settingsOpen, int W) {
    DrawRectangle(0, 0, W, (int)lay::kTopH, Rgb(20, 23, 31));
    DrawRectangle(0, (int)lay::kTopH - 1, W, 1, Rgb(46, 52, 68));
    DrawRectangleRounded(Rectangle{14, 9, 28, 28}, 0.3f, 8, Rgb(66, 118, 235));
    TxtCentered("Q", 28, 11, 20, Rgb(255, 255, 255), true);
    Txt("QUBE", 52, 6, 22, Rgb(240, 243, 250), true);
    Txt("Queue-Based Unified Balancing Engine", 112, 14, 12, Rgb(120, 130, 152));
    if (Button(Rectangle{112.f + TxtW("Queue-Based Unified Balancing Engine", 12.f) + 22.f, 10.f, 92.f, 26.f}, "Settings",
               settingsOpen ? Rgb(48, 86, 170) : Rgb(34, 39, 52), 12.5f))
        settingsOpen = !settingsOpen;

    float x = (float)W - 14.f;
    char buf[48];
    const int secs = (int)eng.now();
    std::snprintf(buf, sizeof buf, "T+ %02d:%02d", secs / 60, secs % 60);
    x = Chip(x, 10, buf, Rgb(200, 208, 225), Rgb(34, 39, 52));
    std::snprintf(buf, sizeof buf, "%.2gx", speed);
    x = Chip(x, 10, buf, Rgb(200, 208, 225), Rgb(34, 39, 52));
    if (rs == RunState::Ready) x = Chip(x, 10, "READY", Rgb(150, 190, 255), Rgb(28, 44, 82));
    else if (rs == RunState::Paused) x = Chip(x, 10, "PAUSED", Rgb(255, 205, 90), Rgb(70, 56, 24));
    else x = Chip(x, 10, "RUNNING", Rgb(110, 225, 150), Rgb(24, 62, 42));
    x = Chip(x, 10, autoSpawn ? "AUTO-ARRIVALS ON" : "AUTO-ARRIVALS OFF", Rgb(200, 208, 225), Rgb(34, 39, 52));
    {
        const qube::Policy pols[4] = {qube::Policy::FCFS, qube::Policy::Priority, qube::Policy::SJF, qube::Policy::Aging};
        float bw[4], total = 0.f;
        for (int i = 0; i < 4; ++i) {
            bw[i] = TxtW(PolicyName(pols[i]), 12.f, true) + 22.f;
            total += bw[i] + 4.f;
        }
        float px = x - total + 4.f;
        for (int i = 0; i < 4; ++i) {
            const bool on = eng.config().policy == pols[i];
            if (Button(Rectangle{px, 10, bw[i], 26}, PolicyName(pols[i]), on ? Rgb(48, 86, 170) : Rgb(34, 39, 52), 12.f))
                eng.setPolicy(pols[i]);
            px += bw[i] + 4.f;
        }
    }
}

void DrawTile(float x, float y, float w, float h, const char* label, const char* value, Color accent, float vs = 22.f) {
    DrawRectangleRounded(Rectangle{x, y, w, h}, 0.15f, 8, Rgb(31, 36, 48));
    DrawRectangle((int)x, (int)y + 10, 3, (int)h - 20, accent);
    Txt(label, x + 12, y + 6, 11.f, Rgb(140, 150, 170));
    Txt(value, x + 12, y + 21, vs, Rgb(240, 243, 250), true);
}

Color FairnessColor(double j) {
    if (j >= 0.8) return Rgb(80, 200, 120);
    if (j >= 0.6) return Rgb(240, 200, 60);
    return Rgb(235, 70, 70);
}

void DrawOverviewTab(qube::Engine& eng, const qube::Stats& s, float x, float y, float cw, float bottom) {
    const float tw = (cw - 10.f) * 0.5f, th = 52.f;
    char v[32];
    auto tile = [&](int col, int row, const char* label, const char* val, Color acc) {
        DrawTile(x + col * (tw + 10.f), y + row * (th + 6.f), tw, th, label, val, acc);
    };
    std::snprintf(v, sizeof v, "%d", s.waiting);
    tile(0, 0, "WAITING", v, Rgb(240, 200, 60));
    std::snprintf(v, sizeof v, "%d", s.inService);
    tile(1, 0, "BEING SERVED", v, Rgb(90, 170, 255));
    std::snprintf(v, sizeof v, "%.1fs", s.avgWait);
    tile(0, 1, "AVG WAIT", v, Rgb(80, 200, 120));
    std::snprintf(v, sizeof v, "%.1fs", s.longestWait);
    tile(1, 1, "LONGEST WAIT", v, Rgb(235, 70, 70));
    std::snprintf(v, sizeof v, "%d", s.completed);
    tile(0, 2, "COMPLETED", v, Rgb(170, 110, 200));
    std::snprintf(v, sizeof v, "%.1f/min", s.throughputPerMin);
    tile(1, 2, "THROUGHPUT", v, Rgb(90, 190, 190));
    std::snprintf(v, sizeof v, "%d", s.starved);
    char starvedLabel[32];
    std::snprintf(starvedLabel, sizeof starvedLabel, "STARVED (>%.0fs)", eng.config().starvationThreshold);
    tile(0, 3, starvedLabel, v, s.starved > 0 ? Rgb(235, 70, 70) : Rgb(80, 200, 120));
    std::snprintf(v, sizeof v, "%.0f%%", s.avgUtilization * 100.0);
    tile(1, 3, "UTILIZATION", v, Rgb(66, 118, 235));
    y += 4 * (th + 6.f) + 4.f;

    Txt("AVG WAIT BY PRIORITY", x, y, 12, Rgb(120, 130, 152), true);
    const char* desc = PolicyDesc(eng.config().policy);
    Txt(desc, x + cw - TxtW(desc, 11.f), y + 1.f, 11.f, Rgb(150, 190, 255));
    y += 19.f;
    const float mw = (cw - 12.f) / 3.f;
    for (int p = 0; p < 3; ++p) {
        char lab[24];
        std::snprintf(lab, sizeof lab, "P%d  (%d)", p + 1, s.countByPriority[p]);
        std::snprintf(v, sizeof v, "%.1fs", s.avgWaitByPriority[p]);
        DrawTile(x + p * (mw + 6.f), y, mw, 52.f, lab, v, PriorityColor(p + 1), 19.f);
    }
    y += 52.f + 10.f;

    Txt("COUNTERS", x, y, 12, Rgb(120, 130, 152), true);
    y += 21.f;
    const int n = static_cast<int>(eng.counters().size());
    const float ch = std::clamp((bottom - y) / (float)n - 6.f, 26.f, 44.f);
    const bool compact = ch < 38.f; // many counters: single-row cards
    for (int i = 0; i < n; ++i) {
        const qube::Counter& k = eng.counters()[static_cast<size_t>(i)];
        const Color sc = CounterColor(k.phase);
        const double u = eng.utilization(i);
        DrawRectangleRounded(Rectangle{x, y, cw, ch}, 0.15f, 8, Rgb(31, 36, 48));
        DrawCircle((int)x + 16, (int)y + (compact ? 11 : 15), 5.f, sc);
        char t[48];
        std::snprintf(t, sizeof t, "Counter %d", i + 1);
        Txt(t, x + 30, y + (compact ? 3.f : 6.f), 13.5f, Rgb(235, 238, 246), true);
        {
            char b[8];
            std::snprintf(b, sizeof b, "P%d", k.homeClass);
            if (Button(Rectangle{x + 104.f, y + (compact ? 3.f : 5.f), 34.f, 18.f}, b,
                       Mix(PriorityColor(k.homeClass), Rgb(16, 19, 27), 0.45f), 11.5f))
                eng.setCounterClass(i, NextClass(k.homeClass));
        }
        if (k.phase == qube::CounterPhase::Idle) std::snprintf(t, sizeof t, "Idle");
        else if (k.phase == qube::CounterPhase::Approaching) std::snprintf(t, sizeof t, "Calling #%d", k.customerId);
        else std::snprintf(t, sizeof t, "Serving #%d", k.customerId);
        Txt(t, x + cw - 12 - TxtW(t, 12.f, true), y + (compact ? 4.f : 7.f), 12.f, sc, true);
        if (compact) {
            const float bw = cw - 28.f;
            DrawRectangleRounded(Rectangle{x + 14, y + ch - 8.f, bw, 5}, 0.5f, 6, Rgb(46, 52, 68));
            if (u > 0.01) DrawRectangleRounded(Rectangle{x + 14, y + ch - 8.f, bw * (float)u, 5}, 0.5f, 6, Rgb(66, 118, 235));
        } else {
            DrawRectangleRounded(Rectangle{x + 14, y + 29, cw - 190, 7}, 0.5f, 6, Rgb(46, 52, 68));
            if (u > 0.01) DrawRectangleRounded(Rectangle{x + 14, y + 29, (cw - 190) * (float)u, 7}, 0.5f, 6, Rgb(66, 118, 235));
            std::snprintf(t, sizeof t, "%.0f%% | %d served | %d borrowed", u * 100.0, k.served, k.servedForeign);
            Txt(t, x + cw - 12 - TxtW(t, 11.f), y + 24, 11.f, Rgb(150, 160, 180));
        }
        y += ch + 6.f;
    }
}

void DrawQueueChart(float x, float y, float w, float h, const std::vector<float>& d) {
    constexpr int kSamples = 150; // 2 s per sample = last 5 simulated minutes
    DrawRectangleRounded(Rectangle{x, y, w, h}, 0.06f, 8, Rgb(31, 36, 48));
    float peak = 5.f;
    for (float v : d) peak = std::max(peak, v);
    const float top = y + 10.f, bottom = y + h - 24.f, left = x + 10.f, right = x + w - 10.f;
    for (int i = 0; i <= 3; ++i) DrawRectangle((int)left, (int)(top + (bottom - top) * i / 3.f), (int)(right - left), 1, Rgb(46, 52, 68));
    for (size_t i = 1; i < d.size(); ++i) {
        const float x0 = right - (right - left) * (float)(d.size() - i) / (kSamples - 1);
        const float x1 = right - (right - left) * (float)(d.size() - 1 - i) / (kSamples - 1);
        const float y0 = bottom - (bottom - top) * d[i - 1] / peak;
        const float y1 = bottom - (bottom - top) * d[i] / peak;
        DrawLineEx(Vector2{x0, y0}, Vector2{x1, y1}, 2.f, Rgb(240, 200, 60));
    }
    char b[32];
    std::snprintf(b, sizeof b, "peak %d", (int)peak);
    Txt("people waiting", x + 10.f, y + h - 19.f, 11.f, Rgb(140, 150, 170));
    Txt(b, x + w - 10.f - TxtW(b, 11.f), y + h - 19.f, 11.f, Rgb(140, 150, 170));
}

void DrawFairnessTab(const qube::Stats& s, const std::vector<float>& qHist, float x, float y, float cw) {
    const float tw = (cw - 10.f) * 0.5f, th = 52.f;
    char v[48];
    auto tile = [&](int col, int row, const char* label, const char* val, Color acc, float vs = 22.f) {
        DrawTile(x + col * (tw + 10.f), y + row * (th + 6.f), tw, th, label, val, acc, vs);
    };
    std::snprintf(v, sizeof v, "%.2f", s.jainIndex);
    tile(0, 0, "JAIN FAIRNESS INDEX", v, FairnessColor(s.jainIndex));
    std::snprintf(v, sizeof v, "%.1fs", s.waitStdDev);
    tile(1, 0, "WAIT SPREAD (STD DEV)", v, Rgb(170, 110, 200));
    std::snprintf(v, sizeof v, "%.1fs", s.waitP90);
    tile(0, 1, "90TH PERCENTILE WAIT", v, Rgb(240, 140, 50));
    if (s.priorityGap > 0.0) std::snprintf(v, sizeof v, "%.1fx", s.priorityGap);
    else std::snprintf(v, sizeof v, "-");
    tile(1, 1, "P1 WAIT / P3 WAIT", v, Rgb(255, 98, 82));
    std::snprintf(v, sizeof v, "%.2f", s.counterBalance);
    tile(0, 2, "COUNTER BALANCE", v, FairnessColor(s.counterBalance));
    std::snprintf(v, sizeof v, "%d / %d / %d", s.starvedByPriority[0], s.starvedByPriority[1], s.starvedByPriority[2]);
    tile(1, 2, "STARVED  P1 / P2 / P3", v, s.starved > 0 ? Rgb(235, 70, 70) : Rgb(80, 200, 120), 17.f);
    y += 3 * (th + 6.f) + 4.f;

    Txt("AVG WAIT BY PRIORITY", x, y, 12, Rgb(120, 130, 152), true);
    y += 21.f;
    double mx = 1.0;
    for (int p = 0; p < 3; ++p) mx = std::max(mx, s.avgWaitByPriority[p]);
    for (int p = 0; p < 3; ++p) {
        char lab[8];
        std::snprintf(lab, sizeof lab, "P%d", p + 1);
        Txt(lab, x, y + 2.f, 12.5f, PriorityColor(p + 1), true);
        const float bx = x + 30.f, bw = cw - 30.f - 56.f;
        DrawRectangleRounded(Rectangle{bx, y + 4.f, bw, 11.f}, 0.5f, 6, Rgb(38, 44, 58));
        const float fw = bw * (float)(s.avgWaitByPriority[p] / mx);
        if (fw > 2.f) DrawRectangleRounded(Rectangle{bx, y + 4.f, fw, 11.f}, 0.5f, 6, PriorityColor(p + 1));
        std::snprintf(v, sizeof v, "%.1fs", s.avgWaitByPriority[p]);
        Txt(v, x + cw - TxtW(v, 12.f, true), y + 2.f, 12.f, Rgb(225, 232, 250), true);
        y += 24.f;
    }
    y += 8.f;

    Txt("QUEUE LENGTH - LAST 5 MIN", x, y, 12, Rgb(120, 130, 152), true);
    y += 20.f;
    DrawQueueChart(x, y, cw, 118.f, qHist);
    y += 118.f + 12.f;

    Txt("Jain index: 1.00 = everyone waited the same.", x, y, 11.f, Rgb(120, 130, 152));
    Txt("Priority policies lower it on purpose; watch", x, y + 15.f, 11.f, Rgb(120, 130, 152));
    Txt("the P1 / starved numbers to see if it costs P1.", x, y + 30.f, 11.f, Rgb(120, 130, 152));
}

void DrawPanel(qube::Engine& eng, const qube::Stats& s, int& tab, const std::vector<float>& qHist, int W, int H) {
    const float x0 = (float)W - lay::kPanelW, y0 = lay::kTopH;
    DrawRectangle((int)x0, (int)y0, (int)lay::kPanelW, H - (int)y0, Rgb(22, 25, 33));
    DrawRectangle((int)x0, (int)y0, 1, H - (int)y0, Rgb(46, 52, 68));

    const float cw = lay::kPanelW - 32.f;
    const float x = x0 + 16.f;
    float y = y0 + 10.f;
    const float tabW = (cw - 6.f) * 0.5f;
    if (Button(Rectangle{x, y, tabW, 26.f}, "OVERVIEW", tab == 0 ? Rgb(48, 86, 170) : Rgb(34, 39, 52), 12.f)) tab = 0;
    if (Button(Rectangle{x + tabW + 6.f, y, tabW, 26.f}, "FAIRNESS", tab == 1 ? Rgb(48, 86, 170) : Rgb(34, 39, 52), 12.f)) tab = 1;
    y += 34.f;

    if (tab == 0) DrawOverviewTab(eng, s, x, y, cw, (float)H - 82.f);
    else DrawFairnessTab(s, qHist, x, y, cw);

    y = (float)H - 74.f;
    Txt("SHORTCUTS", x, y, 12, Rgb(120, 130, 152), true);
    Txt("SPACE run  A add  B rush  G auto  F full", x, y + 20.f, 11.5f, Rgb(170, 178, 196));
    Txt("1-4 policy  TAB panel  S settings  UP/DOWN speed", x, y + 38.f, 11.5f, Rgb(170, 178, 196));
    Txt("Click a counter's P-badge to change its class", x, y + 56.f, 11.5f, Rgb(170, 178, 196));
}

void DrawLegend() {
    const float x = 16.f, y = lay::kTopH + 12.f;
    DrawRectangleRounded(Rectangle{x, y, 188, 62}, 0.12f, 8, Rgb(16, 19, 27, 200));
    Txt("WAITING TIME", x + 12, y + 7, 11, Rgb(130, 140, 160), true);
    const Color cs[4] = {WaitColor(0), WaitColor(kWaitGreen), WaitColor(kWaitYellow), WaitColor(kWaitOrange)};
    const char* ls[4] = {"< 10s", "10-20s", "20-30s", "30s+"};
    for (int i = 0; i < 4; ++i) {
        const float px = x + 12 + (i % 2) * 88, py = y + 26 + (i / 2) * 18;
        DrawCircle((int)px + 5, (int)py + 7, 5.f, cs[i]);
        Txt(ls[i], px + 16, py, 12, Rgb(200, 208, 225));
    }
}

// ============================================================ app helpers
std::mt19937 gRng(2026);
float Rand(float a, float b) { return std::uniform_real_distribution<float>(a, b)(gRng); }

void AddRandomCustomer(qube::Engine& eng) {
    const float r = Rand(0.f, 100.f);
    const int prio = r < (float)gSet.p3Share ? 3 : (r < (float)(gSet.p3Share + gSet.p2Share) ? 2 : 1);
    const double svc = std::round(Rand((float)gSet.svcMin, (float)gSet.svcMax) * 2.f) / 2.0;
    eng.addCustomer(prio, svc);
}

void SeedScene(qube::Engine& eng) {
    for (int i = 0; i < 12; ++i) AddRandomCustomer(eng);
}


// ============================================================ immediate-mode UI
void SpeedSlider(Rectangle track, int& idx) {
    static bool dragging = false;
    const Rectangle hit{track.x - 10.f, track.y - 14.f, track.width + 20.f, track.height + 28.f};
    const bool hover = CheckCollisionPointRec(gMouse, hit);
    if (hover) gHoverAny = true;
    if (gClick && hover) dragging = true;
    if (!gDown) dragging = false;
    if (dragging) {
        const float t = std::clamp((gMouse.x - track.x) / track.width, 0.f, 1.f);
        idx = (int)std::lround(t * 5.f);
    }
    DrawRectangleRounded(track, 0.5f, 6, Rgb(46, 52, 68));
    const float kx = track.x + track.width * (idx / 5.f);
    DrawRectangleRounded(Rectangle{track.x, track.y, kx - track.x, track.height}, 0.5f, 6, Rgb(66, 118, 235));
    for (int i = 0; i < 6; ++i)
        DrawCircle((int)(track.x + track.width * (i / 5.f)), (int)(track.y + track.height * 0.5f), 2.f, Rgb(110, 120, 145));
    DrawCircle((int)kx, (int)(track.y + track.height * 0.5f), 8.f, Rgb(225, 232, 250));
    DrawCircle((int)kx, (int)(track.y + track.height * 0.5f), 4.f, Rgb(66, 118, 235));
}

struct Controls {
    RunState run = RunState::Ready;
    int speedIdx = 2;
    bool autoSpawn = true;
    bool resetRequested = false;
    bool formOpen = false;
    int priority = 1, service = 8, qty = 1;
    int panelTab = 0; // 0 = overview, 1 = fairness
    bool settingsOpen = false;

    void toggleRun() {
        run = run == RunState::Running ? RunState::Paused : RunState::Running; // Ready/Paused -> Running
    }
};

void DrawReadyBanner(RunState rs, int W) {
    if (rs != RunState::Ready) return;
    const char* t = "READY  -  press Start (or SPACE) to begin";
    const float w = TxtW(t, 14.f, true) + 40.f, cx = ((float)W - lay::kPanelW) * 0.5f;
    DrawRectangleRounded(Rectangle{cx - w * 0.5f - 1, lay::kTopH + 13.f, w + 2, 36.f}, 0.4f, 8, Rgb(66, 118, 235));
    DrawRectangleRounded(Rectangle{cx - w * 0.5f, lay::kTopH + 14.f, w, 34.f}, 0.4f, 8, Rgb(20, 23, 31, 240));
    TxtCentered(t, cx, lay::kTopH + 21.f, 14.f, Rgb(225, 232, 250), true);
}

void DrawControls(qube::Engine& eng, Controls& ui, int W, int H) {
    const float availW = (float)W - lay::kPanelW;
    const float dockW = 850.f, dockH = 56.f;
    const float dx = (availW - dockW) * 0.5f, dy = (float)H - dockH - 14.f;
    DrawRectangleRounded(Rectangle{dx - 1, dy - 1, dockW + 2, dockH + 2}, 0.3f, 10, Rgb(46, 52, 68));
    DrawRectangleRounded(Rectangle{dx, dy, dockW, dockH}, 0.3f, 10, Rgb(20, 23, 31, 242));

    const float by = dy + 11.f, bh = 34.f;
    float x = dx + 14.f;
    auto separator = [&]() {
        x += 18.f;
        DrawRectangle((int)x, (int)dy + 12, 1, (int)dockH - 24, Rgb(46, 52, 68));
        x += 19.f;
    };

    const char* runLabel = ui.run == RunState::Ready ? "Start" : (ui.run == RunState::Running ? "Pause" : "Resume");
    const Color runCol = ui.run == RunState::Running ? Rgb(160, 118, 34) : Rgb(34, 128, 84);
    if (Button(Rectangle{x, by, 112, bh}, runLabel, runCol)) ui.toggleRun();
    x += 120.f;
    if (Button(Rectangle{x, by, 84, bh}, "Reset", Rgb(54, 60, 78))) ui.resetRequested = true;
    x += 84.f;
    separator();

    Txt("SPEED", x, by + 9.f, 11.f, Rgb(130, 140, 160), true);
    x += 46.f;
    SpeedSlider(Rectangle{x, by + bh * 0.5f - 3.f, 150.f, 6.f}, ui.speedIdx);
    x += 166.f;
    char sp[16];
    std::snprintf(sp, sizeof sp, "%.2gx", kSpeeds[ui.speedIdx]);
    Txt(sp, x, by + 8.f, 14.f, Rgb(240, 243, 250), true);
    x += 46.f;
    separator();

    const Rectangle addBtn{x, by, 138, bh};
    if (Button(addBtn, ui.formOpen ? "Close form" : "+ Add customer", ui.formOpen ? Rgb(66, 118, 235) : Rgb(48, 86, 170)))
        ui.formOpen = !ui.formOpen;
    x += 146.f;
    if (Button(Rectangle{x, by, 140, bh}, ui.autoSpawn ? "Auto-arrivals: ON" : "Auto-arrivals: OFF",
               ui.autoSpawn ? Rgb(34, 92, 70) : Rgb(54, 60, 78), 12.5f))
        ui.autoSpawn = !ui.autoSpawn;

    // ---- add-customer form
    if (!ui.formOpen) return;
    const float pw = 320.f, ph = 284.f;
    const float px = std::clamp(addBtn.x + addBtn.width - pw, 12.f, availW - pw - 12.f);
    const float py = dy - ph - 12.f;
    DrawRectangleRounded(Rectangle{px - 1, py - 1, pw + 2, ph + 2}, 0.06f, 10, Rgb(46, 52, 68));
    DrawRectangleRounded(Rectangle{px, py, pw, ph}, 0.06f, 10, Rgb(24, 28, 38, 248));
    Txt("ADD CUSTOMER", px + 16, py + 14, 13, Rgb(240, 243, 250), true);
    if (Button(Rectangle{px + pw - 40.f, py + 10.f, 24, 24}, "x", Rgb(54, 60, 78), 12.f)) ui.formOpen = false;

    const Color grey = Rgb(130, 140, 160), dark = Rgb(44, 50, 66);
    Txt("PRIORITY", px + 16, py + 48, 11, grey, true);
    const char* pl[3] = {"P1 Normal", "P2 High", "P3 Urgent"};
    const Color pc[3] = {Rgb(70, 110, 190), Rgb(190, 140, 30), Rgb(200, 60, 60)};
    const float bw = (pw - 32.f - 12.f) / 3.f;
    for (int i = 0; i < 3; ++i)
        if (Button(Rectangle{px + 16.f + i * (bw + 6.f), py + 66.f, bw, 34}, pl[i], ui.priority == i + 1 ? pc[i] : dark, 12.5f))
            ui.priority = i + 1;

    Txt("SERVICE TIME", px + 16, py + 112, 11, grey, true);
    const float sw = 46.f, g = 6.f, vw = 80.f, x0 = px + 16.f, sy = py + 130.f;
    const char* sl[4] = {"-5", "-1", "+1", "+5"};
    const int sd[4] = {-5, -1, 1, 5};
    // layout: [-5][-1][ value ][+1][+5]
    const float vx = x0 + 2 * (sw + g);
    const float rx[4] = {x0, x0 + sw + g, vx + vw + g, vx + vw + g + sw + g};
    for (int i = 0; i < 4; ++i)
        if (Button(Rectangle{rx[i], sy, sw, 34}, sl[i], dark)) ui.service = std::clamp(ui.service + sd[i], 1, 60);
    DrawRectangleRounded(Rectangle{vx, sy, vw, 34}, 0.3f, 8, Rgb(16, 19, 27));
    char vb[24];
    std::snprintf(vb, sizeof vb, "%d s", ui.service);
    TxtCentered(vb, vx + vw * 0.5f, sy + 7.f, 15.f, Rgb(240, 243, 250), true);

    Txt("QUANTITY", px + 16, py + 176, 11, grey, true);
    const float qy = py + 194.f;
    if (Button(Rectangle{x0, qy, 46, 34}, "-", dark)) ui.qty = std::max(1, ui.qty - 1);
    DrawRectangleRounded(Rectangle{x0 + 52.f, qy, 60, 34}, 0.3f, 8, Rgb(16, 19, 27));
    std::snprintf(vb, sizeof vb, "%d", ui.qty);
    TxtCentered(vb, x0 + 82.f, qy + 7.f, 15.f, Rgb(240, 243, 250), true);
    if (Button(Rectangle{x0 + 118.f, qy, 46, 34}, "+", dark)) ui.qty = std::min(10, ui.qty + 1);
    if (Button(Rectangle{x0 + 176.f, qy, 112, 34}, "Add to queue", Rgb(34, 128, 84), 12.5f))
        for (int i = 0; i < ui.qty; ++i) eng.addCustomer(ui.priority, (double)ui.service);

    if (Button(Rectangle{x0, py + 240.f, 288, 30}, "Add random customer", Rgb(54, 60, 78), 12.5f)) AddRandomCustomer(eng);
}


// ============================================================ settings window
Rectangle SettingsRect(int W, int H) {
    const float availW = (float)W - lay::kPanelW;
    const float w = 480.f;
    const float h = std::min(540.f, (float)H - lay::kTopH - lay::kDockH - 14.f);
    return Rectangle{(availW - w) * 0.5f, lay::kTopH + 8.f, w, h};
}

// One settings row: label on the left, [-] value [+] on the right. Shift+click = 5 steps. Returns true if changed.
bool Stepper(float x, float y, float w, const char* label, double& v, double step, double lo, double hi,
             const char* fmt, const char* zeroText = nullptr) {
    Txt(label, x, y + 6.f, 12.5f, Rgb(205, 212, 230));
    const float bw = 30.f, vw = 100.f, g = 4.f;
    const float rx = x + w - (bw * 2.f + vw + g * 2.f);
    const double mult = (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) ? 5.0 : 1.0;
    bool changed = false;
    if (Button(Rectangle{rx, y, bw, 25.f}, "-", Rgb(44, 50, 66))) {
        v = std::clamp(std::round((v - step * mult) / step) * step, lo, hi);
        changed = true;
    }
    DrawRectangleRounded(Rectangle{rx + bw + g, y, vw, 25.f}, 0.3f, 8, Rgb(16, 19, 27));
    char b[40];
    if (zeroText && v < 1e-9) std::snprintf(b, sizeof b, "%s", zeroText);
    else std::snprintf(b, sizeof b, fmt, v);
    TxtCentered(b, rx + bw + g + vw * 0.5f, y + 4.f, 13.f, Rgb(240, 243, 250), true);
    if (Button(Rectangle{rx + bw + g + vw + g, y, bw, 25.f}, "+", Rgb(44, 50, 66))) {
        v = std::clamp(std::round((v + step * mult) / step) * step, lo, hi);
        changed = true;
    }
    return changed;
}

void DrawSettings(qube::Engine& eng, Controls& ui, int W, int H) {
    if (!ui.settingsOpen) return;
    gInModal = true;
    const Rectangle r = SettingsRect(W, H);
    DrawRectangleRounded(Rectangle{r.x - 1, r.y - 1, r.width + 2, r.height + 2}, 0.03f, 10, Rgb(66, 118, 235));
    DrawRectangleRounded(r, 0.03f, 10, Rgb(22, 26, 36, 250));
    Txt("SETTINGS", r.x + 20, r.y + 14, 14, Rgb(240, 243, 250), true);
    if (Button(Rectangle{r.x + r.width - 44.f, r.y + 10.f, 26, 26}, "x", Rgb(54, 60, 78), 12.f)) ui.settingsOpen = false;

    const float x = r.x + 20.f, w = r.width - 40.f, rh = 27.f;
    float y = r.y + 46.f;
    bool changed = false;
    const int oldCounters = gSet.counters;

    auto section = [&](const char* t) {
        Txt(t, x, y, 11.5f, Rgb(120, 130, 152), true);
        DrawRectangle((int)x, (int)y + 16, (int)w, 1, Rgb(46, 52, 68));
        y += 21.f;
    };
    auto rowD = [&](const char* label, double& v, double step, double lo, double hi, const char* fmt,
                    const char* zero = nullptr) {
        if (Stepper(x, y, w, label, v, step, lo, hi, fmt, zero)) changed = true;
        y += rh;
    };
    auto rowF = [&](const char* label, float& v, double step, double lo, double hi, const char* fmt,
                    const char* zero = nullptr) {
        double d = v;
        rowD(label, d, step, lo, hi, fmt, zero);
        v = (float)d;
    };
    auto rowI = [&](const char* label, int& v, double step, double lo, double hi, const char* fmt) {
        double d = v;
        rowD(label, d, step, lo, hi, fmt);
        v = (int)std::lround(d);
    };

    section("ARRIVALS (random customers)");
    rowF("Average arrival interval", gSet.arrivalMean, 0.5, 1.0, 20.0, "%.1f s");
    rowF("Rush every (average)", gSet.rushEvery, 5.0, 0.0, 300.0, "%.0f s", "Off");
    rowI("Rush size", gSet.rushSize, 1.0, 2.0, 20.0, "%.0f people");
    rowI("Service time - shortest", gSet.svcMin, 1.0, 1.0, 60.0, "%.0f s");
    rowI("Service time - longest", gSet.svcMax, 1.0, 1.0, 60.0, "%.0f s");
    rowI("Share of P3 (urgent)", gSet.p3Share, 5.0, 0.0, 100.0, "%.0f %%");
    rowI("Share of P2 (high)", gSet.p2Share, 5.0, 0.0, 100.0, "%.0f %%");
    {
        char t[64];
        std::snprintf(t, sizeof t, "P1 (normal) gets the remaining %d %% of arrivals",
                      std::max(0, 100 - gSet.p3Share - gSet.p2Share));
        Txt(t, x, y + 1.f, 11.5f, Rgb(140, 150, 170));
        y += 22.f;
    }

    section("SCHEDULING");
    rowD("Aging override wait", gSet.agingOverride, 1.0, 0.0, 300.0, "%.0f s");
    rowD("Aging interval (per priority level)", gSet.agingInterval, 5.0, 1.0, 120.0, "%.0f s");
    rowD("Starvation threshold", gSet.starvation, 10.0, 5.0, 600.0, "%.0f s");

    section("COUNTERS");
    rowI("Number of counters (restarts the run)", gSet.counters, 1.0, 2.0, 6.0, "%.0f");
    rowD("Walk-to-desk time", gSet.approach, 0.5, 0.5, 10.0, "%.1f s");

    if (changed) {
        ClampSettings();
        ApplySettings(eng);
        if (gSet.counters != oldCounters) {
            eng.setCounterCount(gSet.counters);
            ui.resetRequested = true;
        }
        SaveSettings();
    }

    Txt("Shift+click changes by 5 steps. Settings are saved automatically.", x, r.y + r.height - 76.f, 11.f,
        Rgb(130, 140, 160));
    if (Button(Rectangle{x, r.y + r.height - 46.f, 150, 30}, "Reset to defaults", Rgb(54, 60, 78), 12.5f)) {
        gSet = Settings{};
        ClampSettings();
        ApplySettings(eng);
        if (gSet.counters != oldCounters) {
            eng.setCounterCount(gSet.counters);
            ui.resetRequested = true;
        }
        SaveSettings();
    }
    if (Button(Rectangle{r.x + r.width - 20.f - 100.f, r.y + r.height - 46.f, 100, 30}, "Close", Rgb(48, 86, 170), 12.5f))
        ui.settingsOpen = false;
    gInModal = false;
}

} // namespace

// ============================================================ main
int main() {
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(1280, 720, "QUBE - Queue-Based Unified Balancing Engine");
    SetWindowMinSize(1200, 720);
    SetTargetFPS(60);
    LoadFonts();

    LoadSettings(); // defaults if no qube_settings.txt yet
    qube::Config cfg;
    cfg.counterCount = gSet.counters;
    cfg.approachTime = gSet.approach;
    cfg.policy = qube::Policy::Aging;
    cfg.agingInterval = gSet.agingInterval;
    cfg.starvationThreshold = gSet.starvation;
    cfg.homeClasses = {3, 2, 1, 1}; // counter 1 = P3, counter 2 = P2, counters 3-4 = P1 (click a badge to change)
    cfg.agingOverrideWait = gSet.agingOverride;
    qube::Engine eng(cfg);
    SeedScene(eng);

    std::unordered_map<int, Actor> actors;
    int nextId = 1;
    Controls ui;
    float spawnClock = 2.f, burstClock = 25.f, histClock = 0.f;
    std::vector<float> qHist;

    const Vector3 camOffset{30.f, 26.f, 30.f};
    gToCam = Vector3Normalize(camOffset);
    gLight = Vector3Normalize(Vector3{0.15f, 0.9f, 0.5f});

    while (!WindowShouldClose()) {
        const float wallDt = std::min(GetFrameTime(), 0.05f);
        gMouse = GetMousePosition();
        gClick = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        gDown = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
        gHoverAny = false;
        gModalOn = ui.settingsOpen;
        gModal = SettingsRect(GetScreenWidth(), GetScreenHeight());

        // ---- keyboard shortcuts (the dock buttons do the same things)
        if (IsKeyPressed(KEY_SPACE)) ui.toggleRun();
        if (IsKeyPressed(KEY_UP) && ui.speedIdx < 5) ++ui.speedIdx;
        if (IsKeyPressed(KEY_DOWN) && ui.speedIdx > 0) --ui.speedIdx;
        if (IsKeyPressed(KEY_G)) ui.autoSpawn = !ui.autoSpawn;
        if (IsKeyPressed(KEY_TAB)) ui.panelTab = 1 - ui.panelTab;
        if (IsKeyPressed(KEY_S)) ui.settingsOpen = !ui.settingsOpen;
        if (IsKeyPressed(KEY_ONE)) eng.setPolicy(qube::Policy::FCFS);
        if (IsKeyPressed(KEY_TWO)) eng.setPolicy(qube::Policy::Priority);
        if (IsKeyPressed(KEY_THREE)) eng.setPolicy(qube::Policy::SJF);
        if (IsKeyPressed(KEY_FOUR)) eng.setPolicy(qube::Policy::Aging);
        if (IsKeyPressed(KEY_A)) AddRandomCustomer(eng);
        if (IsKeyPressed(KEY_B))
            for (int i = 0; i < gSet.rushSize; ++i) AddRandomCustomer(eng); // manual rush
        if (IsKeyPressed(KEY_F)) ToggleBorderlessWindowed();
        if (IsKeyPressed(KEY_R)) ui.resetRequested = true;

        if (ui.resetRequested) {
            ui.resetRequested = false;
            eng.reset();
            actors.clear();
            nextId = 1;
            SeedScene(eng);
            ApplySettings(eng);
            spawnClock = 2.f;
            burstClock = 25.f;
            histClock = 0.f;
            qHist.clear();
            ui.run = RunState::Ready;
        }

        // ---- simulate (frozen while Ready or Paused)
        const bool running = ui.run == RunState::Running;
        const float dt = running ? wallDt * kSpeeds[ui.speedIdx] : 0.f;
        eng.step(dt);
        const qube::Stats st = eng.stats(); // computed once per frame
        histClock += dt;
        while (histClock >= 2.f) { // sample queue length every 2 simulated seconds
            histClock -= 2.f;
            qHist.push_back((float)st.waiting);
            if (qHist.size() > 150) qHist.erase(qHist.begin());
        }
        // Arrival load comes from the Settings window (default: a customer every ~5.5 s plus a rush of ~6
        // people every ~45 s), which keeps the counters busy so queues form and clear.
        // Arrivals pause while any lane is full so people never stack up.
        const bool roomInLanes = eng.lane(1).size() < (size_t)LaneCapacity(1) && eng.lane(2).size() < (size_t)LaneCapacity(2) &&
                                 eng.lane(3).size() < (size_t)LaneCapacity(3);
        if (running && ui.autoSpawn && roomInLanes) {
            spawnClock -= dt;
            if (spawnClock <= 0.f) {
                AddRandomCustomer(eng);
                spawnClock = Rand(0.6f * gSet.arrivalMean, 1.4f * gSet.arrivalMean);
            }
            if (gSet.rushEvery > 0.f) {
                burstClock -= dt;
                if (burstClock <= 0.f) {
                    const int n = std::max(1, (int)std::lround(gSet.rushSize + Rand(-1.5f, 1.5f)));
                    for (int i = 0; i < n; ++i) AddRandomCustomer(eng);
                    burstClock = Rand(0.7f * gSet.rushEvery, 1.3f * gSet.rushEvery);
                }
            }
        }
        UpdateActors(eng, actors, nextId, dt, wallDt);

        // ---- camera: orthographic isometric, centred in the free area (not under HUD/dock)
        const int W = GetScreenWidth(), H = GetScreenHeight();
        const float availW = (float)W - lay::kPanelW, availH = (float)H - lay::kTopH - lay::kDockH;
        const float fovy = std::max(24.f * H / availH, 34.f * H / availW);
        const float wpp = fovy / (float)H; // world units per pixel
        Vector3 target{0.f, 1.f, 1.f};
        const Vector3 fwd = Vector3Normalize(Vector3Negate(camOffset));
        const Vector3 right = Vector3Normalize(Vector3CrossProduct(fwd, Vector3{0, 1, 0}));
        const Vector3 up = Vector3CrossProduct(right, fwd);
        target = Vector3Add(target, Vector3Scale(right, lay::kPanelW * 0.5f * wpp));
        target = Vector3Add(target, Vector3Scale(up, (lay::kTopH - lay::kDockH) * 0.5f * wpp));

        Camera3D cam{};
        cam.target = target;
        cam.position = Vector3Add(target, camOffset);
        cam.up = Vector3{0.f, 1.f, 0.f};
        cam.fovy = fovy;
        cam.projection = CAMERA_ORTHOGRAPHIC;

        // ---- draw
        BeginDrawing();
        ClearBackground(Rgb(14, 16, 22));
        BeginMode3D(cam);
        rlDisableBackfaceCulling();
        DrawScene(eng, actors);
        EndMode3D();
        DrawSceneLabels(eng, actors, cam);
        DrawLegend();
        DrawReadyBanner(ui.run, W);
        DrawTopBar(eng, kSpeeds[ui.speedIdx], ui.run, ui.autoSpawn, ui.settingsOpen, W);
        DrawPanel(eng, st, ui.panelTab, qHist, W, H);
        DrawControls(eng, ui, W, H);
        DrawSettings(eng, ui, W, H);
        SetMouseCursor(gHoverAny ? MOUSE_CURSOR_POINTING_HAND : MOUSE_CURSOR_DEFAULT);
        EndDrawing();
    }

    CloseWindow();
    return 0;
}
