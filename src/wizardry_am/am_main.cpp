/*
 *  Wizardry 6 & 7 Automap Mod
 *  Original: Copyright (C) 2014 KoriTama
 *  Refactor: Copyright (C) 2025 DungeonCrawl-Classics.com 
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  ── Refactor Details -─────────────────────────────────────────────────────
 *  The original mod used OpenGL 1.x immediate mode (glBegin/glEnd).
 *  This port replaces that entirely with SDL_Renderer (SDL2 accelerated 2D).
 *  This port also uses DOXBox Staging 0.84.0-alpha (a550b) for rendering.
 *  -- Fixes core functional issues to restore functionality on latest DOSBox.
 *  -- Fixes to exploration progress (fog of war) in Wiz6 so it works and saves correctly.
 *  -- Added a right-click option to purge saved exploration progress in Wiz6.
 *  -- No Wiz7 exploration purge added as it is handled by the game itself.
 *  -- Added a right-click option to delete all Notes in Wiz6 and Wiz7.
 *
 */

#include "automap.h"

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <vector>
#include <algorithm>

// DOSBox Staging public headers
#include "dosbox.h"
#include "hardware/memory.h"
#include "cpu/cpu.h"
#include "cpu/registers.h"
#include "cpu/paging.h"
#include "cpu/callback.h"
#include "config/setup.h"
#include "config/config.h"
#include "misc/support.h"
#include "hardware/pic.h"

// SDL2
#include <SDL.h>

#ifdef WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <SDL_syswm.h>
#  include <windows.h>
#  include <commctrl.h>
#  include <commdlg.h>
#endif

#include "InputBox.h"
#include <fstream>
#include <string>
#include <sstream>

// ── Game identification ───────────────────────────────────────────────────────

enum class AutoMapGame { None = 0, Wizardry6, Wizardry7 };
static AutoMapGame s_amGame = AutoMapGame::None;

static const char* s_amGameName[] = { "undetected", "Wizardry 6", "Wizardry 7" };

// ── Config / window state ─────────────────────────────────────────────────────

bool am_show_automap     = false;
int  am_width            = 512;
int  am_height           = 512;

extern bool amw6_show_tooltips;
extern bool amw6_hide_in_dark_zones;
extern bool amw7_show_tooltips;
extern bool amw7_hide_in_dark_zones;
extern bool amw7_use_spellPower;
extern bool amw7_rus;

static int32_t s_versionDelta = 0;

// ── SDL_Renderer window & resources ──────────────────────────────────────────

static SDL_Window*   s_amWindow   = nullptr;
static SDL_Renderer* s_amRenderer = nullptr;
static uint32_t      s_amWindowID = 0;

// Sprite-atlas textures (one per game)
static SDL_Texture*  s_texW6      = nullptr;   // 256×16 RGBA atlas (Wiz6 tiles)
static SDL_Texture*  s_texW7      = nullptr;   // 256×32 RGBA atlas (Wiz7 tiles)

extern unsigned long  tiles32[];        // from am_wiz6_res.cpp
extern int            tiles32_count;
extern unsigned long  w7tiles32[];      // from am_wiz7_res.cpp
extern int            w7tiles32_count;

// ── Color palette for solid-color drawing ────────────────────────────────────
// Mirrors the original glColor3f table in DrawRectGL.

static const SDL_Color s_palette[] = {
    {   0,   0,   0, 255 }, // -1  black         (background / no feature)
    { 255,   0,   0, 255 }, //  0  red            (no feature)
    {   0, 255,   0, 255 }, //  1  green          (stairs up)
    { 255, 255,   0, 255 }, //  2  yellow         (stairs down)
    {   0,   0, 255, 255 }, //  3  blue           (sconce)
    { 255,   0, 255, 255 }, //  4  magenta        (fountain)
    {   0, 255, 255, 255 }, //  5  cyan
    {  64,  64,  64, 255 }, //  6  dark grey
    { 191, 191, 191, 255 }, //  7  light grey
    { 153, 102,  31, 255 }, //  8  brown
    { 250, 159,  31, 255 }, //  9  pumpkin orange (niche)
    { 250,  10, 179, 255 }, // 10  hot pink
    { 153, 102, 179, 255 }, // 11  purple
    { 255, 255, 255, 255 }, // 12+ white (default)
};
static constexpr int PALETTE_SIZE = (int)(sizeof(s_palette) / sizeof(s_palette[0]));

static SDL_Color IndexToColor(int idx) {
    // idx -1 maps to index 0 (black)
    int i = idx + 1;
    if (i < 0 || i >= PALETTE_SIZE) i = PALETTE_SIZE - 1;
    return s_palette[i];
}

// ── Drawing primitives ────────────────────────────────────────────────────────
// These replace every glBegin/glEnd call in the original am_main.cpp.

void AM_DrawRect(int x, int y, int w, int h, int colorIndex)
{
    if (!s_amRenderer) return;
    SDL_Color c = IndexToColor(colorIndex);
    SDL_SetRenderDrawColor(s_amRenderer, c.r, c.g, c.b, c.a);
    SDL_Rect r = { x, y, w, h };
    SDL_RenderFillRect(s_amRenderer, &r);
}

// Draw a sub-region of a sprite atlas at (x,y) with size (w,h).
// u0,v0,u1,v1 are normalised UV coordinates in [0,1].
// The atlas pixel dimensions must be passed so we can convert to SDL_Rect.
// 'dark' dims the quad to 50% brightness.

void AM_DrawSprite(int x, int y, int w, int h,
                   float u0, float v0, float u1, float v1,
                   SDL_Texture* tex,
                   int atlasW, int atlasH,
                   bool dark = false,
                   SDL_RendererFlip flip = SDL_FLIP_NONE)
{
    if (!s_amRenderer || !tex) return;

    int sx = (int)(u0 * atlasW);
    int sy = (int)(v0 * atlasH);
    int sw = (int)((u1 - u0) * atlasW);
    int sh = (int)((v1 - v0) * atlasH);
    if (sw <= 0 || sh <= 0) return;

    SDL_SetTextureColorMod(tex,
        dark ? 128 : 255,
        dark ? 128 : 255,
        dark ? 128 : 255);

    SDL_Rect src  = { sx, sy, sw, sh };
    SDL_Rect dest = {  x,  y,  w,  h };
    SDL_RenderCopyEx(s_amRenderer, tex, &src, &dest, 0, nullptr, flip);

    SDL_SetTextureColorMod(tex, 255, 255, 255);
}

// Wiz6 atlas is 256×16 px.  Wiz7 atlas is 256×32 px.
// Expose named wrappers so am_wiz6.cpp / am_wiz7.cpp don't need to know the
// atlas dimensions.

void AM_DrawW6Sprite(int x, int y, int w, int h,
                     float u0, float v0, float u1, float v1,
                     bool dark = false,
                     SDL_RendererFlip flip = SDL_FLIP_NONE)
{
    AM_DrawSprite(x, y, w, h, u0, v0, u1, v1, s_texW6, 256, 16, dark, flip);
}

void AM_DrawW7Sprite(int x, int y, int w, int h,
                     float u0, float v0, float u1, float v1,
                     bool dark = false,
                     SDL_RendererFlip flip = SDL_FLIP_NONE)
{
    AM_DrawSprite(x, y, w, h, u0, v0, u1, v1, s_texW7, 256, 32, dark, flip);
}

// ── Texture upload ────────────────────────────────────────────────────────────
// The resource files store 32-bit RGBA pixels in arrays of unsigned long.
// We need them as SDL_PIXELFORMAT_RGBA8888 (or ABGR depending on endianness).
// The original code ORed 0xFF000000 to set alpha, matching OpenGL GL_RGBA.

static SDL_Texture* UploadAtlas(SDL_Renderer* ren,
                                unsigned long* pixels, int count,
                                int atlasW, int atlasH)
{
    // Make a local copy so we don't modify the read-only source array
    std::vector<uint32_t> buf(count);
    for (int i = 0; i < count; ++i) {
        uint32_t p = (uint32_t)pixels[i];
        if (p == 0) { buf[i] = 0; continue; }
        // Data is 0x00BBGGRR, set alpha to FF → swap R and B for ARGB8888 (0xFFRRGGBB)
        uint8_t b = (p >> 16) & 0xFF;
        uint8_t g = (p >>  8) & 0xFF;
        uint8_t r = (p      ) & 0xFF;
        buf[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }

    SDL_Texture* tex = SDL_CreateTexture(ren,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STATIC,
        atlasW, atlasH);
    if (!tex) {
        LOG_MSG("AUTOMAP: SDL_CreateTexture failed: %s", SDL_GetError());
        return nullptr;
    }

    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_UpdateTexture(tex, nullptr, buf.data(), atlasW * 4);
    return tex;
}

// ── Renderer helpers ──────────────────────────────────────────────────────────

static void BeginFrame()
{
    if (!s_amRenderer) return;
    SDL_SetRenderDrawColor(s_amRenderer, 0, 0, 0, 255);
    SDL_RenderClear(s_amRenderer);
    // Logical size lets all drawing code use fixed coordinates regardless of
    // how the user has resized the automap window.
    SDL_RenderSetLogicalSize(s_amRenderer, am_width, am_height);
}

static void EndFrame()
{
    if (!s_amRenderer) return;
    SDL_RenderPresent(s_amRenderer);
}

// ── Window title helper ───────────────────────────────────────────────────────

void SetAutomapWindowTitle(const char* title)
{
    static char oldTitle[1024] = {};
    if (s_amWindow && strcmp(oldTitle, title) != 0) {
        strncpy(oldTitle, title, sizeof(oldTitle) - 1);
        SDL_SetWindowTitle(s_amWindow, title);
    }
}

// ── Win32 tooltip plumbing (Windows only) ────────────────────────────────────
// Unchanged from the original except that HWND is obtained via SDL_GetWindowWMInfo
// which still works in SDL2.

#ifdef WIN32
static TOOLINFOW s_toolItem  = {};
static HWND      s_hwndAM    = nullptr;   // automap HWND
static HWND      s_hwndTT    = nullptr;   // automap tooltip

static TOOLINFOW s_toolItemM = {};
static HWND      s_hwndMain  = nullptr;   // main game HWND
static HWND      s_hwndTTM   = nullptr;   // main-window tooltip

static HWND CreateTrackingTooltip(HWND parent, LPCWSTR text, TOOLINFOW& ti, bool transparent)
{
    InitCommonControls();
    HWND hwndTT = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP | WS_CHILD | WS_VISIBLE | SS_NOTIFY,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        parent, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!hwndTT) return nullptr;

    SetWindowPos(hwndTT, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    memset(&ti, 0, sizeof(ti));
    ti.cbSize   = TTTOOLINFOW_V2_SIZE;
    ti.uFlags   = TTF_IDISHWND | TTF_TRACK | TTF_ABSOLUTE
                | (transparent ? TTF_TRANSPARENT : 0u);
    ti.hwnd     = parent;
    ti.hinst    = GetModuleHandle(nullptr);
    ti.lpszText = const_cast<LPWSTR>(text);
    ti.uId      = (UINT_PTR)parent;
    GetClientRect(parent, &ti.rect);

    SendMessageW(hwndTT, TTM_ADDTOOLW,        0, (LPARAM)&ti);
    SendMessageW(hwndTT, TTM_SETMAXTIPWIDTH,  0, 400);
    return hwndTT;
}
#endif // WIN32

// ── Public tooltip API (called by am_wiz6/am_wiz7) ───────────────────────────

void TooltipForAutomapWindow_Show(bool show)
{
#ifdef WIN32
    if (!s_hwndTT) return;
    SendMessageW(s_hwndTT, TTM_TRACKACTIVATE,
        (WPARAM)(show ? TRUE : FALSE), (LPARAM)&s_toolItem);
#endif
}

void TooltipForAutomapWindow_SetText(wchar_t* text)
{
#ifdef WIN32
    if (!s_hwndTT) return;
    s_toolItem.lpszText = const_cast<LPWSTR>(text);
    SendMessageW(s_hwndTT, TTM_SETTOOLINFOW, 0, (LPARAM)&s_toolItem);
#endif
}

static BOOL s_showTipBackup = -1;

void TooltipForMainWindow_Show(bool show)
{
#ifdef WIN32
    if (!s_hwndTTM) return;
    BOOL SHOW = show ? TRUE : FALSE;
    if (s_showTipBackup != SHOW) {
        s_showTipBackup = SHOW;
        SendMessageW(s_hwndTTM, TTM_TRACKACTIVATE, (WPARAM)SHOW, (LPARAM)&s_toolItemM);
    }
#endif
}

void TooltipForMainWindow_SetText(wchar_t* text)
{
#ifdef WIN32
    if (!s_hwndTTM) return;
    s_toolItemM.lpszText = const_cast<LPWSTR>(text);
    SendMessageW(s_hwndTTM, TTM_SETTOOLINFOW, 0, (LPARAM)&s_toolItemM);
#endif
}

// ── Callback / loop plumbing ──────────────────────────────────────────────────

static uintptr_t  s_automapCallbackHandle = 0;
static bool  s_amExitLoop = false;

static uintptr_t AutomapLoopResume()
{
    // Re-enable all breakpoints and hand control back to the normal CPU loop.
    extern void AM_ActivateBreakpoints(PhysPt excludeAddr, bool activate);
    extern uintptr_t am_ignoreAddressOnce;

    AM_ActivateBreakpoints(SegPhys(cs) + reg_eip, true);
    am_ignoreAddressOnce = SegPhys(cs) + reg_eip;
    DOSBOX_SetNormalLoop();
    return 0;
}

static uintptr_t AutoMapCallbackHandler()
{
    s_amExitLoop = true;
    DOSBOX_SetLoop(&AutomapLoopResume);
    return 0;
}

void W6_NativeSaveVis();  // forward declaration
void W6_NativeLoadVis();  // forward declaration
void W6_NativeSaveNotes(); // forward declaration
void W6_NativeLoadNotes(); // forward declaration
void W6_SetSavePath(const char* basePath);  // forward declaration
void W7_SetSavePath(const char* basePath);  // forward declaration
void W7_NativeLoadNotes();  // forward declaration
void W7_NativeSaveNotes();  // forward declaration

bool AutoMapExitLoop()
{
    if (s_amExitLoop) {
        s_amExitLoop = false;
        // Save W6 visdata and notes on exit
        if (s_amGame == AutoMapGame::Wizardry6) { W6_NativeSaveVis(); W6_NativeSaveNotes(); }
        // Save W7 notes on exit
        if (s_amGame == AutoMapGame::Wizardry7) W7_NativeSaveNotes();
        return true;
    }
    return false;
}

// ── Breakpoint engine ─────────────────────────────────────────────────────────
// Identical logic to the original — writes 0xCC into emulated memory.

typedef void (*AM_BreakpointCallback)();

struct AM_BP {
    bool                  active;
    bool                  isInOverlay;
    PhysPt                location;
    uint8_t               oldData;
    AM_BreakpointCallback callback;
};

static std::vector<AM_BP> s_bps;

uintptr_t am_ignoreAddressOnce = 0;

static PhysPt AM_PhysMakeProt(uint16_t selector, uint32_t offset)
{
    Descriptor desc;
    if (cpu.gdt.GetDescriptor(selector, desc)) return desc.GetBase() + offset;
    return 0;
}

static PhysPt AM_GetAddress(uint16_t seg, uint32_t offset)
{
    if (seg == SegValue(cs)) return SegPhys(cs) + offset;
    if (cpu.pmode && !(reg_flags & FLAG_VM)) {
        Descriptor desc;
        if (cpu.gdt.GetDescriptor(seg, desc))
            return AM_PhysMakeProt(seg, offset);
    }
    return (seg << 4) + offset;
}

int AM_AddBreakpoint(uint32_t off, bool isInOverlay = false,
                     AM_BreakpointCallback cb = nullptr)
{
    AM_BP bp;
    bp.active      = false;
    bp.isInOverlay = isInOverlay;
    bp.location = AM_GetAddress((uint16_t)(off >> 16), (uint16_t)(off & 0xffff));
    bp.callback    = cb;
    s_bps.push_back(bp);
    return (int)s_bps.size() - 1;
}

bool AM_DeleteBreakpoint(PhysPt where)
{
    auto it = std::find_if(s_bps.begin(), s_bps.end(),
        [where](const AM_BP& b){ return b.location == where; });
    if (it == s_bps.end()) return false;
    s_bps.erase(it);
    return true;
}

void AM_ActivateBreakpoint(int index, bool active)
{
    AM_BP& bp = s_bps[index];
    if (active) {
        uint8_t data = mem_readb(bp.location);
        if (data != 0xCC) {
            bp.oldData = data;
            mem_writeb(bp.location, 0xCC);
        }
    } else {
        if (mem_readb(bp.location) == 0xCC)
            mem_writeb(bp.location, bp.oldData);
    }
    bp.active = active;
}

void AM_ActivateBreakpoints(PhysPt excludeAddr, bool activate)
{
    for (int i = 0; i < (int)s_bps.size(); ++i) {
        if (activate && s_bps[i].location == excludeAddr) continue;
        AM_ActivateBreakpoint(i, activate);
    }
}

static bool s_overlayLoaded              = false;
static bool s_ignoreOverlayCycleOnce     = true;

void AM_ActivateOverlayBreakpoints(PhysPt excludeAddr, bool activate)
{
    for (int i = 0; i < (int)s_bps.size(); ++i) {
        if (s_bps[i].isInOverlay && !s_overlayLoaded) continue;
        if (activate && s_bps[i].location == excludeAddr) continue;
        AM_ActivateBreakpoint(i, activate);
    }
}

static int AutomapCheckBreakpoint(PhysPt addr)
{
    if (am_ignoreAddressOnce != 0 && addr == am_ignoreAddressOnce) {
        am_ignoreAddressOnce = 0;
        return -1;
    }
    am_ignoreAddressOnce = 0;
    for (int i = 0; i < (int)s_bps.size(); ++i)
        if (s_bps[i].active && s_bps[i].location == addr)
            return i;
    return -1;
}

bool AutomapBreakpoint()
{
	LOG_MSG("AUTOMAP: INT3 hit at CS:IP %04X:%04X",
	        (unsigned)SegValue(cs),
	        (unsigned)reg_ip);
    PhysPt where = AM_GetAddress(SegValue(cs), reg_eip);
    int i = AutomapCheckBreakpoint(where);
    if (i < 0) return false;
    AM_ActivateBreakpoint(i, false);
    if (s_bps[i].callback) s_bps[i].callback();
    return true;
}

// ── Forward declarations for per-game modules ─────────────────────────────────

void W6_Update(int xSize, int ySize);
void W7_Update(int xSize, int ySize);
bool W7_NeedUpdate();
void W6_NewGame();
void W7_NewGame();
void W6_Load(const char* fullName);
void W7_Load(const char* fullName);
void W6_Save(const char* fullName);
void W7_Save(const char* fullName);
extern bool amw6_isSaving;
extern bool amw6_isLoading;
void W6_SetSavePath(const char* basePath);
void W6_NativeLoadVis();
void W6_NativeSaveVis();
void W6_NativeSaveNotes();
void W6_NativeLoadNotes();
void W7_OnOverlayLoad();
void W6_OnMouseMotionInMainWindow(RECT rc, int newX, int newY);
void W7_OnMouseMotionInMainWindow(RECT rc, int newX, int newY);
void W6_OnMouseMotionInAutomapWindow(int newX, int newY, bool alt);
void W7_OnMouseMotionInAutomapWindow(int newX, int newY, bool alt);
void W6_OnAutomapDrag(int dx, int dy);
void W7_OnAutomapDrag(int dx, int dy);
void W6_OnMouseButtonInAutomapWindow(SDL_MouseButtonEvent* btn, bool alt, bool ctrl);
void W7_OnMouseButtonInAutomapWindow(SDL_MouseButtonEvent* btn, bool alt, bool ctrl);

extern uint32_t amw6_dataseg_addr;
extern uint32_t amw7_dataseg_addr;
PhysPt am_loadaddress = 0;

// Overlay breakpoint callbacks (defined in am_wiz7.cpp)
extern uint16_t W7GameStateOnOverlayLoad;
void W7BP_OnMapLoadStart();   void W7BP_OnMapLoadEnd();
void W7BP_OnMapRedraw();
void W7BP_OnShowEnemyList();  void W7BP_OnHideEnemyList();
void W7BP_OnShowSpellWindow();void W7BP_OnHideSpellWindow();
void W7BP_OnShowUseWindow();  void W7BP_OnHideUseWindow();
void W7BP_OnShowUnlockWindow();void W7BP_OnHideUnlockWindow();
void W7BP_OnShowDisarmWindow();void W7BP_OnHideDisarmWindow();
void W7BP_OnShowLootWindow(); void W7BP_OnHideLootWindow();
void W7BP_OnShowBuyWindow();  void W7BP_OnHideBuyWindow();
void W7BP_OnShowSellWindow(); void W7BP_OnHideSellWindow();
void W7BP_OnShowGiveWindow(); void W7BP_OnHideGiveWindow();
void W7BP_OnShowSaveWindow(); void W7BP_OnHideSaveWindow();
void W7BP_OnShowConfigWindow();void W7BP_OnHideConfigWindow();
void W7BP_OnShowAddCharacterWindow(); void W7BP_OnHideAddCharacterWindow();
void W7BP_OnShowSecurityWindow(); void W7BP_OnHideSecurityWindow();
void W7BP_OnShowLoadWindow(); void W7BP_OnHideLoadWindow();
void W7BP_OnShowImportWindow();void W7BP_OnHideImportWindow();
void W7BP_OnIntroStart();     void W7BP_OnIntroEnd();
void W7BP_OnCreateSaveFileStart(); void W7BP_OnCreateSaveFileEnd();
void W7BP_OnOpenSaveFileStart();   void W7BP_OnOpenSaveFileEnd();
void W7BP_OnCastSpell();
void AMW7BP_OnMouseLock();    void AMW7BP_OnMouseUnlock();
void AMW7BP_OnBitArrayModification();
void AMBP_OnOverlayLoad();    void AMBP_OnOverlayUnload();

// ── Public: AutoMapInit (dosbox.cpp calls this) ───────────────────────────────

void AUTOMAP_AddConfigSection(const ConfigPtr& conf)
{
	assert(conf);
	using enum Property::Changeable::Value;

    auto section = conf->AddSection("automap");

	auto pbool = section->AddBool("enable", OnlyAtStart, false);
	pbool->SetHelp("Enable automap window for Wizardry 6 and Wizardry 7.");

	pbool = section->AddBool("show_tooltips", OnlyAtStart, true);
	pbool->SetHelp("Show hover tooltips on the automap.");

	pbool = section->AddBool("hide_in_dark_zones", OnlyAtStart, true);
	pbool->SetHelp("Hide unvisited cells in dark zones.");

	auto pint = section->AddInt("width", OnlyAtStart, 512);
	pint->SetHelp("Initial width of the automap window in pixels.");

	pint = section->AddInt("height", OnlyAtStart, 512);
	pint->SetHelp("Initial height of the automap window in pixels.");

	pbool = section->AddBool("wiz7_sns_mode", OnlyAtStart, false);
	pbool->SetHelp("Wizardry 7: use Spell Power data for SNS mode overlay.");
}

void AutoMapInit(Section* section)
{
	auto* sec = dynamic_cast<SectionProp*>(section);
	if (!sec) {
		LOG_MSG("AUTOMAP: AutoMapInit - sec is null!");
		return;
	}

	am_show_automap         = sec->GetBool("enable");
	LOG_MSG("AUTOMAP: enable=%d", (int)am_show_automap);
	amw6_show_tooltips      = sec->GetBool("show_tooltips");
	amw7_show_tooltips      = amw6_show_tooltips;
	amw6_hide_in_dark_zones = sec->GetBool("hide_in_dark_zones");
	amw7_hide_in_dark_zones = amw6_hide_in_dark_zones;
	am_width                = sec->GetInt("width");
	am_height               = sec->GetInt("height");
	amw7_use_spellPower     = sec->GetBool("wiz7_sns_mode");

	s_automapCallbackHandle = CALLBACK_Allocate();
	CALLBACK_Setup(s_automapCallbackHandle, AutoMapCallbackHandler, CB_RETF, "automap");
}

// ── Public: AutoMapCreate (sdlmain.cpp calls this after main window exists) ───

void AutoMapCreate()
{
	// Read config from global control object
	auto* section = dynamic_cast<SectionProp*>(control->GetSection("automap"));
	if (section) {
		am_show_automap = section->GetBool("enable");
		am_width        = section->GetInt("width");
		am_height       = section->GetInt("height");
	}

	LOG_MSG("AUTOMAP: AutoMapCreate called, am_show_automap=%d",
	        (int)am_show_automap);
    if (!am_show_automap) return;

    // Create the secondary window
    s_amWindow = SDL_CreateWindow(
        "undetected - Automap Mod",
        14, SDL_WINDOWPOS_CENTERED,
        am_width, am_height,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);

    if (!s_amWindow) {
        LOG_MSG("AUTOMAP: Failed to create window: %s", SDL_GetError());
        return;
    }
    s_amWindowID = SDL_GetWindowID(s_amWindow);

    // Create an accelerated renderer (falls back to software if needed)
    s_amRenderer = SDL_CreateRenderer(s_amWindow, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_amRenderer) {
        LOG_MSG("AUTOMAP: Accelerated renderer failed (%s), trying software",
                SDL_GetError());
        s_amRenderer = SDL_CreateRenderer(s_amWindow, -1,
            SDL_RENDERER_SOFTWARE);
    }
    if (!s_amRenderer) {
        LOG_MSG("AUTOMAP: Could not create renderer: %s", SDL_GetError());
        SDL_DestroyWindow(s_amWindow);
        s_amWindow = nullptr;
        return;
    }

    LOG_MSG("AUTOMAP: Window and renderer created (SDL_Renderer)");

    // Upload tile atlases
    s_texW6 = UploadAtlas(s_amRenderer, tiles32,   tiles32_count,   256, 16);
    s_texW7 = UploadAtlas(s_amRenderer, w7tiles32, w7tiles32_count, 256, 32);

    // Windows: install tooltip on the automap HWND
#ifdef WIN32
    SDL_SysWMinfo wmi;
    SDL_VERSION(&wmi.version);
    if (SDL_GetWindowWMInfo(s_amWindow, &wmi)) {
        s_hwndAM = wmi.info.win.window;
        s_hwndTT = CreateTrackingTooltip(s_hwndAM, L"", s_toolItem, false);
    }
#endif
}

uint32_t AutoMapGetWindowID() { return s_amWindowID; }

// ── Public: AutoMapOnMainWindowCreate ────────────────────────────────────────

void AutoMapOnMainWindowCreate(void* hwnd_win32)
{
#ifdef WIN32
    s_hwndMain = (HWND)hwnd_win32;
    s_hwndTTM  = CreateTrackingTooltip(s_hwndMain, L"", s_toolItemM, true);
#else
    (void)hwnd_win32;
#endif
}

// ── Public: AutoMapUpdate (called once per frame from sdlmain.cpp) ─────────────

static bool s_needUpdateOnce = false;

void AutoMapUpdate()
{
    if (!am_show_automap || !s_amRenderer) return;

    if (s_amGame == AutoMapGame::Wizardry7 && !s_needUpdateOnce)
        if (!W7_NeedUpdate()) return;
    s_needUpdateOnce = false;

    BeginFrame();

    if (s_amGame == AutoMapGame::Wizardry6) {
        W6_Update(am_width, am_height);
    } else if (s_amGame == AutoMapGame::Wizardry7) {
        W7_Update(am_width, am_height);
    } else {
        // No game detected yet — show a simple status message via title bar
        char title[128];
        snprintf(title, sizeof(title), "%s - Automap Mod",
                 s_amGameName[(int)s_amGame]);
        SetAutomapWindowTitle(title);
    }

    EndFrame();
}

// ── Public: AutoMapHandleEvent ────────────────────────────────────────────────
// Handles all SDL events directed at the automap window.
// Called from the main event loop in sdlmain.cpp.

static bool  s_dragStart = false;
static int   s_dragStartX = 0, s_dragStartY = 0;
static bool  s_cursorShown = true;
#ifdef WIN32
static BOOL  s_trackingMouse  = FALSE;
static BOOL  s_trackingMouseM = FALSE;
#endif

bool AutoMapHandleEvent(const SDL_Event& event)
{
    if (!am_show_automap || !s_amWindow) return false;

    switch (event.type) {

    case SDL_WINDOWEVENT:
        if (event.window.windowID != s_amWindowID) return false;
        switch (event.window.event) {
        case SDL_WINDOWEVENT_EXPOSED:
            s_needUpdateOnce = true;
            AutoMapUpdate();
            break;
        case SDL_WINDOWEVENT_RESIZED:
            am_width  = event.window.data1;
            am_height = event.window.data2;
            SDL_RenderSetLogicalSize(s_amRenderer, am_width, am_height);
            s_needUpdateOnce = true;
            AutoMapUpdate();
            break;
        case SDL_WINDOWEVENT_ENTER:
            s_cursorShown = SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE;
            SDL_ShowCursor(SDL_ENABLE);
            break;
        case SDL_WINDOWEVENT_LEAVE:
            SDL_ShowCursor(s_cursorShown ? SDL_ENABLE : SDL_DISABLE);
            TooltipForAutomapWindow_Show(false);
#ifdef WIN32
            s_trackingMouse = FALSE;
#endif
            s_dragStart = false;
            break;
        case SDL_WINDOWEVENT_CLOSE:
            // Closing the automap window closes the application
		    SDL_Event quit_event = {};
		    quit_event.type      = SDL_QUIT;
		    SDL_PushEvent(&quit_event);
            break;
        }
        return true;

    case SDL_MOUSEMOTION:
        if (event.motion.windowID != s_amWindowID) return false;
        {
            const Uint8* keys = SDL_GetKeyboardState(nullptr);
            bool alt = keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT];
            int nx = event.motion.x;
            int ny = event.motion.y;
            static int oldX = 0, oldY = 0;
            if (!alt) {
                if ((nx != oldX || ny != oldY) && s_dragStart) {
                    if (s_amGame == AutoMapGame::Wizardry6) W6_OnAutomapDrag(oldX-nx, oldY-ny);
                    if (s_amGame == AutoMapGame::Wizardry7) W7_OnAutomapDrag(oldX-nx, oldY-ny);
                }
                oldX = nx; oldY = ny;
            }
            if (s_amGame == AutoMapGame::Wizardry6) W6_OnMouseMotionInAutomapWindow(nx, ny, alt);
            if (s_amGame == AutoMapGame::Wizardry7) W7_OnMouseMotionInAutomapWindow(nx, ny, alt);

#ifdef WIN32
            if (s_hwndTT && s_hwndAM) {
                POINT pt = { nx, ny };
                ClientToScreen(s_hwndAM, &pt);
                SendMessageW(s_hwndTT, TTM_TRACKPOSITION, 0,
                    (LPARAM)MAKELONG(pt.x + 20, pt.y - 30));
                if (!s_trackingMouse) {
                    TRACKMOUSEEVENT tme = { sizeof(tme) };
                    tme.hwndTrack = s_hwndAM;
                    tme.dwFlags   = TME_LEAVE;
                    TrackMouseEvent(&tme);
                    s_trackingMouse = TRUE;
                }
            }
#endif
        }
        return true;

    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (event.button.windowID != s_amWindowID) return false;
        {
            const Uint8* keys = SDL_GetKeyboardState(nullptr);
            bool alt  = keys[SDL_SCANCODE_LALT]  || keys[SDL_SCANCODE_RALT];
            bool ctrl = keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL];

            if (!alt && !ctrl &&
                event.button.state == SDL_PRESSED &&
                event.button.button == SDL_BUTTON_LEFT) {
                s_dragStartX = event.button.x;
                s_dragStartY = event.button.y;
                s_dragStart  = true;
            }
            if (event.button.state == SDL_RELEASED) {
                if (event.button.button == SDL_BUTTON_LEFT && s_dragStart) {
                    s_dragStart = false;
                    // If the mouse moved, it was a drag — don't fire a click
                    if (s_dragStartX != event.button.x ||
                        s_dragStartY != event.button.y)
                        return true;
                }
                if (s_amGame == AutoMapGame::Wizardry6)
                    W6_OnMouseButtonInAutomapWindow(
                        const_cast<SDL_MouseButtonEvent*>(&event.button), alt, ctrl);
                if (s_amGame == AutoMapGame::Wizardry7)
                    W7_OnMouseButtonInAutomapWindow(
                        const_cast<SDL_MouseButtonEvent*>(&event.button), alt, ctrl);
            }
        }
        return true;

    default:
        break;
    }
    return false;
}

// ── Main-window mouse event (called by sdlmain.cpp for the game window) ───────

void AutoMapOnMainWindowMouseMotion(int newX, int newY)
{
    if (!am_show_automap) return;
#ifdef WIN32
    if (!s_hwndMain || !s_hwndTTM) return;
    RECT rc;
    GetClientRect(s_hwndMain, &rc);
    if (s_amGame == AutoMapGame::Wizardry6) W6_OnMouseMotionInMainWindow(rc, newX, newY);
    if (s_amGame == AutoMapGame::Wizardry7) W7_OnMouseMotionInMainWindow(rc, newX, newY);

    POINT pt = { newX, newY };
    ClientToScreen(s_hwndMain, &pt);
    SendMessageW(s_hwndTTM, TTM_TRACKPOSITION, 0,
        (LPARAM)MAKELONG(pt.x + 20, pt.y - 30));
    if (!s_trackingMouseM) {
        TRACKMOUSEEVENT tme = { sizeof(tme) };
        tme.hwndTrack = s_hwndMain;
        tme.dwFlags   = TME_LEAVE;
        TrackMouseEvent(&tme);
        s_trackingMouseM = TRUE;
    }
#else
    (void)newX; (void)newY;
#endif
}

// ── InputBox helper (used by note-editing in am_wiz7.cpp) ────────────────────

bool InputBox(const wchar_t* title, const wchar_t* hint, wchar_t* buf, int bufSize)
{
#ifdef WIN32
    CInputBox myinp(GetModuleHandle(nullptr));
    return myinp.ShowInputBox(s_hwndAM, title, hint, buf, bufSize) == IDOK;
#else
    (void)title; (void)hint; (void)buf; (void)bufSize;
    return false;
#endif
}

bool ChooseColorDialog(uint32_t& color, uint32_t* palette)
{
#ifdef WIN32
    CHOOSECOLOR cc = {};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner   = s_hwndAM;
    cc.hInstance   = (HWND)GetModuleHandle(nullptr);
    cc.lpCustColors = (COLORREF*)palette;
    cc.Flags       = CC_ANYCOLOR | CC_FULLOPEN | CC_RGBINIT;
    cc.rgbResult   = color;
    if (ChooseColor(&cc)) {
        color = cc.rgbResult;
        return true;
    }
#else
    (void)color; (void)palette;
#endif
    return false;
}

// ── Bit-array helper (shared between Wiz6 and Wiz7 parsing code) ──────────────

uint16_t GetBitArrayElement(uint8_t* pArray, uint16_t index, uint16_t bitsPerElement)
{
    uint16_t elemOfs   = index * bitsPerElement;
    uint16_t elemShift = elemOfs % 8;
    uint16_t elemMask  = 0xFFFF;
    elemMask >>= 16 - bitsPerElement;
    elemOfs /= 8;
    uint16_t ret = *reinterpret_cast<uint16_t*>(pArray + elemOfs);
    ret >>= elemShift;
    ret &= elemMask;
    return ret;
}

// ── Public: AutoMapDetectGame (dos_execute.cpp) ───────────────────────────────

void AutoMapDetectGame(char* name, uint16_t loadseg, uint16_t headersize)
{
    (void)headersize; // unused — kept for API compatibility
	s_amGame = AutoMapGame::None;

	if (strcasecmp(name, "WROOT.EXE") == 0) {
		s_amGame = AutoMapGame::Wizardry6;
	}
	if (strcasecmp(name, "DS.EXE") == 0 || strcasecmp(name, "DS.OVR") == 0) {
		s_amGame = AutoMapGame::Wizardry7;
	}

	if (s_amGame == AutoMapGame::None) {
		return;
	}

	PhysPt loadaddress = (PhysPt)(loadseg) << 4;

	// ── Wizardry 6 ────────────────────────────────────────────────────────────
	if (s_amGame == AutoMapGame::Wizardry6) {
		// Scan for WMAZE signature
		uint32_t wmaze_offset = 0;
		bool found_wmaze = false;
		for (uint32_t offset = 0; offset < 0x80000; offset++) {
			uint8_t c = 0;
			mem_readb_checked(loadaddress + offset, &c);
			if (c != 'W') continue;
			char buf[6] = {};
			for (int i = 0; i < 5; i++) {
				uint8_t b = 0;
				mem_readb_checked(loadaddress + offset + i, &b);
				buf[i] = b;
			}
			if (strncmp(buf, "WMAZE", 5) == 0) {
				wmaze_offset = offset;
				found_wmaze  = true;
				break;
			}
		}
		if (!found_wmaze) {
			LOG_MSG("AUTOMAP: Wizardry 6 WMAZE not found");
			s_amGame = AutoMapGame::None;
			return;
		}
		// Correct dataseg derived from CheatEngine signature analysis:
		// correct_dataseg = loadaddress + wmaze_offset - 0x57C
		amw6_dataseg_addr = loadaddress + wmaze_offset - 0x57C;
		am_loadaddress    = loadaddress;
		// Set native save path in AppData (avoids Program Files permission issues)
		char* basePath = SDL_GetPrefPath("WizardryAutomap", "Wizardry6");
		if (basePath) { W6_SetSavePath(basePath); SDL_free(basePath); }
		else W6_SetSavePath("./");
		// Load existing visdata and notes if present
		W6_NativeLoadVis();
		W6_NativeLoadNotes();
		LOG_MSG("AUTOMAP: Wizardry 6 detected! WMAZE=0x%X dataseg=0x%08X",
		        wmaze_offset, (unsigned)amw6_dataseg_addr);
		AutoMapCreate();
		return;
	}

	// ── Wizardry 7
	// ──────────────────────────────────────────────────────────── Step 1:
	// scan for VMAZE signature to compute version delta
	uint32_t vmaze_offset = 0;
	bool found_vmaze      = false;
	for (uint32_t offset = 0; offset < 0x80000; offset++) {
		uint8_t c = 0;
		mem_readb_checked(loadaddress + offset, &c);
		if (c != 'V') {
			continue;
		}
		char buf[6] = {};
		for (int i = 0; i < 5; i++) {
			uint8_t b = 0;
			mem_readb_checked(loadaddress + offset + i, &b);
			buf[i] = b;
		}
		if (strncmp(buf, "VMAZE", 5) == 0) {
			vmaze_offset = offset;
			found_vmaze  = true;
			break;
		}
	}
	if (!found_vmaze) {
		s_amGame = AutoMapGame::None;
		return;
	}

	s_versionDelta = (int32_t)vmaze_offset - (int32_t)0x10D78;

	// Step 2: scan for INSERT SAVEGAME to compute dataseg address
	uint32_t savegame_offset = 0;
	bool found_savegame      = false;
	for (uint32_t offset = 0; offset < 0x80000; offset++) {
		uint8_t c = 0;
		mem_readb_checked(loadaddress + offset, &c);
		if (c != 'I') {
			continue;
		}
		char buf[16] = {};
		for (int i = 0; i < 15; i++) {
			uint8_t b = 0;
			mem_readb_checked(loadaddress + offset + i, &b);
			buf[i] = b;
		}
		if (strncmp(buf, "INSERT SAVEGAME", 15) == 0) {
			savegame_offset = offset;
			found_savegame  = true;
			break;
		}
	}

    if (found_savegame) {
		LOG_MSG("AUTOMAP: INSERT SAVEGAME found at offset 0x%X",
		        savegame_offset);
		amw7_dataseg_addr = loadaddress + savegame_offset - 0xB74;
	} else {
		LOG_MSG("AUTOMAP: INSERT SAVEGAME not found at detect time - will scan later");
		amw7_dataseg_addr = loadaddress + vmaze_offset - 0xC18;
	}

	amw7_rus       = (strcasecmp(name, "DS.OVR") == 0);
	am_loadaddress = loadaddress;

	// Set native save path in AppData (avoids Program Files permission issues)
	{
		char* bp = SDL_GetPrefPath("WizardryAutomap", "Wizardry7");
		if (bp) { W7_SetSavePath(bp); SDL_free(bp); } else W7_SetSavePath("./");
	}
	W7_NativeLoadNotes();

	LOG_MSG("AUTOMAP: Wizardry 7 detected! VMAZE=0x%X delta=%d dataseg=0x%08X",
	        vmaze_offset,
	        s_versionDelta,
	        (unsigned)amw7_dataseg_addr);

	AutoMapCreate();
}

// ── Public: AutoMapSetupBreakpoints (dos_execute.cpp) ────────────────────────

void AutoMapSetupBreakpoints()
{
	if (s_amGame != AutoMapGame::Wizardry7) {
		return;
	}

	s_overlayLoaded          = false;
	s_ignoreOverlayCycleOnce = true;

	const int32_t d = s_versionDelta;
	LOG_MSG("AUTOMAP: Setting up breakpoints with delta=%d", d);

	AM_ActivateBreakpoint(AM_AddBreakpoint(am_loadaddress + 0x4179 + d,
	                                       false,
	                                       AMBP_OnOverlayUnload),
	                      true);
	AM_ActivateBreakpoint(AM_AddBreakpoint(am_loadaddress + 0x417B + d, false),
	                      true);
	AM_ActivateBreakpoint(AM_AddBreakpoint(am_loadaddress + 0x425C + d,
	                                       false,
	                                       AMBP_OnOverlayLoad),
	                      true);
	AM_ActivateBreakpoint(AM_AddBreakpoint(am_loadaddress + 0x425D + d, false),
	                      true);
	AM_ActivateBreakpoint(AM_AddBreakpoint(am_loadaddress + 0x412E + d,
	                                       false,
	                                       AMW7BP_OnMouseLock),
	                      true);
	AM_ActivateBreakpoint(AM_AddBreakpoint(am_loadaddress + 0x4134 + d, false),
	                      true);
	AM_ActivateBreakpoint(AM_AddBreakpoint(am_loadaddress + 0x413B + d,
	                                       false,
	                                       AMW7BP_OnMouseUnlock),
	                      true);
	AM_ActivateBreakpoint(AM_AddBreakpoint(am_loadaddress + 0x4141 + d, false),
	                      true);
	AM_ActivateBreakpoint(AM_AddBreakpoint(am_loadaddress + 0x3CB7 + d,
	                                       false,
	                                       AMW7BP_OnBitArrayModification),
	                      true);
	AM_ActivateBreakpoint(AM_AddBreakpoint(am_loadaddress + 0x3CB9 + d, false),
	                      true);
	AM_ActivateBreakpoint(AM_AddBreakpoint(am_loadaddress + 0x3D38 + d,
	                                       false,
	                                       AMW7BP_OnBitArrayModification),
	                      true);
	AM_ActivateBreakpoint(AM_AddBreakpoint(am_loadaddress + 0x3D39 + d, false),
	                      true);
}

// ── Public: AutomapTerminate (dos_execute.cpp) ────────────────────────────────

void AutomapTerminate()
{
    if (s_amGame == AutoMapGame::Wizardry7) {
        AM_ActivateBreakpoints(0, false);
        s_bps.clear();
    }
    s_amGame = AutoMapGame::None;
}

// ── Public: DOS file hooks ────────────────────────────────────────────────────

void AutoMapOnOpenDOSFile(const char* fileName, uint8_t flags, uint16_t entry)
{
    bool isRead  = (flags & 0x03) == 0; // OPEN_READ = 0
    bool isWrite = (flags & 0x03) == 1 || (flags & 0x03) == 2; // OPEN_WRITE=1, OPEN_READWRITE=2
    int  l = (int)strlen(fileName);
    if (l < 1) return;
    int i = l - 1;
    while (i >= 0 && fileName[i] != '\\' && fileName[i] != '/') --i;

    // Overlay detection (Wiz7 only)
    if (isRead) {
        const char* base = &fileName[i + 1];
        int bl = (int)strlen(base);
        if (bl > 4 && strcasecmp(base + bl - 4, ".OVR") == 0)
            if (s_amGame == AutoMapGame::Wizardry7) W7_OnOverlayLoad();
    }

    if (!am_show_automap) return;
    const char* base = &fileName[i + 1];

    if (s_amGame == AutoMapGame::Wizardry6) {
        if (isRead) {
            if (strcasecmp(base, "NEWGAME.DBS") == 0) { W6_NewGame(); return; }
            if (strcasecmp(base, "SAVEGAME.DBS") == 0) {
                amw6_isLoading = true;
                W6_Load(fileName);
                amw6_isLoading = false;
                extern uint32_t amw6_lastLoadTime;
                amw6_lastLoadTime = SDL_GetTicks();
            }
        }
        if (isWrite) {
            if (strcasecmp(base, "SAVEGAME.DBS") == 0) {
                extern uint32_t amw6_lastLoadTime;
                uint32_t now = SDL_GetTicks();
                if (now - amw6_lastLoadTime > 2000) {
                    amw6_isSaving = true;
                    W6_Save(fileName);
                    amw6_isSaving = false;
                } else {
                }
            }
        }
        return;
    }
    if (!isRead) return;
    if (s_amGame == AutoMapGame::Wizardry7) {
        if (strcasecmp(base, "NEWGAME.DBS") == 0) { W7_NewGame(); return; }
        W7_Load(fileName);
    }
    (void)entry;
}

void AutoMapOnCreateDOSFile(const char* fileName)
{
    if (!am_show_automap) return;
    int l = (int)strlen(fileName);
    if (l < 1) return;

    // W6 save is handled in AutoMapOnOpenDOSFile (write-open)
    if (s_amGame == AutoMapGame::Wizardry7)
        W7_Save(fileName);
}

// ── AMBP: Overlay load/unload (called by the breakpoint callbacks) ────────────
// These are large functions identical in logic to the original; they add and
// remove overlay-specific breakpoints depending on the game-state register.
// The implementations are at the bottom because they reference all the
// W7BP_On* forward-declared callbacks above.

void AMBP_OnOverlayUnload()
{
	if (!s_overlayLoaded && !s_ignoreOverlayCycleOnce) {
		LOG_MSG("AUTOMAP: overlay load/unload sequence broken!");
	}

	AM_ActivateOverlayBreakpoints(SegPhys(cs) + reg_eip, false);

	if (s_overlayLoaded) {
		if (s_amGame == AutoMapGame::Wizardry7) {
			const int32_t d = s_versionDelta;
			uint16_t st     = W7GameStateOnOverlayLoad;
			if (st == 5 || st == 6 || st == 25) {
				AM_DeleteBreakpoint(am_loadaddress + 0x5A12 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x5AB4 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xA44A + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xA44F + d);
			}
			if (st == 23 || st == 27) {
				AM_DeleteBreakpoint(am_loadaddress + 0x8ED9 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x8EDE + d);
			}
			if (st == 4 || st == 7) {
				AM_DeleteBreakpoint(am_loadaddress + 0xBC0B + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xBC0F + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xBB89 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xBB8C + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xB32C + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xB6A8 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x80D0 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x80D4 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x7076 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x707A + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x8209 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x820D + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xDFBB + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xE376 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xC2B0 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xC2B4 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x8AAB + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x8AAF + d);
			} else if (st == 12) {
				AM_DeleteBreakpoint(am_loadaddress + 0x8C90 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x8C91 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x8BF0 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x8BF1 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x934F + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x9350 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x7997 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x799A + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x79A6 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x79A9 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x85C8 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x85CB + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x85DB + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x85DE + d);
			} else if (st == 18) {
				AM_DeleteBreakpoint(am_loadaddress + 0xBACB + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xBAD1 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xBAD4 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xBADA + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xAF83 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xAF89 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xAF90 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xAF9F + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xAFB0 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xAFBC + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xB4AC + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xB4B8 + d);
			} else if (st == 19 || st == 20) {
				AM_DeleteBreakpoint(am_loadaddress + 0x9638 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x963F + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x9352 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x9359 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x6D1C + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x6D1F + d);
			} else if (st == 15 || st == 21) {
				AM_DeleteBreakpoint(am_loadaddress + 0xA0F5 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xA0F8 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xA102 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xA105 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xA1B6 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xA1BC + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xA1A7 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xA1AD + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xA398 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xA551 + d);
			} else if (st == 24) {
				AM_DeleteBreakpoint(am_loadaddress + 0xB9D9 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xB9DD + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xB9E5 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xB9E9 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x8AAB + d);
				AM_DeleteBreakpoint(am_loadaddress + 0x8AAF + d);
			} else if (st == 26) {
				AM_DeleteBreakpoint(am_loadaddress + 0xC270 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xC271 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xC315 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xC318 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xC337 + d);
				AM_DeleteBreakpoint(am_loadaddress + 0xC33A + d);
			}
		}
	}
	s_ignoreOverlayCycleOnce = false;
	s_overlayLoaded          = false;
}

void AMBP_OnOverlayLoad()
{
	if (s_overlayLoaded) {
		LOG_MSG("AUTOMAP: overlay load/unload sequence broken (double load)!");
	}

	if (s_amGame == AutoMapGame::Wizardry7) {
		const int32_t d = s_versionDelta;
		uint16_t st     = W7GameStateOnOverlayLoad;
		LOG_MSG("AUTOMAP: OnOverlayLoad state=%d", (int)st);
		if (st == 5 || st == 6 || st == 25) {
			AM_AddBreakpoint(am_loadaddress + 0x5A12 + d,
			                 true,
			                 W7BP_OnMapLoadStart);
			AM_AddBreakpoint(am_loadaddress + 0x5AB4 + d,
			                 true,
			                 W7BP_OnMapLoadEnd);
			AM_AddBreakpoint(am_loadaddress + 0xA44A + d,
			                 true,
			                 W7BP_OnMapRedraw);
			AM_AddBreakpoint(am_loadaddress + 0xA44F + d, true);
		}
		if (st == 23 || st == 27) {
			AM_AddBreakpoint(am_loadaddress + 0x8ED9 + d,
			                 true,
			                 W7BP_OnMapRedraw);
			AM_AddBreakpoint(am_loadaddress + 0x8EDE + d, true);
		}
		if (st == 4 || st == 7) {
			AM_AddBreakpoint(am_loadaddress + 0xBC0B + d,
			                 true,
			                 W7BP_OnShowConfigWindow);
			AM_AddBreakpoint(am_loadaddress + 0xBC0F + d,
			                 true,
			                 W7BP_OnHideConfigWindow);
			AM_AddBreakpoint(am_loadaddress + 0xBB89 + d,
			                 true,
			                 W7BP_OnShowAddCharacterWindow);
			AM_AddBreakpoint(am_loadaddress + 0xBB8C + d,
			                 true,
			                 W7BP_OnHideAddCharacterWindow);
			AM_AddBreakpoint(am_loadaddress + 0xB32C + d,
			                 true,
			                 W7BP_OnShowSecurityWindow);
			AM_AddBreakpoint(am_loadaddress + 0xB6A8 + d,
			                 true,
			                 W7BP_OnHideSecurityWindow);
			AM_AddBreakpoint(am_loadaddress + 0x80D0 + d,
			                 true,
			                 W7BP_OnShowLoadWindow);
			AM_AddBreakpoint(am_loadaddress + 0x80D4 + d,
			                 true,
			                 W7BP_OnHideLoadWindow);
			AM_AddBreakpoint(am_loadaddress + 0x7076 + d,
			                 true,
			                 W7BP_OnShowImportWindow);
			AM_AddBreakpoint(am_loadaddress + 0x707A + d,
			                 true,
			                 W7BP_OnHideImportWindow);
			AM_AddBreakpoint(am_loadaddress + 0x8209 + d,
			                 true,
			                 W7BP_OnOpenSaveFileStart);
			AM_AddBreakpoint(am_loadaddress + 0x820D + d,
			                 true,
			                 W7BP_OnOpenSaveFileEnd);
			AM_AddBreakpoint(am_loadaddress + 0xDFBB + d,
			                 true,
			                 W7BP_OnIntroStart);
			AM_AddBreakpoint(am_loadaddress + 0xE376 + d,
			                 true,
			                 W7BP_OnIntroEnd);
			AM_AddBreakpoint(am_loadaddress + 0xC2B0 + d,
			                 true,
			                 W7BP_OnShowSaveWindow);
			AM_AddBreakpoint(am_loadaddress + 0xC2B4 + d,
			                 true,
			                 W7BP_OnHideSaveWindow);
			AM_AddBreakpoint(am_loadaddress + 0x8AAB + d,
			                 true,
			                 W7BP_OnCreateSaveFileStart);
			AM_AddBreakpoint(am_loadaddress + 0x8AAF + d,
			                 true,
			                 W7BP_OnCreateSaveFileEnd);
		} else if (st == 12) {
			AM_AddBreakpoint(am_loadaddress + 0x8C90 + d,
			                 true,
			                 W7BP_OnShowEnemyList);
			AM_AddBreakpoint(am_loadaddress + 0x8C91 + d, true);
			AM_AddBreakpoint(am_loadaddress + 0x8BF0 + d,
			                 true,
			                 W7BP_OnShowEnemyList);
			AM_AddBreakpoint(am_loadaddress + 0x8BF1 + d, true);
			AM_AddBreakpoint(am_loadaddress + 0x934F + d,
			                 true,
			                 W7BP_OnHideEnemyList);
			AM_AddBreakpoint(am_loadaddress + 0x9350 + d, true);
			AM_AddBreakpoint(am_loadaddress + 0x7997 + d,
			                 true,
			                 W7BP_OnShowSpellWindow);
			AM_AddBreakpoint(am_loadaddress + 0x799A + d, true);
			AM_AddBreakpoint(am_loadaddress + 0x79A6 + d,
			                 true,
			                 W7BP_OnHideSpellWindow);
			AM_AddBreakpoint(am_loadaddress + 0x79A9 + d, true);
			AM_AddBreakpoint(am_loadaddress + 0x85C8 + d,
			                 true,
			                 W7BP_OnShowUseWindow);
			AM_AddBreakpoint(am_loadaddress + 0x85CB + d, true);
			AM_AddBreakpoint(am_loadaddress + 0x85DB + d,
			                 true,
			                 W7BP_OnHideUseWindow);
			AM_AddBreakpoint(am_loadaddress + 0x85DE + d, true);
		} else if (st == 18) {
			AM_AddBreakpoint(am_loadaddress + 0xBACB + d,
			                 true,
			                 W7BP_OnShowSpellWindow);
			AM_AddBreakpoint(am_loadaddress + 0xBAD1 + d,
			                 true,
			                 W7BP_OnHideSpellWindow);
			AM_AddBreakpoint(am_loadaddress + 0xBAD4 + d,
			                 true,
			                 W7BP_OnShowUseWindow);
			AM_AddBreakpoint(am_loadaddress + 0xBADA + d,
			                 true,
			                 W7BP_OnHideUseWindow);
			AM_AddBreakpoint(am_loadaddress + 0xAF83 + d,
			                 true,
			                 W7BP_OnShowBuyWindow);
			AM_AddBreakpoint(am_loadaddress + 0xAF89 + d,
			                 true,
			                 W7BP_OnHideBuyWindow);
			AM_AddBreakpoint(am_loadaddress + 0xAF90 + d,
			                 true,
			                 W7BP_OnShowSellWindow);
			AM_AddBreakpoint(am_loadaddress + 0xAF9F + d,
			                 true,
			                 W7BP_OnHideSellWindow);
			AM_AddBreakpoint(am_loadaddress + 0xAFB0 + d,
			                 true,
			                 W7BP_OnShowGiveWindow);
			AM_AddBreakpoint(am_loadaddress + 0xAFBC + d,
			                 true,
			                 W7BP_OnHideGiveWindow);
			AM_AddBreakpoint(am_loadaddress + 0xB4AC + d,
			                 true,
			                 W7BP_OnShowGiveWindow);
			AM_AddBreakpoint(am_loadaddress + 0xB4B8 + d,
			                 true,
			                 W7BP_OnHideGiveWindow);
		} else if (st == 19 || st == 20) {
			AM_AddBreakpoint(am_loadaddress + 0x9638 + d,
			                 true,
			                 W7BP_OnShowSpellWindow);
			AM_AddBreakpoint(am_loadaddress + 0x963F + d,
			                 true,
			                 W7BP_OnHideSpellWindow);
			AM_AddBreakpoint(am_loadaddress + 0x9352 + d,
			                 true,
			                 W7BP_OnShowUseWindow);
			AM_AddBreakpoint(am_loadaddress + 0x9359 + d,
			                 true,
			                 W7BP_OnHideUseWindow);
			AM_AddBreakpoint(am_loadaddress + 0x6D1C + d,
			                 true,
			                 W7BP_OnCastSpell);
			AM_AddBreakpoint(am_loadaddress + 0x6D1F + d, true);
		} else if (st == 15 || st == 21) {
			AM_AddBreakpoint(am_loadaddress + 0xA0F5 + d,
			                 true,
			                 W7BP_OnShowDisarmWindow);
			AM_AddBreakpoint(am_loadaddress + 0xA0F8 + d, true);
			AM_AddBreakpoint(am_loadaddress + 0xA102 + d,
			                 true,
			                 W7BP_OnHideDisarmWindow);
			AM_AddBreakpoint(am_loadaddress + 0xA105 + d, true);
			AM_AddBreakpoint(am_loadaddress + 0xA1B6 + d,
			                 true,
			                 W7BP_OnShowUseWindow);
			AM_AddBreakpoint(am_loadaddress + 0xA1BC + d,
			                 true,
			                 W7BP_OnHideUseWindow);
			AM_AddBreakpoint(am_loadaddress + 0xA1A7 + d,
			                 true,
			                 W7BP_OnShowSpellWindow);
			AM_AddBreakpoint(am_loadaddress + 0xA1AD + d,
			                 true,
			                 W7BP_OnHideSpellWindow);
			AM_AddBreakpoint(am_loadaddress + 0xA398 + d,
			                 true,
			                 W7BP_OnShowLootWindow);
			AM_AddBreakpoint(am_loadaddress + 0xA551 + d,
			                 true,
			                 W7BP_OnHideLootWindow);
		} else if (st == 24) {
			AM_AddBreakpoint(am_loadaddress + 0xB9D9 + d,
			                 true,
			                 W7BP_OnShowConfigWindow);
			AM_AddBreakpoint(am_loadaddress + 0xB9DD + d,
			                 true,
			                 W7BP_OnHideConfigWindow);
			AM_AddBreakpoint(am_loadaddress + 0xB9E5 + d,
			                 true,
			                 W7BP_OnShowSaveWindow);
			AM_AddBreakpoint(am_loadaddress + 0xB9E9 + d,
			                 true,
			                 W7BP_OnHideSaveWindow);
			AM_AddBreakpoint(am_loadaddress + 0x8AAB + d,
			                 true,
			                 W7BP_OnCreateSaveFileStart);
			AM_AddBreakpoint(am_loadaddress + 0x8AAF + d,
			                 true,
			                 W7BP_OnCreateSaveFileEnd);
		} else if (st == 26) {
			AM_AddBreakpoint(am_loadaddress + 0xC270 + d,
			                 true,
			                 W7BP_OnHideUnlockWindow);
			AM_AddBreakpoint(am_loadaddress + 0xC271 + d, true);
			AM_AddBreakpoint(am_loadaddress + 0xC315 + d,
			                 true,
			                 W7BP_OnShowUnlockWindow);
			AM_AddBreakpoint(am_loadaddress + 0xC318 + d, true);
			AM_AddBreakpoint(am_loadaddress + 0xC337 + d,
			                 true,
			                 W7BP_OnShowUnlockWindow);
			AM_AddBreakpoint(am_loadaddress + 0xC33A + d, true);
		}
	}
	s_overlayLoaded = true;
}

// Accessor so am_wiz7.cpp can call SDL_RenderDrawRect directly for note borders.
// Declared extern in the generated replacement block inside am_wiz7.cpp.
SDL_Renderer* AM_GetRenderer() { return s_amRenderer; }
