// NFSW.HUDScale.asi - Scales NFS World's Scaleform (GFx 3.x) HUD/menus for high resolutions.
//
// NFS World renders every HUD element / menu as a separate GFx movie with its own
// viewport sized in physical pixels, so at 1440p/4K everything stays 1080p-sized.
// This plugin hooks GFxMovieRoot::SetViewport/GetViewport (shared vtable) and:
//   * full-screen NoScale movies  -> GViewport.Scale /= s (stage shrinks, content grows)
//   * sub-rect "window" movies    -> rect grows by s, re-anchored to nearest screen edge
//                                    (+ Scale /= s when the movie is NoScale)
// GetViewport returns the game's original (unscaled) rect so its layout math is untouched.
//
// Addresses below are for nfsw.exe SHA-256 b2c616c6...c434ec819 (Soapbox build).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <intrin.h>
#include <map>
#include <set>
#include <string>
#include <vector>

struct GViewport {
    int   BufferWidth, BufferHeight;
    int   Left, Top;
    int   Width, Height;
    int   ScissorLeft, ScissorTop, ScissorWidth, ScissorHeight;
    float Scale;
    float AspectRatio;
    unsigned Flags;
};
static_assert(sizeof(GViewport) == 0x34, "GViewport layout");

enum { View_UseScissorRect = 0x4 };
enum { SM_NoScale = 0, SM_ShowAll = 1, SM_ExactFit = 2, SM_NoBorder = 3 };

// --- nfsw.exe layout (preferred base 0x400000) ---
static const DWORD kImageBase       = 0x00400000;
static const DWORD kMovieRootVtbl   = 0x00C3CAF0;
static const DWORD kSlotSetViewport = 0x64 / 4;
static const DWORD kSlotGetViewport = 0x68 / 4;
static const DWORD kOrigSetViewport = 0x00A1DB40;
static const DWORD kOrigGetViewport = 0x00A12090;
static const DWORD kOffViewport     = 0x78;   // GViewport inside GFxMovieRoot
static const DWORD kOffScaleMode    = 0xC0;   // GFxMovieView::ScaleModeType
static const DWORD kOffMovieDef     = 0x6C;   // used to read native stage size for logging
// GetViewport callers that want the on-screen rect (mouse routing), not the game's layout rect.
// 0x51B671: window message handler - subtracts Left/Top from WM_MOUSE* coords and hit-tests the rect.
// 0x482057: "is cursor over this movie" - (cursor - Left/Top) -> GFxMovieView::HitTest.
static const DWORD kScreenRectCallers[] = { 0x0051B671, 0x00482057 };
static DWORD gDelta;

typedef void(__thiscall* SetViewport_t)(void* self, const GViewport* vp);
typedef void(__thiscall* GetViewport_t)(void* self, GViewport* vp);
static SetViewport_t oSetViewport;
static GetViewport_t oGetViewport;

// --- config ---
static float gUIScale      = 0.0f;   // 0 = auto (BufferHeight / BaseHeight)
static int   gBaseHeight   = 1080;
static int   gScaleFull    = 1;      // full-screen NoScale movies
static int   gScaleWindows = 1;      // sub-rect movies
static int   gScaleTopBar  = 1;      // CommonHUD.gfx (top button bar)
static int   gScalePowerups = 1;     // powerup.gfx (power-up wheel)
static float gMinScale     = 1.0f;
static int   gLog          = 1;
struct Size { int w, h; };
static std::vector<Size> gExclude;   // native viewport sizes (game's WxH) to leave alone
static std::vector<Size> gExcludeStages;  // movie stage sizes (as logged stage=WxH) to leave alone
static std::vector<Size> gContentOnlyStages;  // stages scaled in place (rect untouched)
static std::vector<Size> gPinnedStages;       // stages enlarged at the game's placement (tooltips)
// GFX file names (without .gfx) - matched case-insensitively against CreateMovie paths
static std::vector<std::string> gExcludeMovies, gTooltipMovies, gTopLeftMovies, gInPlaceMovies, gContentOnlyMovies;
static int gScaleNameBadges = 0;              // in-place scaling for PlayerNameBadge
static int gRememberPositions = 1;            // persist dragged window positions
static int gHideCursor = 0;                   // hide the cursor in-game after inactivity
static float gHideCursorDelay = 5.0f;         // seconds
static std::vector<std::string> gHudMovies;   // movies that don't count as "menu open"
static const char kDefaultHudMovies[] =
    "InstrumentCluster,CommonHud,flashers,powerup,PowerupBuffBar,BuffBar,ChatGadget,GroupChatHUD,ChatGroupSelect,"
    "TreasureHud,DriveIndicator,RaceGadget,Tooltip,PlayerNameBadge,_Overlay,MeetingPlaceHUD,DragRaceHUD,"
    "SprintRankingIndicator,CircuitRankingIndicator,PursuitIndicator,TeamEscapePursuitIndicator,"
    "PursuitOutrunRankingIndicator,MultiplayerPursuitHUD,TrackerHUD,TickerGadget,RaceCountdownTimer,"
    "LevelUpFlasher,Font-ENG,Font-CHS,Font-CHT,Font-THA,Global,Common,IME,Dummy";
static float gTooltipScale;                   // pinned-stage scale relative to UI scale; 0/1 = same
static int   gFixMenuTooltips = 1;            // convert ShowTooltipAt coords from scaled menus
static Size gDebugStage;             // movie stage to log mouse routing for
static std::vector<std::string> gDebugMovies;  // movie names to log mouse routing for ("*" = all)
static std::set<DWORD> gScreenCallers;  // return addresses (preferred base) getting the applied rect
static std::set<DWORD> gSeenCallers;

// Real screen size = client area of the game's largest visible top-level window.
// (Movie BufferWidth/Height can't be trusted: some movies use private 1024x768 buffers.)
static BOOL CALLBACK FindGameWnd(HWND h, LPARAM lp) {
    DWORD pid; GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
    RECT r; if (!GetClientRect(h, &r)) return TRUE;
    SIZE* best = (SIZE*)lp;
    if ((r.right - r.left) * (r.bottom - r.top) > best->cx * best->cy) { best->cx = r.right - r.left; best->cy = r.bottom - r.top; }
    return TRUE;
}

static bool ScreenSize(int& w, int& h) {
    static SIZE cached; static DWORD last;
    DWORD now = GetTickCount();
    if (!last || now - last > 500) {
        SIZE best = { 0, 0 };
        EnumWindows(FindGameWnd, (LPARAM)&best);
        if (best.cx >= 640 && best.cy >= 480) cached = best;
        last = now;
    }
    w = cached.cx; h = cached.cy;
    return w > 0 && h > 0;
}

static FILE* gLogFile;
static int   gLogLines;
static CRITICAL_SECTION gLock;

// Per-axis game->screen position mapping. Kept fixed per movie (only rebuilt when its size or
// the screen changes) so it can be inverted when the game writes back an on-screen rect,
// e.g. while dragging a window.
struct AxisMap {
    int   cls;          // 0 = low-edge anchored, 1 = centred on itself, 2 = high-edge anchored, 3 = fixed (game's coordinate)
    int   buf, len, nlen;
    float s;
};

enum Policy { P_Default, P_Exclude, P_ContentOnly, P_Tooltip, P_TopLeft, P_InPlace };

struct MovieState {
    GViewport   orig;     // what the game asked for
    GViewport   applied;  // what we actually set
    bool        mapped;
    AxisMap     x, y;
    bool        resolved; // name/policy looked up
    std::string name;     // GFX file name without extension ("" if unknown)
    int         policy;
    bool        restored; // saved user position applied
};
static std::map<void*, MovieState> gMovies;

static void Log(const char* fmt, ...) {
    if (!gLog || !gLogFile || gLogLines > 20000) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(gLogFile, fmt, ap);
    va_end(ap);
    fputc('\n', gLogFile);
    fflush(gLogFile);
    ++gLogLines;
}

static int ScaleModeOf(void* self) { return *(int*)((char*)self + kOffScaleMode); }

// Native stage size in pixels (movie frame rect), for identifying movies in the log.
static bool StageSize(void* self, float& w, float& h) {
    __try {
        char* def = *(char**)((char*)self + kOffMovieDef);
        if (!def) return false;
        char* p = *(char**)(def + 0x1C); if (!p) return false;
        p = *(char**)(p + 0x0C);         if (!p) return false;
        p = *(char**)(p + 0x20);         if (!p) return false;
        float* r = (float*)(p + 0x30);
        w = (r[2] - r[0]) / 20.0f;
        h = (r[3] - r[1]) / 20.0f;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static float ScaleFor(int bufH) {
    float s = gUIScale;
    if (s <= 0.0f) s = (float)bufH / (float)gBaseHeight;
    if (s < gMinScale) s = gMinScale;
    return s;
}

// Anchor class from where the game placed the rect on this axis.
static AxisMap MakeAxis(int buf, int pos, int len, int nlen, float s) {
    int lo = pos, hi = buf - (pos + len);
    float t = (lo + hi) > 0 ? (float)lo / (float)(lo + hi) : 0.5f;
    AxisMap m = { t < 0.25f ? 0 : (t > 0.75f ? 2 : 1), buf, len, nlen, s };
    return m;
}

static int MapPos(const AxisMap& m, int pos) {
    float p;
    if (m.cls == 3)      p = (float)pos;
    else if (m.cls == 0) p = pos * m.s;
    else if (m.cls == 2) p = m.buf - (m.buf - pos - m.len) * m.s - m.nlen;
    else                 p = pos + m.len * 0.5f - m.nlen * 0.5f;
    return (int)lroundf(p);
}

static int UnmapPos(const AxisMap& m, int npos) {
    float p;
    if (m.cls == 3)      p = (float)npos;
    else if (m.cls == 0) p = npos / m.s;
    else if (m.cls == 2) p = m.buf - (m.buf - npos - m.nlen) / m.s - m.len;
    else                 p = npos - m.len * 0.5f + m.nlen * 0.5f;
    return (int)lroundf(p);
}

static int ClampPos(int pos, int len, int buf) {
    if (len > buf) return pos;
    if (pos < 0) return 0;
    if (pos + len > buf) return buf - len;
    return pos;
}

static bool Excluded(const GViewport& vp) {
    for (auto& e : gExclude)
        if (e.w == vp.Width && e.h == vp.Height) return true;
    return false;
}

static bool IsStage(void* self, int w, int h) {
    float sw, sh;
    return StageSize(self, sw, sh) && lroundf(sw) == w && lroundf(sh) == h;
}

// --- movie names (from GFxLoader::CreateMovie) ---
// Key = the movie data object both the loaded GFxMovieDef and every instance's def lead to
// (def+0x1C -> +0x0C -> +0x20, the same chain StageSize uses).
static std::map<void*, std::string> gMovieNames;
static CRITICAL_SECTION gNameLock;

static void* DataKey(char* def) {
    __try {
        if (!def) return nullptr;
        char* p = *(char**)(def + 0x1C); if (!p) return nullptr;
        p = *(char**)(p + 0x0C);         if (!p) return nullptr;
        return *(void**)(p + 0x20);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static void* MovieDefOf(void* self) {
    __try { return *(void**)((char*)self + kOffMovieDef); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static std::string MovieName(void* self) {
    void* def = MovieDefOf(self);
    if (!def) return "";
    void* key = DataKey((char*)def);
    std::string n;
    EnterCriticalSection(&gNameLock);
    auto it = gMovieNames.find(def);
    if (it == gMovieNames.end() && key) it = gMovieNames.find(key);
    if (it != gMovieNames.end()) n = it->second;
    LeaveCriticalSection(&gNameLock);
    return n;
}

// Stage sizes of movies with a unique size, used to match names only if the CreateMovie hook
// failed (no names at all). Ambiguous sizes (e.g. DropdownMenu's 550x400) are not listed.
static bool gNamesActive;
static const struct { const char* name; int w, h; } kKnownStages[] = {
    { "WorldMap", 1089, 687 }, { "Tooltip", 214, 36 }, { "Tooltip_powerupManagement", 200, 150 },
    { "CommonHUD", 1024, 1024 }, { "powerup", 180, 180 }, { "PlayerNameBadge", 256, 32 },
};

static bool IsMovie(void* self, const std::string& name, const char* want) {
    if (gNamesActive) return !name.empty() && _stricmp(name.c_str(), want) == 0;
    for (auto& k : kKnownStages)
        if (_stricmp(k.name, want) == 0) return IsStage(self, k.w, k.h);
    return false;
}

static bool InList(void* self, const std::vector<std::string>& list, const std::string& name) {
    for (auto& e : list) if (IsMovie(self, name, e.c_str())) return true;
    return false;
}

static int PolicyFor(void* self, const std::string& name) {
    if (InList(self, gExcludeMovies, name)) return P_Exclude;
    for (auto& e : gExcludeStages) if (IsStage(self, e.w, e.h)) return P_Exclude;
    if (InList(self, gTooltipMovies, name)) return P_Tooltip;
    for (auto& e : gPinnedStages) if (IsStage(self, e.w, e.h)) return P_Tooltip;
    if (InList(self, gTopLeftMovies, name)) return P_TopLeft;
    if (gScaleNameBadges && InList(self, gInPlaceMovies, name)) return P_InPlace;
    if (InList(self, gContentOnlyMovies, name)) return P_ContentOnly;
    for (auto& e : gContentOnlyStages) if (IsStage(self, e.w, e.h)) return P_ContentOnly;
    return P_Default;
}

static const char* const kPolicyName[] = { "", "excluded", "content-only", "tooltip", "top-left", "in-place" };

static GViewport Transform(void* self, const GViewport& in, MovieState& st, const char** kind) {
    GViewport out = in;
    *kind = "skip";
    if (in.BufferWidth <= 0 || in.BufferHeight <= 0 || in.Width <= 0 || in.Height <= 0) return out;
    if (Excluded(in)) { *kind = "excluded"; return out; }
    if (!st.resolved) {
        st.name = MovieName(self);
        // a nameless movie may be resolved later (names are recorded at load time)
        st.resolved = !st.name.empty();
    }
    int policy = st.policy = PolicyFor(self, st.name);
    if (policy == P_Exclude) { *kind = "excluded"; return out; }
    int mode = ScaleModeOf(self);
    int sw, sh;
    bool haveScreen = ScreenSize(sw, sh);

    // Content-only: leave the game's rect untouched (its own move/resize/mouse code stays
    // consistent) and only enlarge the Flash content.
    if (policy == P_ContentOnly) {
        float s = ScaleFor(haveScreen ? sh : in.BufferHeight);
        if (s <= 1.001f || mode != SM_NoScale) return out;
        out.Scale = in.Scale / s;
        *kind = "content-only";
        return out;
    }
    if (!gScaleTopBar   && IsMovie(self, st.name, "CommonHUD")) { *kind = "topbar-off";  return out; }
    if (!gScalePowerups && IsMovie(self, st.name, "powerup"))   { *kind = "powerup-off"; return out; }

    bool full = in.Left <= 0 && in.Top <= 0 &&
                in.Left + in.Width >= in.BufferWidth && in.Top + in.Height >= in.BufferHeight;
    if (full && policy == P_Default) {
        float s = ScaleFor(in.BufferHeight);
        if (s <= 1.001f || !gScaleFull || mode != SM_NoScale) return out;  // ShowAll etc. already fill the screen
        out.Scale = in.Scale / s;
        *kind = "full-noscale";
        return out;
    }

    if (!gScaleWindows) return out;
    if (policy != P_Default &&
        ((in.Left <= 0 && in.Left + in.Width >= in.BufferWidth) || (in.Top <= 0 && in.Top + in.Height >= in.BufferHeight)))
        return out;   // point-placed movies: ignore their full-width/height setup viewports
    // Movie buffers are often stale (old resolution, or the movie's own size like 180x180 while
    // it is placed on screen at [50,1010]); use the real back-buffer size unless the movie
    // genuinely renders into a small private target that contains its rect.
    int bw = in.BufferWidth, bh = in.BufferHeight;
    bool inside = in.Left >= 0 && in.Top >= 0 && in.Left + in.Width <= bw && in.Top + in.Height <= bh;
    if ((bw >= 640 && bh >= 480 || !inside || policy != P_Default) && haveScreen) { bw = sw; bh = sh; }
    float s = ScaleFor(bh);
    if (policy == P_Tooltip) s *= gTooltipScale > 0.0f ? gTooltipScale : 1.0f;
    if (s <= 1.001f) return out;

    // Never grow a window past the screen - except along an axis the game already stretched
    // across the whole screen (e.g. CommonHUD's 2560x1024 strip): for NoScale movies content
    // scales through GViewport.Scale, so that axis is simply clipped to the screen.
    bool spanW = mode == SM_NoScale && in.Left <= 0 && in.Width >= bw;
    bool spanH = mode == SM_NoScale && in.Top <= 0 && in.Height >= bh;
    float fit = 1e9f;  // strips: full scale, both axes clipped to the screen below
    if (!spanW && !spanH) fit = min((float)bw / in.Width, (float)bh / in.Height);
    float se = min(s, max(1.0f, fit));
    if (se <= 1.001f) return out;
    int nw = (int)lroundf(in.Width * se), nh = (int)lroundf(in.Height * se);
    nw = min(nw, max(bw, in.Width));
    nh = min(nh, max(bh, in.Height));

    if (!st.mapped || st.x.buf != bw || st.y.buf != bh || st.x.len != in.Width || st.y.len != in.Height ||
        st.x.nlen != nw || st.y.nlen != nh || st.x.s != se) {
        // Rebuilt only when size/screen change, so a window's anchor never flips mid-drag.
        st.x = MakeAxis(bw, in.Left, in.Width, nw, se);
        st.y = MakeAxis(bh, in.Top, in.Height, nh, se);
        // Movies the game places at a point (cursor, hovered item, car) keep that point.
        if (policy == P_Tooltip) { st.x.cls = 1; st.y.cls = 3; }   // same centre x, same top
        if (policy == P_TopLeft) { st.x.cls = 3; st.y.cls = 3; }   // same top-left corner
        if (policy == P_InPlace) { st.x.cls = 1; st.y.cls = 1; }   // same centre
        st.mapped = true;
    }
    out.Width = nw; out.Height = nh;
    out.Left = ClampPos(MapPos(st.x, in.Left), nw, bw);
    out.Top  = ClampPos(MapPos(st.y, in.Top),  nh, bh);
    if (in.Flags & View_UseScissorRect) {
        int dx = out.Left - in.Left, dy = out.Top - in.Top;
        out.ScissorLeft   = in.Left + dx + (int)lroundf((in.ScissorLeft - in.Left) * se);
        out.ScissorTop    = in.Top  + dy + (int)lroundf((in.ScissorTop  - in.Top)  * se);
        out.ScissorWidth  = (int)lroundf(in.ScissorWidth  * se);
        out.ScissorHeight = (int)lroundf(in.ScissorHeight * se);
    }
    if (mode == SM_NoScale) out.Scale = in.Scale / se;
    *kind = policy != P_Default ? kPolicyName[policy] : (mode == SM_NoScale ? "window-noscale" : "window");
    return out;
}

static bool SameVp(const GViewport& a, const GViewport& b) { return memcmp(&a, &b, sizeof(a)) == 0; }

// --- remembered window positions (game coordinates, per movie name) ---
struct SavedPos { int left, top, width, height; };
static std::map<std::string, SavedPos> gSavedPos;
static char  gPosPath[MAX_PATH];
static bool  gPosDirty;
static DWORD gPosLastWrite;

static void LoadPositions() {
    char keys[8192];
    DWORD n = GetPrivateProfileStringA("Positions", nullptr, "", keys, sizeof(keys), gPosPath);
    for (char* k = keys; k < keys + n && *k; k += strlen(k) + 1) {
        char v[64]; SavedPos p;
        GetPrivateProfileStringA("Positions", k, "", v, sizeof(v), gPosPath);
        if (sscanf(v, "%d,%d,%d,%d", &p.left, &p.top, &p.width, &p.height) == 4) gSavedPos[k] = p;
    }
}

static void FlushPositions(bool force) {
    if (!gPosDirty || (!force && GetTickCount() - gPosLastWrite < 2000)) return;
    for (auto& kv : gSavedPos) {
        char v[64];
        sprintf(v, "%d,%d,%d,%d", kv.second.left, kv.second.top, kv.second.width, kv.second.height);
        WritePrivateProfileStringA("Positions", kv.first.c_str(), v, gPosPath);
    }
    gPosDirty = false; gPosLastWrite = GetTickCount();
}

static void __fastcall hkSetViewport(void* self, void*, const GViewport* vp) {
    EnterCriticalSection(&gLock);
    GViewport orig = *vp;
    auto it = gMovies.find(self);
    bool known = it != gMovies.end();
    MovieState st = known ? it->second : MovieState{};
    GViewport prevOrig = st.orig, prevApplied = st.applied;
    bool scaled = known && (prevApplied.Width != prevOrig.Width || prevApplied.Height != prevOrig.Height ||
                            prevApplied.Scale != prevOrig.Scale);
    const char* via = "";
    if (scaled && SameVp(*vp, prevApplied)) {
        // GFx re-applying its stored (already transformed) viewport, e.g. from SetViewScaleMode.
        orig = prevOrig;
    } else if (scaled && !st.mapped && vp->Scale == prevApplied.Scale &&
               prevApplied.Width == prevOrig.Width && prevApplied.Height == prevOrig.Height) {
        // Content-only movie: rect is the game's own, only Scale was ours - restore it.
        orig.Scale = prevOrig.Scale;
        via = " (screen write)";
    } else if (scaled && st.mapped && vp->Width == prevApplied.Width && vp->Height == prevApplied.Height &&
               vp->Scale == prevApplied.Scale) {
        // The game wrote back an on-screen rect it read through a screen-rect caller with a new
        // position (window drag). Convert the position back to game space instead of re-scaling.
        orig = prevOrig;
        orig.Left = UnmapPos(st.x, vp->Left);
        orig.Top  = UnmapPos(st.y, vp->Top);
        orig.BufferWidth = vp->BufferWidth; orig.BufferHeight = vp->BufferHeight; orig.Flags = vp->Flags;
        via = " (screen write)";
    }

    if (!st.resolved) { st.name = MovieName(self); st.resolved = !st.name.empty(); }
    if (gRememberPositions && !st.name.empty()) {
        bool windowLike = orig.BufferWidth >= 640 && orig.BufferHeight >= 480 &&
                          !(orig.Left <= 0 && orig.Top <= 0 && orig.Width >= orig.BufferWidth && orig.Height >= orig.BufferHeight);
        int pol = PolicyFor(self, st.name);
        if (windowLike && pol == P_Default) {
            if (*via) {
                // user dragged it: remember where (game coordinates)
                gSavedPos[st.name] = { orig.Left, orig.Top, orig.Width, orig.Height };
                gPosDirty = true;
                st.restored = true;
            } else if (!st.restored) {
                auto sp = gSavedPos.find(st.name);
                if (sp != gSavedPos.end() && sp->second.width == orig.Width && sp->second.height == orig.Height) {
                    orig.Left = sp->second.left; orig.Top = sp->second.top;
                    via = " (restored position)";
                }
                st.restored = true;
            }
        }
        FlushPositions(false);
    }

    const char* kind;
    GViewport out = Transform(self, orig, st, &kind);

    bool changed = !known || !SameVp(prevOrig, orig) || !SameVp(prevApplied, out);
    st.orig = orig; st.applied = out;
    gMovies[self] = st;
    if (changed && gLog) {
        float sw = 0, sh = 0; StageSize(self, sw, sh);
        int scw = 0, sch = 0; ScreenSize(scw, sch);
        Log("%p %s screen=%dx%d stage=%.0fx%.0f mode=%d buf=%dx%d game=[%d,%d %dx%d] scale=%.3f flags=%x -> %s%s [%d,%d %dx%d] scale=%.3f",
            self, st.name.empty() ? "?" : st.name.c_str(), scw, sch, sw, sh, ScaleModeOf(self), orig.BufferWidth, orig.BufferHeight,
            orig.Left, orig.Top, orig.Width, orig.Height, orig.Scale, orig.Flags,
            kind, via, out.Left, out.Top, out.Width, out.Height, out.Scale);
    }
    LeaveCriticalSection(&gLock);
    oSetViewport(self, &out);
}

static void __fastcall hkGetViewport(void* self, void*, GViewport* vp) {
    DWORD caller = (DWORD)_ReturnAddress() - gDelta;
    oGetViewport(self, vp);
    EnterCriticalSection(&gLock);
    bool screen = gScreenCallers.count(caller) != 0;
    if (gLog && gSeenCallers.insert(caller).second)
        Log("GetViewport caller %08X (%s)", caller, screen ? "gets on-screen rect" : "gets game rect");
    auto it = gMovies.find(self);
    if (it != gMovies.end()) {
        if (!SameVp(*vp, it->second.applied)) gMovies.erase(it);  // movie recycled or changed behind our back
        else if (!screen) *vp = it->second.orig;
    }
    LeaveCriticalSection(&gLock);
}

// GetPrivateProfileString keeps inline "; comments" - drop them.
static void ReadStr(const char* key, const char* def, char* buf, DWORD size, const char* ini) {
    GetPrivateProfileStringA("MAIN", key, def, buf, size, ini);
    if (char* c = strchr(buf, ';')) *c = 0;
}

static void LoadConfig(const char* ini) {
    char buf[1024];
    GetPrivateProfileStringA("MAIN", "UIScale", "0", buf, sizeof(buf), ini);       gUIScale = (float)atof(buf);
    gBaseHeight   = GetPrivateProfileIntA("MAIN", "BaseHeight", 1080, ini);
    GetPrivateProfileStringA("MAIN", "MinScale", "1.0", buf, sizeof(buf), ini);    gMinScale = (float)atof(buf);
    gScaleFull    = GetPrivateProfileIntA("MAIN", "ScaleFullscreenMovies", 1, ini);
    gScaleWindows = GetPrivateProfileIntA("MAIN", "ScaleWindowMovies", 1, ini);
    gScaleTopBar  = GetPrivateProfileIntA("MAIN", "ScaleTopBar", 1, ini);
    gScalePowerups = GetPrivateProfileIntA("MAIN", "ScalePowerupWheel", 1, ini);
    gLog          = GetPrivateProfileIntA("MAIN", "Log", 1, ini);
    if (gBaseHeight < 240) gBaseHeight = 1080;

    ReadStr("ExcludeSizes", "", buf, sizeof(buf), ini);
    for (char* tok = strtok(buf, ",; "); tok; tok = strtok(nullptr, ",; ")) {
        Size sz;
        if (sscanf(tok, "%dx%d", &sz.w, &sz.h) == 2) gExclude.push_back(sz);
    }

    ReadStr("ExcludeStages", "", buf, sizeof(buf), ini);
    for (char* tok = strtok(buf, ",; "); tok; tok = strtok(nullptr, ",; ")) {
        Size sz;
        if (sscanf(tok, "%dx%d", &sz.w, &sz.h) == 2) gExcludeStages.push_back(sz);
    }
    ReadStr("ContentOnlyStages", "", buf, sizeof(buf), ini);
    for (char* tok = strtok(buf, ",; "); tok; tok = strtok(nullptr, ",; ")) {
        Size sz;
        if (sscanf(tok, "%dx%d", &sz.w, &sz.h) == 2) gContentOnlyStages.push_back(sz);
    }
    gFixMenuTooltips = GetPrivateProfileIntA("MAIN", "FixMenuTooltips", 1, ini);
    gScaleNameBadges = GetPrivateProfileIntA("MAIN", "ScaleNameBadges", 0, ini);
    gRememberPositions = GetPrivateProfileIntA("MAIN", "RememberWindowPositions", 1, ini);
    auto names = [&](const char* key, const char* def, std::vector<std::string>& v) {
        ReadStr(key, def, buf, sizeof(buf), ini);
        for (char* tok = strtok(buf, ",; \t"); tok; tok = strtok(nullptr, ",; \t")) v.push_back(tok);
    };
    names("ExcludeMovies", "", gExcludeMovies);
    names("TooltipMovies", "Tooltip,Tooltip_powerupManagement", gTooltipMovies);
    names("TopLeftMovies", "ContextMenu,DropdownMenu", gTopLeftMovies);
    names("InPlaceMovies", "PlayerNameBadge", gInPlaceMovies);
    names("ContentOnlyMovies", "", gContentOnlyMovies);
    gHideCursor = GetPrivateProfileIntA("MAIN", "HideCursorInGame", 0, ini);
    ReadStr("HideCursorDelay", "5", buf, sizeof(buf), ini);  gHideCursorDelay = (float)atof(buf);
    if (gHideCursorDelay < 0.5f) gHideCursorDelay = 0.5f;
    names("HudMovies", kDefaultHudMovies, gHudMovies);
    ReadStr("TooltipScale", "1.0", buf, sizeof(buf), ini);  gTooltipScale = (float)atof(buf);
    ReadStr("PinnedStages", "", buf, sizeof(buf), ini);
    for (char* tok = strtok(buf, ",; "); tok; tok = strtok(nullptr, ",; ")) {
        Size sz;
        if (sscanf(tok, "%dx%d", &sz.w, &sz.h) == 2) gPinnedStages.push_back(sz);
    }
    names("DebugMouseMovies", "", gDebugMovies);
    ReadStr("DebugMouseStage", "", buf, sizeof(buf), ini);
    if (sscanf(buf, "%dx%d", &gDebugStage.w, &gDebugStage.h) != 2) gDebugStage = { 0, 0 };

    for (DWORD a : kScreenRectCallers) gScreenCallers.insert(a);
    ReadStr("ScreenRectCallers", "", buf, sizeof(buf), ini);
    for (char* tok = strtok(buf, ",; "); tok; tok = strtok(nullptr, ",; ")) {
        DWORD a = strtoul(tok, nullptr, 16);
        if (a) gScreenCallers.insert(a);
    }
}

// --- mouse diagnostics for one movie (DebugMouseStage = WxH) ---
static const DWORD kSlotHandleEvent = 0xA4 / 4, kSlotNotifyMouse = 0xAC / 4, kSlotHitTest = 0xB0 / 4;
typedef int (__thiscall* HandleEvent_t)(void*, const void*, int, int);
typedef void(__thiscall* NotifyMouse_t)(void*, float, float, unsigned, unsigned);
typedef bool(__thiscall* HitTest_t)(void*, float, float, int);
static HandleEvent_t oHandleEvent;
static NotifyMouse_t oNotifyMouse;
static HitTest_t     oHitTest;

// Debugged: DebugMouseStage (stage size) or DebugMouseMovies (names, "*" = every movie).
static bool Debugged(void* self) {
    if (gDebugStage.w > 0 && IsStage(self, gDebugStage.w, gDebugStage.h)) return true;
    if (gDebugMovies.empty()) return false;
    if (gDebugMovies[0] == "*") return true;
    return InList(self, gDebugMovies, MovieName(self));
}

static void LogCursor(void* self, const char* what) {
    POINT p; GetCursorPos(&p);
    HWND h = GetForegroundWindow(); if (h) ScreenToClient(h, &p);
    std::string name = MovieName(self);
    EnterCriticalSection(&gLock);
    struct Unlock { ~Unlock() { LeaveCriticalSection(&gLock); } } unlock;
    auto it = gMovies.find(self);
    if (it != gMovies.end()) {
        const GViewport& g = it->second.orig; const GViewport& a = it->second.applied;
        Log("  [mouse] %p %s %s cursor=%ld,%ld game=[%d,%d %dx%d] screen=[%d,%d %dx%d] scale=%.3f", self,
            name.empty() ? "?" : name.c_str(), what, p.x, p.y,
            g.Left, g.Top, g.Width, g.Height, a.Left, a.Top, a.Width, a.Height, a.Scale);
    } else Log("  [mouse] %p %s %s cursor=%ld,%ld", self, name.empty() ? "?" : name.c_str(), what, p.x, p.y);
}

// One log line per movie per left-button press (press = button went down since last frame).
static DWORD gPressId;
static bool  gPressLmb;
static std::map<void*, DWORD> gLoggedPress;
static bool NewPressFor(void* self) {
    bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (lmb && !gPressLmb) ++gPressId;
    gPressLmb = lmb;
    if (!lmb) return false;
    DWORD& seen = gLoggedPress[self];
    if (seen == gPressId) return false;
    seen = gPressId;
    return true;
}

static int __fastcall hkHandleEvent(void* self, void*, const void* ev, int a, int b) {
    int r = oHandleEvent(self, ev, a, b);
    const DWORD* d = (const DWORD*)ev;
    if ((d[0] == 2 || d[0] == 3) && Debugged(self)) {   // mouse down / up
        char what[160];
        sprintf(what, "HandleEvent %s x=%.1f y=%.1f -> %d", d[0] == 2 ? "DOWN" : "UP", *(const float*)&d[2], *(const float*)&d[3], r);
        LogCursor(self, what);
    }
    return r;
}

static void __fastcall hkNotifyMouse(void* self, void*, float x, float y, unsigned buttons, unsigned idx) {
    if (buttons && Debugged(self) && NewPressFor(self)) {
        char what[120]; sprintf(what, "NotifyMouseState x=%.1f y=%.1f buttons=%u", x, y, buttons);
        LogCursor(self, what);
    }
    oNotifyMouse(self, x, y, buttons, idx);
}

static bool __fastcall hkHitTest(void* self, void*, float x, float y, int type) {
    bool r = oHitTest(self, x, y, type);
    if (Debugged(self) && NewPressFor(self)) {
        char what[120]; sprintf(what, "HitTest x=%.1f y=%.1f type=%d -> %d", x, y, type, r);
        LogCursor(self, what);
    }
    return r;
}

// --- world map OnResize(width, height) ---
// The map lays out its frame/borders/buttons to the pixel size the game passes to its
// ActionScript OnResize. With content-only scaling the logical stage is window/s, so divide
// those arguments by s. Each call site is `push "OnResize"` (5 bytes) right after the two
// GFxValue number args were built at [ebp-0x40] / [ebp-0x30]; that push is replaced by a call
// to a stub that fixes the args and then performs the push itself.
static const DWORD kOnResizeName = 0x00B2DAF0;
static const DWORD kOnResizeSites[] = { 0x00485EAA, 0x00488D71, 0x00488E88 };
static DWORD gOnResizeNameVA;

struct GFxNumberValue { void* iface; DWORD type; double num; };  // type 3 = number

static void __cdecl FixOnResizeArgs(char* frame) {
    int sw, sh;
    float s = ScaleFor(ScreenSize(sw, sh) ? sh : 1080);
    if (s <= 1.001f) return;
    GFxNumberValue* args = (GFxNumberValue*)(frame - 0x40);
    if (args[0].type != 3 || args[1].type != 3) return;
    if (gLog && gLogLines < 20000)
        Log("map OnResize %.0fx%.0f -> %.0fx%.0f", args[0].num, args[1].num, args[0].num / s, args[1].num / s);
    args[0].num /= s;
    args[1].num /= s;
}

static __declspec(naked) void OnResizeStub() {
    __asm {
        pushad
        pushfd
        push ebp
        call FixOnResizeArgs
        add esp, 4
        popfd
        popad
        // emulate the replaced `push "OnResize"` and return past it
        push eax
        push ecx
        mov ecx, gOnResizeNameVA
        mov eax, [esp + 8]
        mov [esp + 8], ecx
        pop ecx
        xchg eax, [esp]
        ret
    }
}

static bool PatchOnResize(DWORD delta) {
    gOnResizeNameVA = kOnResizeName + delta;
    for (DWORD site : kOnResizeSites) {
        BYTE* p = (BYTE*)(site + delta);
        if (p[0] != 0x68 || *(DWORD*)(p + 1) != gOnResizeNameVA) return false;
    }
    for (DWORD site : kOnResizeSites) {
        BYTE* p = (BYTE*)(site + delta);
        DWORD old;
        VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
        p[0] = 0xE8;
        *(DWORD*)(p + 1) = (DWORD)OnResizeStub - (DWORD)(p + 5);
        VirtualProtect(p, 5, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    return true;
}

// --- menu tooltips: ActionScript ShowTooltipAt(x, y, text, flag) ---
// Menus pass the tooltip position in their own stage coordinates; the game uses them as screen
// pixels (true only for an unscaled movie). Convert with the calling movie's GFx stage->pixel
// transform (the fields GFx itself uses in NotifyMouseState: stage = px * ViewScale + Offset).
static const DWORD kShowTooltipAt = 0x004C34A0;   // cdecl handler(movie, args, nargs)
static const BYTE  kShowTooltipAtPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x1C };
static const DWORD kOffViewScaleX = 0xB0, kOffViewScaleY = 0xB4, kOffViewOffX = 0xB8, kOffViewOffY = 0xBC;
typedef void(__cdecl* ShowTooltipAt_t)(void*, GFxNumberValue*, unsigned);
static ShowTooltipAt_t oShowTooltipAt;
static DWORD gMovieVtbl;

// The handler's first argument is the calling movie or an object holding it; find the GFx movie.
static char* FindMovie(void* ctx) {
    if (!ctx) return nullptr;
    if (*(DWORD*)ctx == gMovieVtbl) return (char*)ctx;
    for (int i = 0; i < 0x40; ++i) {
        char* p = ((char**)ctx)[i];
        if ((DWORD)p > 0x10000 && !IsBadReadPtr(p, 4) && *(DWORD*)p == gMovieVtbl) return p;
    }
    return nullptr;
}

static void __cdecl hkShowTooltipAt(void* ctx, GFxNumberValue* args, unsigned nargs) {
    static int logged;
    __try {
        char* m = FindMovie(ctx);
        if (gLog && logged < 40) {
            ++logged;
            Log("ShowTooltipAt call: ctx=%p movie=%p nargs=%u x=%.1f(t%u) y=%.1f(t%u) view=%.3f,%.3f off=%.1f,%.1f",
                ctx, m, nargs, nargs > 0 ? args[0].num : 0, nargs > 0 ? args[0].type : 0,
                nargs > 1 ? args[1].num : 0, nargs > 1 ? args[1].type : 0,
                m ? *(float*)(m + kOffViewScaleX) : 0, m ? *(float*)(m + kOffViewScaleY) : 0,
                m ? *(float*)(m + kOffViewOffX) : 0, m ? *(float*)(m + kOffViewOffY) : 0);
        }
        if (m && nargs >= 2 && (args[0].type & 0x8F) == 3 && (args[1].type & 0x8F) == 3) {
            float sx = *(float*)(m + kOffViewScaleX), sy = *(float*)(m + kOffViewScaleY);
            float ox = *(float*)(m + kOffViewOffX),   oy = *(float*)(m + kOffViewOffY);
            if (sx > 0.0f && sy > 0.0f && (sx != 1.0f || sy != 1.0f)) {
                double x = (args[0].num - ox) / sx, y = (args[1].num - oy) / sy;
                if (gLog && gLogLines < 20000)
                    Log("ShowTooltipAt %.0f,%.0f -> %.0f,%.0f (view scale %.3f,%.3f off %.1f,%.1f)",
                        args[0].num, args[1].num, x, y, sx, sy, ox, oy);
                args[0].num = x; args[1].num = y;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    oShowTooltipAt(ctx, args, nargs);
}

static bool HookShowTooltipAt(DWORD delta) {
    BYTE* fn = (BYTE*)(kShowTooltipAt + delta);
    if (memcmp(fn, kShowTooltipAtPrologue, sizeof(kShowTooltipAtPrologue)) != 0) return false;
    // trampoline: original 6-byte prologue + jmp back
    BYTE* tr = (BYTE*)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return false;
    memcpy(tr, kShowTooltipAtPrologue, 6);
    tr[6] = 0xE9; *(DWORD*)(tr + 7) = (DWORD)(fn + 6) - (DWORD)(tr + 11);
    oShowTooltipAt = (ShowTooltipAt_t)tr;
    DWORD old;
    VirtualProtect(fn, 6, PAGE_EXECUTE_READWRITE, &old);
    fn[0] = 0xE9; *(DWORD*)(fn + 1) = (DWORD)hkShowTooltipAt - (DWORD)(fn + 5);
    fn[5] = 0x90;
    VirtualProtect(fn, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    return true;
}

// --- item info popups (aftermarket / performance / skill mods / achievements) ---
// The popup managers get the cursor in screen pixels (0x401BE0: GetCursorPos + ScreenToClient)
// and send it to the _Overlay movie as xPos/yPos; _Overlay's ActionScript uses it as Flash
// coordinates and flips/clamps the popup against Stage.width/height (ShowPopupAtPoint /
// OffsetFromScreenEdge). That only matches while _Overlay is unscaled, so convert the cursor to
// _Overlay's stage coordinates (stage = px * ViewScale + ViewOffset, the mapping GFx uses for
// mouse input) right after it is read. At each site the 8 bytes after the cursor call are
// `lea ecx,[ebp+d8]; call X`; they become a call to a per-site stub that converts and then
// performs them.
struct PopupSite { DWORD addr; int xOff, yOff; };   // site, x/y int slots relative to its ebp
static const PopupSite kPopupSites[] = {
    { 0x005195D6, -4,   -8 },   // part popup
    { 0x0051968B, 0x10, -4 },   // skill mod popup
    { 0x0051974B, 0x10, -4 },   // vanity (aftermarket) part popup
    { 0x00519803, -4,   -8 },   // achievement / news popup
};

static char* FindMovieByName(const char* name) {
    for (auto& kv : gMovies)
        if (_stricmp(kv.second.name.c_str(), name) == 0) return (char*)kv.first;
    return nullptr;
}

static void __cdecl FixPopupCursor(int site, char* frame) {
    static int logged;
    __try {
        EnterCriticalSection(&gLock);
        char* m = FindMovieByName("_Overlay");
        LeaveCriticalSection(&gLock);
        if (!m || *(DWORD*)m != gMovieVtbl) return;
        float sx = *(float*)(m + kOffViewScaleX), sy = *(float*)(m + kOffViewScaleY);
        float ox = *(float*)(m + kOffViewOffX),   oy = *(float*)(m + kOffViewOffY);
        if (!(sx > 0.0f && sy > 0.0f) || (sx == 1.0f && sy == 1.0f && ox == 0.0f && oy == 0.0f)) return;
        int* px = (int*)(frame + kPopupSites[site].xOff);
        int* py = (int*)(frame + kPopupSites[site].yOff);
        int nx = (int)lroundf(*px * sx + ox), ny = (int)lroundf(*py * sy + oy);
        if (gLog && logged < 40) {
            ++logged;
            Log("popup cursor (site %d) %d,%d -> %d,%d (_Overlay view scale %.3f,%.3f off %.1f,%.1f)",
                site, *px, *py, nx, ny, sx, sy, ox, oy);
        }
        *px = nx; *py = ny;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static bool PatchPopupSites(DWORD delta) {
    DWORD callee[_countof(kPopupSites)];
    for (int i = 0; i < (int)_countof(kPopupSites); ++i) {
        BYTE* p = (BYTE*)(kPopupSites[i].addr + delta);
        if (p[0] != 0x8D || p[1] != 0x4D || p[3] != 0xE8) return false;   // lea ecx,[ebp+d8]; call rel32
        callee[i] = (DWORD)(p + 8) + *(DWORD*)(p + 4);
    }
    BYTE* code = (BYTE*)VirtualAlloc(nullptr, 64 * _countof(kPopupSites), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!code) return false;
    for (int i = 0; i < (int)_countof(kPopupSites); ++i) {
        BYTE* p = (BYTE*)(kPopupSites[i].addr + delta);
        BYTE* st = code + i * 64, *q = st;
        *q++ = 0x60;                                   // pushad
        *q++ = 0x55;                                   // push ebp
        *q++ = 0x6A; *q++ = (BYTE)i;                   // push site
        *q++ = 0xE8; *(DWORD*)q = (DWORD)FixPopupCursor - (DWORD)(q + 4); q += 4;
        *q++ = 0x83; *q++ = 0xC4; *q++ = 0x08;         // add esp, 8
        *q++ = 0x61;                                   // popad
        *q++ = 0x8D; *q++ = 0x4D; *q++ = p[2];         // lea ecx, [ebp+d8]  (original)
        *q++ = 0xE9; *(DWORD*)q = callee[i] - (DWORD)(q + 4); q += 4;   // jmp X (returns to the nops)
        DWORD old;
        VirtualProtect(p, 8, PAGE_EXECUTE_READWRITE, &old);
        p[0] = 0xE8; *(DWORD*)(p + 1) = (DWORD)st - (DWORD)(p + 5);
        p[5] = p[6] = p[7] = 0x90;
        VirtualProtect(p, 8, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    return true;
}

// --- HUD input region (event HUD buttons: Quit Event, Car/Camera, Snapshot) ---
// 0x481ED0 asks a HUD movie for ActionScript getWidth()/getHeight() (its Stage size) and passes
// them to 0x4812F0(w, h), which registers the HUD's mouse input region in screen pixels. For a
// scaled movie Stage.width/height are logical units (e.g. 1365x768 at 2560x1440), so clicks
// outside that top-left area never reach the HUD. The call at 0x481F4E is redirected to a
// wrapper that converts the size to pixels with the movie's view scale (px = stage / ViewScale).
static const DWORD kSetInputRegionCall = 0x00481F4E;
static const DWORD kSetInputRegion     = 0x004812F0;
static const DWORD kOffHudMovie        = 0x2C;   // HUD object -> its GFx movie
typedef void(__thiscall* SetInputRegion_t)(void*, int, int);
static SetInputRegion_t oSetInputRegion;

// Returns the HUD's movie if its stage is scaled (view scale != 1), else null.
static char* ScaledHudMovie(void* hud, float& sx, float& sy) {
    __try {
        char* m = *(char**)((char*)hud + kOffHudMovie);
        if (!m || *(DWORD*)m != gMovieVtbl) return nullptr;
        sx = *(float*)(m + kOffViewScaleX); sy = *(float*)(m + kOffViewScaleY);
        return sx > 0.0f && sy > 0.0f && (sx != 1.0f || sy != 1.0f) ? m : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static void __fastcall hkSetInputRegion(void* hud, void*, int w, int h) {
    float sx, sy;
    if (char* m = ScaledHudMovie(hud, sx, sy)) {
        // Native behaviour: Stage size == the rect the game gave the movie. Use exactly that
        // (game coordinates), not the enlarged on-screen rect - an oversized region steals
        // keyboard focus from other windows (Esc stopped opening the menu until a click).
        int nw = 0, nh = 0;
        EnterCriticalSection(&gLock);
        auto it = gMovies.find(m);
        if (it != gMovies.end()) { nw = it->second.orig.Width; nh = it->second.orig.Height; }
        LeaveCriticalSection(&gLock);
        if (nw <= 0 || nh <= 0) {
            // unknown movie: convert the stage size (the game truncated it to ints, so round up)
            nw = (int)ceilf(w / sx - 0.01f); nh = (int)ceilf(h / sy - 0.01f);
        }
        if (nw != w || nh != h) Log("HUD input region %s: %dx%d -> %dx%d", MovieName(m).c_str(), w, h, nw, nh);
        w = nw; h = nh;
    }
    oSetInputRegion(hud, w, h);
}

static bool PatchInputRegion(DWORD delta) {
    BYTE* p = (BYTE*)(kSetInputRegionCall + delta);
    if (p[0] != 0xE8 || (DWORD)(p + 5) + *(DWORD*)(p + 1) != kSetInputRegion + delta) return false;
    oSetInputRegion = (SetInputRegion_t)(kSetInputRegion + delta);
    DWORD old;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    *(DWORD*)(p + 1) = (DWORD)hkSetInputRegion - (DWORD)(p + 5);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    return true;
}

static bool PatchSlot(DWORD* slot, void* fn);

// --- hide the mouse cursor in-game after inactivity ---
// "In-game" = every movie the game draws this frame is HUD (HudMovies). Any other visible movie
// (menus, shops, windows, map) counts as a menu and keeps the cursor. The decision runs on the
// game's window thread (PeekMessageW), which owns the cursor; SetCursor calls from the game are
// remembered and suppressed while hidden.
static const DWORD kSlotDisplay = 0x90 / 4, kSlotGetVisible = 0x24 / 4;
static const DWORD kIatSetCursor = 0x00B1D86C, kIatPeekMessageW = 0x00B1D7E4;
typedef void(__thiscall* Display_t)(void*);
typedef bool(__thiscall* GetVisible_t)(void*);
typedef HCURSOR(WINAPI* SetCursor_t)(HCURSOR);
typedef BOOL(WINAPI* PeekMessageW_t)(LPMSG, HWND, UINT, UINT, UINT);
static Display_t      oDisplay;
static GetVisible_t   oGetVisible;
static SetCursor_t    oSetCursor;
static PeekMessageW_t oPeekMessageW;
static volatile DWORD gLastMenuDrawn;      // tick of the last frame a non-HUD movie was visible
static char           gMenuBlocker[64];   // its name (for the log)
static HCURSOR        gGameCursor;
static bool           gCursorHidden;

static bool VisibleSafe(void* self) {
    __try { return oGetVisible(self); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void __fastcall hkDisplay(void* self, void*) {
    oDisplay(self);
    if (!VisibleSafe(self)) return;
    std::string n = MovieName(self);
    if (InList(self, gHudMovies, n)) return;
    gLastMenuDrawn = GetTickCount();
    strncpy(gMenuBlocker, n.empty() ? "?" : n.c_str(), sizeof(gMenuBlocker) - 1);
}

static HCURSOR WINAPI hkSetCursor(HCURSOR c) {
    gGameCursor = c;
    return oSetCursor(gCursorHidden ? nullptr : c);
}

static void CursorTick() {
    static POINT lastPos; static DWORD lastActivity; static bool lastMenu; static char lastBlocker[64];
    DWORD now = GetTickCount();
    POINT p; GetCursorPos(&p);
    bool buttons = (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000;
    if (p.x != lastPos.x || p.y != lastPos.y || buttons || !lastActivity) { lastPos = p; lastActivity = now; }

    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    bool focused = pid == GetCurrentProcessId();
    bool menu = now - gLastMenuDrawn < 300;
    if (gLog && (menu != lastMenu || (menu && strcmp(lastBlocker, gMenuBlocker) != 0))) {
        Log(menu ? "cursor: menu open (%s) - cursor kept visible" : "cursor: in-game (HUD only)%s", menu ? gMenuBlocker : "");
        strcpy(lastBlocker, gMenuBlocker);
    }
    lastMenu = menu;

    bool hide = focused && !menu && now - lastActivity >= (DWORD)(gHideCursorDelay * 1000.0f);
    if (hide != gCursorHidden) {
        gCursorHidden = hide;
        oSetCursor(hide ? nullptr : gGameCursor);
    }
}

static BOOL WINAPI hkPeekMessageW(LPMSG msg, HWND wnd, UINT mn, UINT mx, UINT rm) {
    BOOL r = oPeekMessageW(msg, wnd, mn, mx, rm);
    CursorTick();
    return r;
}

static bool PatchIat(DWORD slot, void* hook, void** orig) {
    DWORD* p = (DWORD*)slot;
    DWORD old;
    if (!VirtualProtect(p, 4, PAGE_READWRITE, &old)) return false;
    *orig = (void*)*p;
    *p = (DWORD)hook;
    VirtualProtect(p, 4, old, &old);
    return *orig != nullptr;
}

static bool InstallCursorHide(DWORD delta, DWORD* vtbl) {
    void* sc = GetProcAddress(GetModuleHandleA("user32.dll"), "SetCursor");
    void* pm = GetProcAddress(GetModuleHandleA("user32.dll"), "PeekMessageW");
    if (*(DWORD*)(kIatSetCursor + delta) != (DWORD)sc || *(DWORD*)(kIatPeekMessageW + delta) != (DWORD)pm)
        return false;   // IAT layout differs (or already hooked by something else)
    oDisplay    = (Display_t)vtbl[kSlotDisplay];
    oGetVisible = (GetVisible_t)vtbl[kSlotGetVisible];
    return PatchSlot(&vtbl[kSlotDisplay], (void*)hkDisplay) &&
           PatchIat(kIatSetCursor + delta, (void*)hkSetCursor, (void**)&oSetCursor) &&
           PatchIat(kIatPeekMessageW + delta, (void*)hkPeekMessageW, (void**)&oPeekMessageW);
}

// --- diagnostics: who swallows Esc/Enter ---
// Before running a key binding (WM_CHAR handler 0x5311F0), the game asks the window that has
// keyboard focus (window manager +0xA0) via ActionScript screen.OnKey(ch) whether it consumes
// the key (0x52E9C0). The call at 0x53120F is redirected to log, for Esc/Enter, which movie had
// focus and whether it consumed the key.
static const DWORD kKeyGateCall = 0x0053120F, kKeyGate = 0x0052E9C0;
static const DWORD kUiRoot = 0x00CECC8C, kGetFocusedWindow = 0x004838D0;
typedef bool(__thiscall* KeyGate_t)(void*, int);
typedef void*(__stdcall* GetUiMgr_t)(void*);
typedef void*(__thiscall* GetFocused_t)(void*);
typedef void*(__thiscall* WinMovie_t)(void*);
static KeyGate_t oKeyGate;
static DWORD     gKeyDelta;

static void* FocusedMovie() {
    __try {
        void* root = (void*)(kUiRoot + gKeyDelta);
        void* mgr = ((GetUiMgr_t)((*(DWORD**)root)[0x9C / 4]))(root);
        if (!mgr) return nullptr;
        void* win = ((GetFocused_t)(kGetFocusedWindow + gKeyDelta))(mgr);
        if (!win) return nullptr;
        return ((WinMovie_t)((*(DWORD**)win)[0x34 / 4]))(win);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static bool __fastcall hkKeyGate(void* self, void*, int ch) {
    bool consumed = oKeyGate(self, ch);
    if (gLog && ((ch & 0xFF) == 0x1B || (ch & 0xFF) == 0x0D)) {
        void* m = FocusedMovie();
        Log("key %s: focused window movie=%p %s -> %s", (ch & 0xFF) == 0x1B ? "ESC" : "ENTER", m,
            m ? (MovieName(m).empty() ? "?" : MovieName(m).c_str()) : "(none)",
            consumed ? "CONSUMED by the window (binding not run)" : "not consumed (binding runs)");
    }
    return consumed;
}

static bool PatchKeyGate(DWORD delta) {
    BYTE* p = (BYTE*)(kKeyGateCall + delta);
    if (p[0] != 0xE8 || (DWORD)(p + 5) + *(DWORD*)(p + 1) != kKeyGate + delta) return false;
    gKeyDelta = delta;
    oKeyGate = (KeyGate_t)(kKeyGate + delta);
    DWORD old;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    *(DWORD*)(p + 1) = (DWORD)hkKeyGate - (DWORD)(p + 5);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    return true;
}

// --- GFxLoader::CreateMovie(filename, flags, arena) -> GFxMovieDef* : records movie names ---
static const DWORD kCreateMovie = 0x009A6120;
static const BYTE  kCreateMoviePrologue[] = { 0x8B, 0x54, 0x24, 0x04, 0x85, 0xD2 };  // mov edx,[esp+4]; test edx,edx
typedef void*(__thiscall* CreateMovie_t)(void*, const char*, unsigned, unsigned);
static CreateMovie_t oCreateMovie;

static void* __fastcall hkCreateMovie(void* loader, void*, const char* file, unsigned flags, unsigned arena) {
    void* def = oCreateMovie(loader, file, flags, arena);
    if (def && file) {
        // "GFX/Tooltip.gfx" -> "Tooltip"
        const char* b = file;
        for (const char* p = file; *p; ++p) if (*p == '/' || *p == '\\') b = p + 1;
        std::string name(b);
        size_t dot = name.rfind('.');
        if (dot != std::string::npos) name.resize(dot);
        void* key = DataKey((char*)def);
        EnterCriticalSection(&gNameLock);
        bool isNew = gMovieNames.find(def) == gMovieNames.end();
        gMovieNames[def] = name;              // the def pointer itself...
        if (key) gMovieNames[key] = name;     // ...and the shared movie data it leads to
        LeaveCriticalSection(&gNameLock);
        if (isNew) Log("movie loaded: %s (%s)", name.c_str(), file);
    }
    return def;
}

// Copy a position-independent prologue into a trampoline and jmp from the function to `hook`.
static void* HookPrologue(BYTE* fn, const BYTE* prologue, size_t len, void* hook) {
    if (memcmp(fn, prologue, len) != 0 || len < 5) return nullptr;
    BYTE* tr = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return nullptr;
    memcpy(tr, prologue, len);
    tr[len] = 0xE9; *(DWORD*)(tr + len + 1) = (DWORD)(fn + len) - (DWORD)(tr + len + 5);
    DWORD old;
    VirtualProtect(fn, len, PAGE_EXECUTE_READWRITE, &old);
    fn[0] = 0xE9; *(DWORD*)(fn + 1) = (DWORD)hook - (DWORD)(fn + 5);
    for (size_t i = 5; i < len; ++i) fn[i] = 0x90;
    VirtualProtect(fn, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    return tr;
}

static bool PatchSlot(DWORD* slot, void* fn) {
    DWORD old;
    if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) return false;
    *slot = (DWORD)fn;
    VirtualProtect(slot, 4, old, &old);
    return true;
}

static void Init(HMODULE self) {
    InitializeCriticalSection(&gLock);
    InitializeCriticalSection(&gNameLock);

    char path[MAX_PATH];
    GetModuleFileNameA(self, path, MAX_PATH);
    char* dot = strrchr(path, '.');
    if (dot) strcpy(dot, ".ini"); else strcat(path, ".ini");
    LoadConfig(path);
    if (gLog) {
        if (dot) strcpy(dot, ".log");
        gLogFile = fopen(path, "w");
    }

    DWORD delta = gDelta = (DWORD)GetModuleHandleA(nullptr) - kImageBase;
    DWORD* vtbl = (DWORD*)(kMovieRootVtbl + delta);
    __try {
        if (vtbl[kSlotSetViewport] != kOrigSetViewport + delta ||
            vtbl[kSlotGetViewport] != kOrigGetViewport + delta) {
            Log("Unsupported nfsw.exe: vtable mismatch (%08X %08X). Plugin disabled.",
                vtbl[kSlotSetViewport], vtbl[kSlotGetViewport]);
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("Unsupported nfsw.exe: vtable not readable. Plugin disabled.");
        return;
    }

    oSetViewport = (SetViewport_t)vtbl[kSlotSetViewport];
    oGetViewport = (GetViewport_t)vtbl[kSlotGetViewport];
    bool ok = PatchSlot(&vtbl[kSlotSetViewport], (void*)hkSetViewport) &&
              PatchSlot(&vtbl[kSlotGetViewport], (void*)hkGetViewport);
    gMovieVtbl = (DWORD)vtbl;
    if (gLog) {
        bool okKey = false;
        __try { okKey = PatchKeyGate(delta); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Log("Key focus diagnostics (Esc/Enter): %s", okKey ? "installed" : "FAILED");
    }
    if (gHideCursor) {
        bool okCur = false;
        __try { okCur = InstallCursorHide(delta, vtbl); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Log("Hide cursor in-game after %.1fs: %s (%u HUD movies)", gHideCursorDelay, okCur ? "installed" : "FAILED", (unsigned)gHudMovies.size());
    }
    {
        bool okIn = false;
        __try { okIn = PatchInputRegion(delta); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Log("HUD input region patch: %s", okIn ? "installed" : "FAILED");
    }
    {
        void* tr = nullptr;
        __try { tr = HookPrologue((BYTE*)(kCreateMovie + delta), kCreateMoviePrologue, sizeof(kCreateMoviePrologue), (void*)hkCreateMovie); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        oCreateMovie = (CreateMovie_t)tr;
        gNamesActive = tr != nullptr;   // otherwise fall back to kKnownStages
        Log("GFxLoader::CreateMovie hook (movie names): %s", tr ? "installed" : "FAILED - name-based options inactive");
    }
    if (gRememberPositions) {
        strcpy(gPosPath, path);
        char* d = strrchr(gPosPath, '.');
        strcpy(d ? d : gPosPath + strlen(gPosPath), ".positions.ini");
        LoadPositions();
        Log("Remembered window positions: %u (%s)", (unsigned)gSavedPos.size(), gPosPath);
    }
    if (gFixMenuTooltips) {
        bool okTip = false;
        __try { okTip = HookShowTooltipAt(delta); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Log("Menu tooltip (ShowTooltipAt) hook: %s", okTip ? "installed" : "FAILED");
        bool okPop = false;
        __try { okPop = PatchPopupSites(delta); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Log("Item info popup cursor patch: %s", okPop ? "installed" : "FAILED");
    }
    bool mapContent = false;
    auto listed = [](const std::vector<std::string>& v) {
        for (auto& e : v) if (_stricmp(e.c_str(), "WorldMap") == 0) return true;
        return false;
    };
    for (auto& e : gContentOnlyStages) if (e.w == 1089 && e.h == 687) mapContent = true;
    if (listed(gContentOnlyMovies)) mapContent = true;
    for (auto& e : gExcludeStages)     if (e.w == 1089 && e.h == 687) mapContent = false;  // excluded wins
    if (listed(gExcludeMovies)) mapContent = false;
    if (mapContent) {
        bool okResize = false;
        __try { okResize = PatchOnResize(delta); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Log("World map OnResize patch: %s", okResize ? "installed" : "FAILED (map content will be cropped)");
    }
    if (gDebugStage.w > 0 || !gDebugMovies.empty()) {
        oHandleEvent = (HandleEvent_t)vtbl[kSlotHandleEvent];
        oNotifyMouse = (NotifyMouse_t)vtbl[kSlotNotifyMouse];
        oHitTest     = (HitTest_t)vtbl[kSlotHitTest];
        PatchSlot(&vtbl[kSlotHandleEvent], (void*)hkHandleEvent);
        PatchSlot(&vtbl[kSlotNotifyMouse], (void*)hkNotifyMouse);
        PatchSlot(&vtbl[kSlotHitTest],     (void*)hkHitTest);
        Log("Mouse diagnostics enabled (stage %dx%d, %u movie names)", gDebugStage.w, gDebugStage.h, (unsigned)gDebugMovies.size());
    }
    Log("NFSW.HUDScale 1.0.0 loaded: hooks %s. UIScale=%.3f (0=auto) BaseHeight=%d Full=%d Windows=%d excludes=%u",
        ok ? "installed" : "FAILED", gUIScale, gBaseHeight, gScaleFull, gScaleWindows, (unsigned)gExclude.size());
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        Init(mod);
    } else if (reason == DLL_PROCESS_DETACH) {
        FlushPositions(true);
    }
    return TRUE;
}
