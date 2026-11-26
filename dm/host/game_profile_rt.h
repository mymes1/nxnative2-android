// Runtime game profile (generic host): the constants the host used to take from the compile-time game_profile.h (dm/games/game.py header), read at
// start-up from a profile file in the game's folder instead. One host (libdmhost.so) runs every game: the loader (loader.c) starts it for the game the
// launcher picked and passes the game's directory; the host reads <dir>/profile.txt here, fills g_game, and everything that used a DM_GAME_* macro now
// reads the field. This is what makes on-device porting possible: the phone produces the pack (profile + rewritten modules + RomFS + shaders) and the
// single host runs it, with no per-game host compiled on a computer.
//
// The profile file is the game.tsv format (key TAB values..., '#' comments), the same one dm/games/<id>/game.tsv uses, so the PC tools (game.py) and the
// on-device porting engine write it the same way. Scalars are one value; the three list keys repeat (modules, build, display_version) or carry
// space-separated values (the shader key list). See game_profile_rt.c for the parser and game.py rtprofile for the writer.
#pragma once
#include <stdint.h>
#include <stddef.h>

// NOTE: DM_GAME_MULTI is intentionally NOT defined here. The generic (runtime-profile) host is
// single-profile: dm_game_cur() is provided by game_profile_rt.c, and the #ifdef DM_GAME_MULTI
// blocks in main.c / hle_oe.c compile out (their multi-program table dm_games[] does not exist).

// One game's profile, the fields the host reads. Types match game.py header_one's emission (strings, uint64 hex, int).
typedef struct {
    // identity
    const char *id;            // DM_GAME_ID: the base title ID (16 hex)
    const char *name;          // DM_GAME_NAME
    uint64_t program_id;       // DM_GAME_PROGRAM_ID
    // modules (the code modules to load, in load order)
    const char *modules[16];   // DM_GAME_MODULES
    int nmod;                  // DM_GAME_NMOD
    // build rows: {build ID, the modules that build loads} (main.c profile_modules)
    const char *build_id[8];
    const char *build_mods[8];
    int nbuild;
    // display versions: {build ID, display version} (hle_sdk16.c, hle_cheats.c)
    const char *ver_build[8];
    const char *ver_name[8];
    int nver;
    // romfs / launch
    const char *romfs_marker;  // DM_GAME_ROMFS_MARKER
    const char *args;          // DM_GAME_ARGS
    const char *env;           // DM_GAME_ENV
    const char *launch_param;  // DM_GAME_LAUNCH_PARAM
    // memory (hle_svc.c)
    uint64_t mem_reserve;      // DM_GAME_MEM_RESERVE
    uint64_t mem_alias;        // DM_GAME_MEM_ALIAS
    uint64_t mem_stack;        // DM_GAME_MEM_STACK
    uint64_t app_pool;         // DM_GAME_APP_POOL
    uint64_t mem_used;         // DM_GAME_MEM_USED
    uint64_t sys_resource;     // DM_GAME_SYS_RESOURCE
    uint64_t stdalloc_cap;     // DM_GAME_STDALLOC_CAP
    int mem_map_limit;         // DM_GAME_MEM_MAP_LIMIT
    int screen_aspect;         // DM_GAME_SCREEN_ASPECT
    int core_preempt;          // DM_GAME_CORE_PREEMPT
    int alloc_big_align;       // DM_GAME_ALLOC_BIG_ALIGN
    // timing / frame step (loader.c, hle_fpslock.c, hle_nvn.c)
    int fps_target;            // DM_GAME_FPS_TARGET
    int fps_max;               // DM_GAME_FPS_MAX
    int fps_hold;              // DM_GAME_FPS_HOLD
    const char *ue_fps_cvar;   // DM_GAME_UE_FPS_CVAR
    int ue_texture_pool;       // DM_GAME_UE_TEXTURE_POOL
    int present_display;       // DM_GAME_PRESENT_DISPLAY
    int present_snap;          // DM_GAME_PRESENT_SNAP
    int pso_mtl4;              // DM_GAME_PSO_MTL4
    int enc_thread;            // DM_GAME_ENC_THREAD
    // input / operation (front_android.c, hle_oe.c)
    int pad_layout;            // DM_GAME_PAD_LAYOUT
    int opmode;                // DM_GAME_OPMODE
    int perf_boost;            // DM_GAME_PERF_BOOST
    // nvn / graphics behaviour (hle_nvn.c, nvn_mtl.m, nvn_rec.h, nvn_capture.h)
    int tex_flush_upload;      // DM_GAME_TEX_FLUSH_UPLOAD
    int tex_dirty_upload;      // DM_GAME_TEX_DIRTY_UPLOAD
    int tex_dyn_slots;         // DM_GAME_TEX_DYN_SLOTS
    int render_cond;           // DM_GAME_RENDER_COND
    int gpu_ts_segment;        // DM_GAME_GPU_TS_SEGMENT
    int draw_barriers_game;    // DM_GAME_DRAW_BARRIERS_GAME
    int vconv_range;           // DM_GAME_VCONV_RANGE
    const char *upscaler_keys; // DM_GAME_UPSCALER_KEYS
    int upscaler_n;            // DM_GAME_UPSCALER_N
    int shader_key_skip_off;   // DM_GAME_SHADER_KEY_SKIP_OFF
    int shader_key_skip_len;   // DM_GAME_SHADER_KEY_SKIP_LEN
    // sdk / device api (hle_sdk16.c, hle_nvn.c): present as runtime values now (were #ifdef'd)
    int shared_font_ttf;        // DM_GAME_SHARED_FONT_TTF (8 = the TTF itself, 0 = BFTTF header + TTF); -1 = not set (derive from the SDK)
    int nvn_api_major;          // DM_GAME_NVN_API_MAJOR; -1 = not set (derive from the SDK)
    int nvn_api_minor;          // DM_GAME_NVN_API_MINOR
} dm_game_t;

extern dm_game_t g_game;   // the loaded profile, filled by dm_game_load()

// Read <path> (a game.tsv profile file) into g_game. Returns 0 on success, -1 on a missing/invalid file (the caller decides whether that is fatal).
// Idempotent: a second call reloads. Safe to call once at host start, before anything reads the profile.
int dm_game_load(const char *path);

// The loaded profile (dm_game_cur in the multi form): always g_game. Kept so the DM_GAME_MULTI code paths (main.c, hle_oe.c) compile unchanged.
const dm_game_t *dm_game_cur(void);

// ---- Scalar macro shims: the DM_GAME_* names the host sources use, mapped to the runtime profile ----
// These let every DM_GAME_* scalar reference in dm/host and dm/gfx compile unchanged against g_game.
// The three list macros (DM_GAME_MODULES, DM_GAME_BUILD_MODULES, DM_GAME_DISPLAY_VERSIONS) are NOT
// defined here: they were brace-initializer lists in the compile-time header. Their (few) usage sites
// read the g_game arrays directly instead (main.c profile_modules, hle_sdk16.c, hle_cheats.c).
#define DM_GAME_ID            g_game.id
#define DM_GAME_NAME          g_game.name
#define DM_GAME_PROGRAM_ID    g_game.program_id
#define DM_GAME_ROMFS_MARKER  g_game.romfs_marker
#define DM_GAME_NMOD          g_game.nmod
#define DM_GAME_ROMFS_MARKER  g_game.romfs_marker
#define DM_GAME_ARGS          g_game.args
#define DM_GAME_ENV           g_game.env
#define DM_GAME_LAUNCH_PARAM  g_game.launch_param
#define DM_GAME_MEM_RESERVE   g_game.mem_reserve
#define DM_GAME_MEM_ALIAS     g_game.mem_alias
#define DM_GAME_MEM_STACK     g_game.mem_stack
#define DM_GAME_APP_POOL      g_game.app_pool
#define DM_GAME_MEM_USED      g_game.mem_used
#define DM_GAME_SYS_RESOURCE  g_game.sys_resource
#define DM_GAME_STDALLOC_CAP  g_game.stdalloc_cap
#define DM_GAME_MEM_MAP_LIMIT g_game.mem_map_limit
#define DM_GAME_SCREEN_ASPECT g_game.screen_aspect
#define DM_GAME_CORE_PREEMPT  g_game.core_preempt
#define DM_GAME_ALLOC_BIG_ALIGN g_game.alloc_big_align
#define DM_GAME_FPS_TARGET    g_game.fps_target
#define DM_GAME_FPS_MAX       g_game.fps_max
#define DM_GAME_FPS_HOLD      g_game.fps_hold
#define DM_GAME_UE_FPS_CVAR   g_game.ue_fps_cvar
#define DM_GAME_UE_TEXTURE_POOL g_game.ue_texture_pool
#define DM_GAME_PRESENT_DISPLAY g_game.present_display
#define DM_GAME_PRESENT_SNAP  g_game.present_snap
#define DM_GAME_PSO_MTL4      g_game.pso_mtl4
#define DM_GAME_ENC_THREAD    g_game.enc_thread
#define DM_GAME_PAD_LAYOUT    g_game.pad_layout
#define DM_GAME_OPMODE        g_game.opmode
#define DM_GAME_PERF_BOOST    g_game.perf_boost
#define DM_GAME_TEX_FLUSH_UPLOAD g_game.tex_flush_upload
#define DM_GAME_TEX_DIRTY_UPLOAD g_game.tex_dirty_upload
#define DM_GAME_TEX_DYN_SLOTS g_game.tex_dyn_slots
#define DM_GAME_RENDER_COND   g_game.render_cond
#define DM_GAME_GPU_TS_SEGMENT g_game.gpu_ts_segment
#define DM_GAME_DRAW_BARRIERS_GAME g_game.draw_barriers_game
#define DM_GAME_VCONV_RANGE   g_game.vconv_range
#define DM_GAME_UPSCALER_KEYS g_game.upscaler_keys
#define DM_GAME_UPSCALER_N    g_game.upscaler_n
#define DM_GAME_SHADER_KEY_SKIP_OFF g_game.shader_key_skip_off
#define DM_GAME_SHADER_KEY_SKIP_LEN g_game.shader_key_skip_len
#define DM_GAME_SHARED_FONT_TTF g_game.shared_font_ttf
#define DM_GAME_NVN_API_MAJOR g_game.nvn_api_major
#define DM_GAME_NVN_API_MINOR g_game.nvn_api_minor
