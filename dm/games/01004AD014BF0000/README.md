# Sonic Frontiers (`01004AD014BF0000`)

The base game (Switch 1): v1.0.0, SDK built for the Switch (`Horizon`), **NVN** — Hedgehog Engine 2 on the
Switch 1's classic NVN API (the Definitive Edition, `0400921025B32000`, is the same engine family on the
Switch 2's NVN2). SEGA / Sonic Team's engine (`hh::needle`, `hh::gfx`; its allocators log as `Rsdx`), with
CRIWARE (CRI Atom / AtomEx sound, CRI Mana movies). Title IDs: `01004AD014BF0000` (US/EU),
`010059C014C40000` (JP/Asia).

**State: scaffolded, waiting for the NSP.** This folder holds the profile's notes; `game.tsv` and the shader
tables are written from the user's NSP (below). There is no upstream profile for this title — the profile is
created here, adapted from the Definitive Edition's where the engine is the same.

## What is known (web-verified)

| | |
|---|---|
| Base build | v1.0.0, main build ID `432d4198d8c563c5e5c2f669410ea643` (full 32-char hash; the 8-char prefix `432D4198D8C563C5` is what cheat sites list) |
| Update 1.0.1 | main build ID `62588a30606b14fbac39a1a9a990d0686` |
| Later updates | 1.1.0, 1.1.1 (`DD7FF70188B281D9` prefix), up to 1.4.1 (NSP package v458752) — the user's update NSP decides which row the profile carries |
| Sizes | base ~10.6 GB NSP, update ~5 GB; base firmware 13.0.0, update 17.0.0 |

The build IDs above are the **main** module hashes only. The `sdk` (and `subsdk0`, if the profile loads it)
hashes come from the NSP's own modules — the survey reads them (16 bytes at NSO offset `0x40`, hex-encoded).

## Why the profile is not a `game.tsv` yet

A registered profile's shader tables (`shader_map.tsv`, `shader_facts.tsv`, `shader_pairs.tsv`, …) must be
**byte-identical by data rows** to what `shader_split.py` produces from the game's own files — a mismatch
stops the build (`BuildError`, "the game is packed without shaders"). Those tables can only come from the
base game's RomFS and `main`, so they are split from the user's NSP. Until then this folder carries no
`game.tsv`: the loader skips a folder without one, and `scripts/build_android_from_nsp.py` surveys the NSP
and packs from a **provisional profile** outside the repository (with the split's tables taken as they come).

## Completing the profile (with the user's NSP) — one command

Drop the base NSP and (optionally) the update NSP in one folder, e.g. `nsp/01004AD014BF0000/`, then run the
Android pipeline with `--register`:

```
python3 scripts/build_android_from_nsp.py nsp/01004AD014BF0000 -k prod.keys --no-push --register --gpu mali-g57-mc2
```

One run does the whole thing:

1. **Survey** — the base NSP and the newest update NSP are surveyed (code modules, RomFS manifest, rewrite
   census, imports) and the report is written to `<folder>/01004AD014BF0000_survey/survey.md`.
2. **Complete the profile** — the survey's provisional profile (real build hashes, the memory partition, the
   marker, the shader groups) is written, the shader tables are split from the game's own files, and
   `--register` promotes it into this folder (`game.tsv` + the shader tables), so the game is registered and
   later builds check its tables.
3. **Port** — `nx_extract`, the module rewrite, the shader split + `spv_build.py --gpu mali-g57-mc2` (the
   Mali pack-time pass), and the Android host.
4. **Package the APK** — `--no-push` stops after the APK (no phone needed); drop `--no-push` to install it
   and copy the game files to a connected phone instead.

Without `--register` the same run keeps the profile provisional (outside the repository); `--register` is what
completes it. The survey's memory keys are the survey's guess — measure the heap request with `DM_SVC=1` on a
first run and adjust `app_pool`/`mem_reserve`/`mem_used` here if the game wants more.

## The profile's keys (planned, mirroring the Definitive Edition where the engine is the same)

The Definitive Edition's profile is the template; the differences are the Switch 1 memory partition and the
absence of NVN2. The engine keys carry over unchanged:

- **`modules main sdk`** — the DE loads `main` and `sdk`; its `subsdk0` (the SDK's movie module) is not
  loaded because `main` takes nothing from it the host does not supply. The base game is the same engine:
  the survey confirms the load order.
- **HF2 textures / archives** (the previous request's HF2 scope, same engine): `romfs_marker` names a file
  in the RomFS (the survey picks the biggest file; the DE uses `raw/texture_streaming/w3_stage.ntsp` — the
  base game's texture-streaming package is expected under `raw/` too, confirmed by the survey), and
  `zip_store .acb:.awb:.dds:.ntsp:.pac:.usm:.metallib` keeps the already-compressed archives (CRIWARE
  `.acb`/`.awb`, `.dds` textures, `.ntsp` texture-streaming packages, PACx `.pac` archives, `.usm` movies)
  stored uncompressed in the APK.
- **`shader_groups /raw/=raw`** — the DE's shaders all live in `raw/NeedleShader.pac`; the base game's
  shaders are expected in the same place (the survey's group list confirms; without the key the split
  scans the whole RomFS).
- **`depth_mode zero`** — the DE sets near = 0 at boot and the key keeps the pipeline prebuild from
  rebuilding every pair at first start; the base game is the same engine, so it is expected to do the same
  (confirm on the first run: a wrong value only costs the one-time prebuild, the host follows the game at
  run time).
- **Switch 1 memory partition** (the DE's `0x270000000`/`0x240000000` is the Switch 2's 9 GB pool and does
  not apply here): the standard Switch 1 shape — `mem_reserve 0x100000000` (256 MB module region + 256 MB
  alias + 256 MB stack + a 3.25 GB heap region), `mem_alias 0x10000000`, `mem_stack 0x10000000`,
  `app_pool 0xCD500000` (the Switch 1's 3285 MB application pool, which is what the SDK sizes the heap
  from), `sys_resource 0x0` (no virtual address memory, as the DE's NPDM). The SM-X110's ~8 GB hosts the
  ~4 GB guest block with room to spare; the heap request is measured with `DM_SVC=1` and the pool can be
  raised afterwards if the game wants more.
- **No `patches.py`** — the DE needs none (the rewriter's compare-and-swap site holds for any module); the
  base game starts without one and gets one only if the bring-up finds a build-bound cut.

## Mali (SM-X110, Mali-G57 MC2)

The base game reuses the same Android pipeline as the Definitive Edition: NVN shaders split by
`shader_split.py`, translated by `dmsh_xlate`, and packed with the Mali pack-time pass
(`dm/android/tools/spv_android.py --gpu mali-g57-mc2`, plumbed end-to-end through
`scripts/build_android_from_nsp.py --gpu` and the `build-apk.yml` workflow). The pass adds the
`NonWritable` decoration to read-only storage buffers at pack time so the Mali driver skips their
transform feedback; the runtime hook (`gfx_vk_draw.inc` → `spv_readonly`) covers devices without
`vertexPipelineStoresAndAtomics`. The `--gpu` target is the same `mali-g57-mc2` used for the Definitive
Edition.

## Open

- The NSP (base, and the update when supplied): the build hashes, the `sdk` hash, the shader tables, the
  marker and the shader groups all come from it.
- Gameplay testing on the SM-X110: the base game's zones, Cyber Space stages, combat, the USM movies and
  sound are not tested here.
- Whether the base game's heap request needs the pool raised for the SM-X110's 8 GB (measure with
  `DM_SVC=1`, then re-check the `app_pool`/`mem_reserve` pair).
