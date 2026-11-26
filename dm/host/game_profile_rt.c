// Runtime game profile loader (generic host): reads a game.tsv-format profile file into g_game (game_profile_rt.h).
//
// The file is the same key TAB values... format dm/games/<id>/game.tsv uses, so the PC tools and the on-device porting engine write it identically.
// Lines starting with '#' are comments; blank lines are skipped. A key may repeat (build, display_version, modules in the tsv) or carry several
// space-separated values on one line (the shader key list). Keys the host does not read are ignored, so a profile may carry extra rows.
//
// The list keys map to the struct's parallel arrays:
//   modules         → modules[] / nmod (space-separated on one line, or repeated lines)
//   build           → build_id[]/build_mods[] / nbuild (each line: <build ID> <module...>; the first token is the build, the rest the modules)
//   display_version → ver_build[]/ver_name[] / nver (each line: <build ID> <version>)
// Everything else is a single value, parsed by type (string, hex/uint64, int).
#include "game_profile_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

dm_game_t g_game;

const dm_game_t *dm_game_cur(void) { return &g_game; }

// Copy a C string into a freshly allocated buffer the struct holds (the struct's string fields point at owned storage).
static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

// Free the strings the previous load allocated. Called at the start of a (re)load.
static void profile_clear(dm_game_t *g) {
    const char *strs[] = {g->id, g->name, g->romfs_marker, g->args, g->env, g->launch_param, g->ue_fps_cvar, g->upscaler_keys};
    for (size_t i = 0; i < sizeof strs / sizeof *strs; i++) free((void *)strs[i]);
    for (int i = 0; i < 16; i++) free((void *)g->modules[i]);
    for (int i = 0; i < 8; i++) { free((void *)g->build_id[i]); free((void *)g->build_mods[i]); free((void *)g->ver_build[i]); free((void *)g->ver_name[i]); }
    memset(g, 0, sizeof *g);
    g->shared_font_ttf = -1;      // not set: derive from the SDK (hle_sdk16.c)
    g->nvn_api_major = -1;        // not set: derive from the SDK (hle_nvn.c)
    g->nvn_api_minor = -1;
    // game.py header_one's defaults, for keys a profile may omit
    g->fps_max = 60;              // fps_max: 60 unless the profile says 120
    g->fps_hold = 1;              // fps_hold: on
    g->opmode = 1;                // op_mode: console
    g->ue_fps_cvar = xstrdup("t.MaxFPS");   // ue_fps_cvar: Unreal's frame-limit console variable
}

static uint64_t parse_u64(const char *s) { return s ? strtoull(s, NULL, 0) : 0; }
static int parse_int(const char *s) { return s ? (int)strtol(s, NULL, 0) : 0; }

// Append a token to a string array (each entry owned). Caps at cap entries.
static void arr_add(const char **arr, int *n, int cap, const char *tok) {
    if (!tok || !*tok || *n >= cap) return;
    arr[(*n)++] = xstrdup(tok);
}

int dm_game_load(const char *path) {
    if (!path || !*path) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    profile_clear(&g_game);

    char line[2048];
    int nmod = 0, nbuild = 0, nver = 0;
    while (fgets(line, sizeof line, f)) {
        // strip the newline; skip blanks and comments
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        char *hash = strchr(line, '#'); if (hash) *hash = 0;
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (!*p) continue;
        // key is up to the first tab (or the whole line when there is no tab)
        char *tab = strchr(p, '\t');
        char key[64];
        if (tab) { size_t k = (size_t)(tab - p); if (k >= sizeof key) k = sizeof key - 1; memcpy(key, p, k); key[k] = 0; p = tab + 1; }
        else { snprintf(key, sizeof key, "%s", p); p = ""; }
        while (*p == ' ' || *p == '\t') p++;

        if (!strcmp(key, "modules")) {
            // space-separated module names on the value line
            char *save = NULL;
            for (char *t = strtok_r(p, " \t", &save); t; t = strtok_r(NULL, " \t", &save)) arr_add(g_game.modules, &nmod, 16, t);
        } else if (!strcmp(key, "build")) {
            // build <version> <display version> <module>=<build ID>...: the version is the row's label (not used by the host), the display
            // version answers GetDisplayVersion, and the main module's build ID is what profile_modules matches against main.fixups.tsv.
            char *save = NULL;
            char *ver = strtok_r(p, " \t", &save);        // the version label (v0, v65536, ...)
            char *disp = strtok_r(NULL, " \t", &save);    // the display version
            char mods[256] = ""; size_t ml = 0; char main_bid[40] = "";
            for (char *m = strtok_r(NULL, " \t", &save); m; m = strtok_r(NULL, " \t", &save)) {
                char *eq = strchr(m, '=');
                if (eq) {
                    *eq = 0;                       // m is the module name, eq+1 its build ID
                    if (!strcmp(m, "main")) snprintf(main_bid, sizeof main_bid, "%s", eq + 1);
                    size_t l = strlen(m); if (ml + l < sizeof mods - 1) { if (ml) mods[ml++] = ' '; memcpy(mods + ml, m, l); ml += l; }
                }
            }
            mods[ml] = 0;
            // the row's build ID is main's; the modules are the names (main sdk); the display version pairs with the build ID.
            // one row index for the parallel build_id/build_mods arrays, and the same for ver_build/ver_name.
            if (nbuild < 8) {
                g_game.build_id[nbuild] = xstrdup(main_bid[0] ? main_bid : (ver ? ver : ""));
                g_game.build_mods[nbuild] = xstrdup(mods);
                nbuild++;
            }
            if (main_bid[0] && nver < 8) {
                g_game.ver_build[nver] = xstrdup(main_bid);
                g_game.ver_name[nver] = xstrdup(disp ? disp : "1.0.0");
                nver++;
            }
        } else if (!strcmp(key, "id") || !strcmp(key, "title_id")) g_game.id = xstrdup(p);
        else if (!strcmp(key, "name")) g_game.name = xstrdup(p);
        else if (!strcmp(key, "program_id")) g_game.program_id = parse_u64(p);
        else if (!strcmp(key, "romfs_marker")) g_game.romfs_marker = xstrdup(p);
        else if (!strcmp(key, "args")) g_game.args = xstrdup(p);
        else if (!strcmp(key, "env")) g_game.env = xstrdup(p);
        else if (!strcmp(key, "launch_param")) g_game.launch_param = xstrdup(p);
        else if (!strcmp(key, "mem_reserve")) g_game.mem_reserve = parse_u64(p);
        else if (!strcmp(key, "mem_alias")) g_game.mem_alias = parse_u64(p);
        else if (!strcmp(key, "mem_stack")) g_game.mem_stack = parse_u64(p);
        else if (!strcmp(key, "app_pool")) g_game.app_pool = parse_u64(p);
        else if (!strcmp(key, "mem_used")) g_game.mem_used = parse_u64(p);
        else if (!strcmp(key, "sys_resource")) g_game.sys_resource = parse_u64(p);
        else if (!strcmp(key, "stdalloc_cap")) g_game.stdalloc_cap = parse_u64(p);
        else if (!strcmp(key, "mem_map_limit")) g_game.mem_map_limit = parse_int(p);
        else if (!strcmp(key, "screen_aspect")) g_game.screen_aspect = parse_int(p);
        else if (!strcmp(key, "core_preempt")) { const char *m = p; g_game.core_preempt = (m && !strcmp(m, "on")) ? 1 : 0; }
        else if (!strcmp(key, "alloc_big_align")) { const char *m = p; g_game.alloc_big_align = (m && !strcmp(m, "on")) ? 1 : 0; }
        else if (!strcmp(key, "fps_target")) g_game.fps_target = parse_int(p);
        else if (!strcmp(key, "fps_max")) g_game.fps_max = parse_int(p);
        else if (!strcmp(key, "fps_hold")) g_game.fps_hold = parse_int(p);
        else if (!strcmp(key, "ue_fps_cvar")) { free((void *)g_game.ue_fps_cvar); g_game.ue_fps_cvar = xstrdup(p); }
        else if (!strcmp(key, "ue_texture_pool")) g_game.ue_texture_pool = parse_int(p);
        else if (!strcmp(key, "present_interval")) { /* present_display is derived from 'display' below */ if (p && !strcmp(p, "display")) g_game.present_display = 1; }
        else if (!strcmp(key, "present_snap")) g_game.present_snap = parse_int(p);
        else if (!strcmp(key, "pso_mtl4")) g_game.pso_mtl4 = parse_int(p);
        else if (!strcmp(key, "enc_thread")) g_game.enc_thread = parse_int(p);
        else if (!strcmp(key, "pad_layout")) {
            const char *m = p; g_game.pad_layout = (!m || !strcmp(m, "simple")) ? 0 : (!strcmp(m, "full") ? 1 : 2);
        }
        else if (!strcmp(key, "op_mode")) { const char *m = p; g_game.opmode = (!m || !strcmp(m, "console")) ? 1 : 0; }
        else if (!strcmp(key, "perf_mode")) { const char *m = p; g_game.perf_boost = (m && !strcmp(m, "boost")) ? 1 : 0; }
        else if (!strcmp(key, "tex_flush_upload")) g_game.tex_flush_upload = parse_int(p);
        else if (!strcmp(key, "tex_dirty_upload")) g_game.tex_dirty_upload = parse_int(p);
        else if (!strcmp(key, "tex_dyn_slots")) g_game.tex_dyn_slots = parse_int(p);
        else if (!strcmp(key, "render_cond")) g_game.render_cond = parse_int(p);
        else if (!strcmp(key, "gpu_timestamps")) { const char *m = p; g_game.gpu_ts_segment = (m && !strcmp(m, "segment")) ? 1 : 0; }
        else if (!strcmp(key, "draw_barriers")) { const char *m = p; g_game.draw_barriers_game = (m && !strcmp(m, "game")) ? 1 : 0; }
        else if (!strcmp(key, "vconv_range")) g_game.vconv_range = parse_int(p);
        else if (!strcmp(key, "upscaler_keys")) g_game.upscaler_keys = xstrdup(p);
        else if (!strcmp(key, "shader_key_skip")) {
            // <offset>+<length>
            unsigned off = 0, len = 0;
            if (sscanf(p, "%u+%u", &off, &len) == 2) { g_game.shader_key_skip_off = (int)off; g_game.shader_key_skip_len = (int)len; }
        }
        else if (!strcmp(key, "shared_font")) { const char *m = p; g_game.shared_font_ttf = (!m || !strcmp(m, "bfttf")) ? 0 : 8; }
        else if (!strcmp(key, "device_api")) {
            // <major> <minor>
            int ma = 0, mi = 0;
            if (sscanf(p, "%d %d", &ma, &mi) == 2) { g_game.nvn_api_major = ma; g_game.nvn_api_minor = mi; }
        }
        // else: a key the host does not read — ignored
    }
    fclose(f);
    g_game.nmod = nmod; g_game.nbuild = nbuild; g_game.nver = nver;
    // a profile with no modules cannot load anything: reject it as invalid
    if (nmod <= 0 || !g_game.id) { profile_clear(&g_game); return -1; }
    return 0;
}
