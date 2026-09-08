#pragma once

// Central Wii-only tuning knobs.
//
// Included from platform/PlatformTuning.h AFTER the desktop baseline table, and
// only when PLATFORM_WII is set. It is an override file, not a third copy of the
// table: the Wii agrees with the desktop baseline on most of those ~60 knobs, so
// only the ones it genuinely disagrees with appear here, each with the
// measurement or the arithmetic that chose it.
//
// What this file is NOT
// --------------------
// It is not the PS2 profile with different numbers. The Wii deliberately keeps
// PLATFORM_CONSOLE_LOW at 0 (see platform/PlatformConfig.h), so vanilla world
// generation, real lighting, options.txt and a user-selectable render distance
// all stay. Everything overridden below is under PLATFORM_BOUNDED_WORLD, i.e.
// "the chunks have to fit", plus two backend facts about the console.
//
// The budget these numbers are sized against
// ------------------------------------------
//   MEM1  24 MB, of which the ~14.5 MB DOL, two 640x480 framebuffers (~1.2 MB)
//         and the 256 KB GX FIFO leave roughly 8 MB.
//   MEM2  64 MB, ~52 MB of which is Arena2.
// With MALLOC_MEM2=1 (src/wii/WiiEarlyInit.cpp) malloc uses Arena2 only, roughly
// 50 MB at early boot. The remaining Arena1 is not malloc fallback. Against that:
//   - a loaded chunk column is ~80 KB (32 KB blocks + 3 nibble arrays + heightmap)
//   - a compiled chunk section is a GX display list at ~29 bytes per vertex,
//     which the port's own logs put at 20-200 KB per section
// The second of those is the larger consumer and is bounded by the render
// distance; these knobs bound the first, and the streaming PLATFORM_PRELOAD_RADIUS_BLOCKS          work that feeds it.

// -----------------------------------------------------------------------------
// Backend facts (not budgets)
// -----------------------------------------------------------------------------

#if PLATFORM_WII
#define PLATFORM_BOUNDED_WORLD 1
#endif

// There is no OS cursor to fall back on, and data/assets/cursor.png ships with
// the game, so use the drawn pointer rather than the vector arrow.
#undef  PLATFORM_CURSOR_TEXTURE
#define PLATFORM_CURSOR_TEXTURE                  1

// Vanilla uses 6 to size the occlusion-query box around a section's frustum
// test. opengx issues no occlusion queries, so here the margin only makes the
// test accept sections it should reject, and each accepted section costs a full
// GX display-list replay. 1 keeps the section's own 1-block mesh margin covered.
// Raise it if geometry pops at the edge of view.
#undef  PLATFORM_RENDERER_AABB_MARGIN
#define PLATFORM_RENDERER_AABB_MARGIN            1.0f

// -----------------------------------------------------------------------------
// Memory budget (PLATFORM_BOUNDED_WORLD)
// -----------------------------------------------------------------------------

// FAR is a desktop assumption, not a budget anyone checked against this heap.
// RenderGlobal::loadRenderers derives its grid from this value:
//
//   FAR    (0)  26 x 8 x 26 = 5408 sections,  676 chunk columns
//   NORMAL (1)  17 x 8 x 17 = 2312 sections,  289 chunk columns
//   SHORT  (2)   9 x 8 x  9 =  648 sections,   81 chunk columns
//
// FAR asks for ~54 MB of chunk columns before a single triangle exists, and the
// display lists on top of that cannot fit. The failure mode is the nasty one --
// not a crash but silently missing terrain, because a section whose list
// allocation fails is skipped and never retried, its WorldRenderer having
// already been marked clean.
//
// SHORT was the first estimate and hardware said no. Measured 2026-08-05 on the
// [WII][RAM] log, ~14 s after entering a world at SHORT:
//
//   post-spawnChunks  free=14076KB in 1418 blocks  sbrk left: MEM2=28303KB
//   in-world  (+4 s)  free= 2104KB in  378 blocks  sbrk left: MEM2=17743KB
//   in-world (+10 s)  free= 4288KB in  662 blocks  sbrk left: MEM2= 3147KB
//
// Read those as deltas rather than levels. Between the samples sbrk took 10.5 MB
// and then 14.5 MB more from Arena2 while the free total barely moved, i.e. ~35
// MB of NET LIVE allocation in 14 seconds, ending with ~53 MB live, 4 MB free and
// 3 MB of arena left. That is the world streaming in, and it is consumption, not
// fragmentation: fragmentation shows a large free total the allocator cannot
// place, and here the free total is simply gone.
//
// 648 sections against ~35 MB of growth puts a section at roughly 54 KB, square
// in the 20-200 KB range this port's own logs reported. TINY is 5 x 8 x 5 = 200
// sections, 31% of that, so the same content costs ~11 MB.
//
// This is a floor chosen to boot reliably, NOT a verdict on what the console can
// do. The render distance stays fully user-selectable (PLATFORM_CONSOLE_LOW is 0,
// so GameSettings keeps the desktop cycling branch), and the new gxlists= figure
// in the [WII][RAM] line reports live display-list bytes and the per-section
// average -- so raising this to SHORT in Options and reading one log line is now
// a measurement rather than a gamble.
#undef  PLATFORM_DEFAULT_RENDER_DISTANCE
#define PLATFORM_DEFAULT_RENDER_DISTANCE         3

// Preload radius, in blocks, for Minecraft::preloadWorld.
//
// This was the single largest allocation in the port and it was never a
// decision: preloadWorld read 128 from a hardcoded #else branch that only the
// PS2 escaped, and its loop steps 16 blocks at a time over [-c, c] on both axes
// -- 17 x 17 = 289 chunk columns, each generated SYNCHRONOUSLY on the loading
// screen. That is ~23 MB before the first frame, plus whatever the light
// flood-fill pulls in from neighbours.
//
// Worse, it does not come back: the unloadAllChunks() immediately after cannot
// free any of it, because eviction requires a chunk to be both outside the
// unload radius and untouched for PLATFORM_MIN_UNUSED_TICKS_BEFORE_UNLOAD ticks,
// and these were all touched a moment ago. The first pass frees nothing, returns
// false, and the drain loop exits.
//
// 32 gives 5 x 5 = 25 columns (~2 MB), which is exactly the TINY render grid:
// preload what the player can actually see and let the streaming path handle the
// rest. Keep this matched to PLATFORM_DEFAULT_RENDER_DISTANCE above -- the loop
// is ((2c/16)+1)^2, and the grid is (64 << (3 - renderDistance))/16 + 1 wide.
#undef  PLATFORM_PRELOAD_RADIUS_BLOCKS
#define PLATFORM_PRELOAD_RADIUS_BLOCKS 32

#undef PLATFORM_LIGHTING_UPDATES_PER_FRAME
#define PLATFORM_LIGHTING_UPDATES_PER_FRAME 256

// Entity simulation normally requires every chunk in a 32-block radius (5x5
// columns in the worst alignment). The Wii resident cache is deliberately
// smaller, so entities at its streaming edge could stop ticking altogether.
// One chunk keeps a useful safety margin without requiring the vanilla 5x5 set.
#undef  PLATFORM_PLAYER_UPDATE_CHUNK_RANGE_BLOCKS
#define PLATFORM_PLAYER_UPDATE_CHUNK_RANGE_BLOCKS 16


// Resident chunk cache, in chunks around the player.
//
// 3 is the minimum that works rather than a guess: the TINY grid is 5 columns
// wide, so it spans +-2 chunks from the centre, and WorldRenderer meshes a
// section through a ChunkCache built with a 1-block margin, which reaches one
// chunk further. Below 3 the outermost sections would mesh against blankChunk
// and render holes.
//
// RAISE THIS IF YOU RAISE THE RENDER DISTANCE. The rule is
// (grid width / 2) + 1, so SHORT (9 wide) needs 5 and NORMAL (17 wide) needs 9.
// Nothing enforces it at compile time because the render distance is a runtime
// Options setting, and getting it wrong shows up as terrain holes at the edge of
// view rather than as a crash.
//
// The unload radius is one chunk beyond, so walking a chunk boundary does not
// immediately delete and regenerate the ring behind you. Ceiling is 9 x 9 = 81
// columns ~ 6.5 MB, and that is a ceiling: chunks are created on demand, so the
// steady state while standing still is nearer the 49 of the cache radius.
#undef  PLATFORM_CHUNK_CACHE_RADIUS
#define PLATFORM_CHUNK_CACHE_RADIUS              3
#undef  PLATFORM_CHUNK_UNLOAD_RADIUS
#define PLATFORM_CHUNK_UNLOAD_RADIUS             4

#undef  PLATFORM_RANDOM_TICK_CHUNK_RADIUS
#define PLATFORM_RANDOM_TICK_CHUNK_RADIUS 4

// Spread the fixed per-chunk weather/cave work across ticks and reduce the
// random block-update rate from the desktop's 80 per resident chunk. With the
// observed 81-chunk working set, the desktop values issue 6480 random probes
// every world tick; 20 keeps crops/fluids responsive while cutting that to a
// quarter, and the round-robin preserves that chosen total rate.
#undef  PLATFORM_RANDOM_BLOCK_TICKS_PER_CHUNK
#define PLATFORM_RANDOM_BLOCK_TICKS_PER_CHUNK    20
#undef  PLATFORM_RANDOM_TICK_CHUNKS_PER_TICK
#define PLATFORM_RANDOM_TICK_CHUNKS_PER_TICK     9

// Entity CPU guardrails. These retain normal mob simulation but prevent one
// tick from running an unlimited number of A* searches, and avoid rescanning
// the whole spawn area every tick.
#undef  PLATFORM_MAX_LIVE_MOBS
#define PLATFORM_MAX_LIVE_MOBS                   48
#undef  PLATFORM_PATHFIND_BUDGET_PER_TICK
#define PLATFORM_PATHFIND_BUDGET_PER_TICK        8
#undef  PLATFORM_PATHFIND_MAX_NODES
#define PLATFORM_PATHFIND_MAX_NODES              512
#undef  PLATFORM_MOB_SPAWN_INTERVAL_TICKS
#define PLATFORM_MOB_SPAWN_INTERVAL_TICKS        4
// Sized for the unload-radius ceiling above (81), with headroom for one step up
// in render distance, so the chunk maps never rehash during play.
#undef  PLATFORM_CHUNK_MAP_RESERVE
#define PLATFORM_CHUNK_MAP_RESERVE               81

// Compressed multiplayer columns sent outside the resident radius. Beta's
// protocol cannot request a chunk again, so dropping them creates permanent
// holes while walking on a server whose view-distance exceeds the Wii cache.
// Keep the original zlib payload instead: typical terrain uses only a few MB
// for the whole radius-10 ring. The hard ceiling protects pathological/high
// entropy worlds; eviction always chooses the farthest cached column first.
#define WII_MP_COMPRESSED_CHUNK_CACHE_BYTES      (12u * 1024u * 1024u)
#define WII_MP_CHUNK_PROMOTIONS_PER_TICK         2

// Eviction rate. Higher than the PS2's 2 because unloadChunk writes through
// libfat rather than a Memory Card, so draining a ring costs far less here, and
// because the cache has to keep up with a player who can cross chunk borders
// faster than the PS2 can generate them. Shorter grace period than the desktop's
// 200 ticks for the same reason: 3 seconds of standing still is long enough to be
// sure the player is not about to walk straight back.
#undef  PLATFORM_MAX_CHUNK_UNLOADS_PER_TICK
#define PLATFORM_MAX_CHUNK_UNLOADS_PER_TICK      2
#undef  PLATFORM_MIN_UNUSED_TICKS_BEFORE_UNLOAD
#define PLATFORM_MIN_UNUSED_TICKS_BEFORE_UNLOAD  60

// Synchronous terrain generation throttle.
//
// Without this, crossing into ungenerated territory makes one world tick
// generate the whole leading edge of the cache at once -- the multi-second
// stall, and a burst of allocator traffic large enough to matter on a heap this
// size.
//
// SYNC_RADIUS 0 guarantees only the column occupied by the player. The renderer
// generation gate requests adjacent columns ahead of time, under the normal
// per-tick budget. Radius 1 exempted all nine nearby columns from that budget,
// so several renderer candidates could each generate one in the same frame and
// recreate the very hitch this throttle is meant to prevent.
//
// CHUNKS_PER_TICK 1 streams the rest. Both are starting points chosen from the
// arithmetic, not from a measurement on hardware -- watch the world-tick time
// once there are numbers and move them together.
#undef  PLATFORM_GENERATE_SYNC_RADIUS
#define PLATFORM_GENERATE_SYNC_RADIUS            0
#undef  PLATFORM_GENERATE_CHUNKS_PER_TICK
#define PLATFORM_GENERATE_CHUNKS_PER_TICK        1

// Deferred decoration, 1 chunk per world tick.
//
// The count is the smaller half of this knob. The important half is that any
// non-zero value moves populate() out of prepareChunk() and onto a queue gated
// by canPopulateChunk(), which breaks the populate -> setBlockWithNotify ->
// provideChunk -> populate re-entrancy. On the inline path that chain can
// force-generate neighbours from inside a generation call, which is unbounded
// growth on a bounded cache.
//
// Decoration is also tunable by feature below; the queue controls when the
// monolithic call runs, while those counts control its worst-case duration.
#undef  PLATFORM_POPULATE_CHUNKS_PER_TICK
#define PLATFORM_POPULATE_CHUNKS_PER_TICK        1

// Decoration is still one indivisible call: its per-tick count prevents two
// chunks from decorating together, but cannot stop one vanilla populate() from
// doing 78 liquid-source attempts plus dungeons and the snow scan. Those block
// probes can dominate the same tick that just generated terrain. Keep ores,
// vegetation and biome identity intact while using a milder version of the PS2
// profile for the three especially expensive optional passes. This only affects
// chunks generated after the change; existing saves retain their decoration.
#undef  PLATFORM_POPULATE_WATER_SPRINGS
#define PLATFORM_POPULATE_WATER_SPRINGS          16
#undef  PLATFORM_POPULATE_LAVA_SPRINGS
#define PLATFORM_POPULATE_LAVA_SPRINGS           8
#undef  PLATFORM_POPULATE_DUNGEONS
#define PLATFORM_POPULATE_DUNGEONS                4
#undef  PLATFORM_POPULATE_SNOW_PASS
#define PLATFORM_POPULATE_SNOW_PASS               0

// -----------------------------------------------------------------------------
// Frame pacing (PLATFORM_MESH_BUDGET)
// -----------------------------------------------------------------------------

// Bound chunk meshing to a slice of each frame.
//
// Without this the Wii took the desktop path, which is unbounded in TWO places
// at once, and they compound:
//
//   * RenderGlobal::updateRenderers builds EVERY queued renderer within 16
//     blocks of the player in one go. The candidate cap next to it was gated on
//     PLATFORM_CONSOLE_LOW, which is 0 here.
//   * EntityRenderer then calls updateRenderers again, and again, in a do/while
//     that only stops when the queue drains or a wall-clock limit passes.
//
// So walking into new terrain, or any edit that dirties a handful of sections,
// spent as much of the frame as it needed. That is the hitch, and it is why the
// budget and the retry loop are one knob: bounding the inner call while the
// outer loop still retries measures well and changes nothing.
//
// The PS2's partial-build state machine is NOT part of this. There,
// updateRenderer() is one step of a section and can return having meshed
// nothing, which is why MeshBudget::run still has to ask ps2LastStepDidWork()
// before charging -- inside a #if PLATFORM_PS2, and that is the only line of the
// budget that is platform-specific. Here the call meshes a whole section or is
// never made, so the budget is just "how many, and for how long". That is why
// this could be turned on without porting the capture-slot machinery this file
// used to point at as the blocker.
#undef  PLATFORM_MESH_BUDGET
#define PLATFORM_MESH_BUDGET                     1

// 6 ms of a 33 ms frame (30 fps) is the real governor; the count is the backstop
// for when the clock misbehaves. Both are starting points taken from the PS2's
// shape rather than from a measurement here -- the Broadway meshes a section far
// faster than the EE, so if the FPS counter says there is headroom, raise
// CHUNK_BUILD_BUDGET_MS first and watch how fast terrain fills in behind you.
#undef  PLATFORM_MAX_RENDERER_UPDATES_PER_FRAME
#define PLATFORM_MAX_RENDERER_UPDATES_PER_FRAME  6
#undef  PLATFORM_CHUNK_BUILD_BUDGET_MS
#define PLATFORM_CHUNK_BUILD_BUDGET_MS           6

// Mesh warm-up after entering a local world.  RenderGlobal creates its whole
// renderer grid at the end of Minecraft::changeWorld(); without a short loading
// phase the first gameplay frames have to compile that grid while also running
// ticks and drawing the world.  Keep this finite: any remaining sections use
// the normal per-frame 6 ms budget, so an unusually dense save cannot make the
// loading screen unbounded.
#define PLATFORM_WII_LOAD_WARMUP_MS               2500

// GX display-list cache maintenance. At 10MB of an 11MB live-list budget,
// evict at most two non-visible sections farther than 32 blocks each frame.
// This leaves room for the newly exposed terrain without a visible mass clear.
#define PLATFORM_WII_GXLIST_EVICT_HIGH_WATER_BYTES (10u * 1024u * 1024u)
#define PLATFORM_WII_GXLIST_EVICT_KEEP_RADIUS_BLOCKS 32.0f
#define PLATFORM_WII_GXLIST_EVICT_MAX_PER_FRAME  2

// Candidate cap. Higher than the executed budget on purpose: candidates that
// bail out on the generation gate cost almost nothing, and starving the list
// would leave sections queued behind renderers that are not ready.
#undef  PLATFORM_RENDERER_UPDATE_CANDIDATES_PER_FRAME
#define PLATFORM_RENDERER_UPDATE_CANDIDATES_PER_FRAME 12

// Autosave period, in world ticks. Vanilla is 40 (2 s).
//
// Every firing rewrites level.dat through libfat and deflates up to two dirty
// chunks, synchronously, inside a world tick. On a desktop that is invisible;
// through an SD card it is a multi-frame stall arriving every two seconds, and
// it was reported as the game "tirando" constantly.
//
// 600 ticks is 30 seconds. That is a real trade and it is the reason this is a
// named knob rather than a new hardcoded number: a crash or a pulled power cord
// now costs up to 30 seconds of play instead of 2. The PS2 answer to the same
// problem was PS2_DISABLE_RUNTIME_AUTOSAVE -- no autosave at all, save from the
// menu -- which is worse here, because libfat can absorb a periodic save and a
// Memory Card genuinely cannot. Lower this if the console is being switched off
// abruptly; raise it if 30 seconds still hitches noticeably.
#undef  PLATFORM_AUTOSAVE_PERIOD_TICKS
#define PLATFORM_AUTOSAVE_PERIOD_TICKS           600

// -----------------------------------------------------------------------------
// Entity rendering cost (PLATFORM_SKIP_ENTITY_SHADOWS)
// -----------------------------------------------------------------------------

// Off here, unlike the PS2. Render::renderShadow() (Render.cpp) has a real
// per-entity, per-frame cost -- a 3D block scan (up to (2*shadowSize+1)^2
// columns under the entity) plus a light lookup per hit column, entirely CPU
// and independent of GPU hardware -- but the PS2's OTHER reason to skip it
// does not apply here: that console's GS framebuffer is PSMCT16 with a single
// alpha bit, which collapses shadow.png's soft radial gradient into hard
// black squares, so PS2_SKIP_ENTITY_SHADOWS was fixing a visual bug as much
// as a CPU one (see Ps2Tuning.h's own PS2_SKIP_ENTITY_SHADOWS comment).
// opengx/GX has real alpha, so the shadow already renders correctly here --
// this knob is purely a CPU lever, which is why it defaults to off (keep the
// vanilla look) instead of on.
//
// Wii enables this now: the PERF log shows worlds with roughly 260 entities,
// so paying the block scan for every visible entity is no longer a small visual
// luxury. This does not disable entities or their simulation; it only omits the
// soft blob projected onto the blocks below them.
#undef  PLATFORM_SKIP_ENTITY_SHADOWS
#define PLATFORM_SKIP_ENTITY_SHADOWS             1

// -----------------------------------------------------------------------------
// Deliberately NOT overridden
// -----------------------------------------------------------------------------
// Recorded here because "why is this not in the PS2 list?" is the question this
// file will be read to answer:
//
//   PLATFORM_USE_HEIGHTMAP_TERRAIN, PLATFORM_FLOAT_BIOME_NOISE,
//   PLATFORM_FLOAT_ORE_VEINS, PLATFORM_SKIP_CAVE_GENERATION
//       Broadway has a hardware FPU. These exist because the EE emulates
//       `double` in software, and every one of them CHANGES the generated world.
//       Vanilla generation stays.
//
//   PLATFORM_FORCE_FULLBRIGHT_TERRAIN
//       Paired on the PS2 with skipping the light flood-fill entirely. Keeping
//       it off is what keeps real lighting.
//
//   PLATFORM_VISIBLE_CHUNK_RADIUS, PLATFORM_VERTICAL_CHUNK_COUNT
//       Only read under PLATFORM_CONSOLE_LOW, where they replace the render grid
//       with a fixed small one. The Wii derives its grid from renderDistance so
//       the Options entry keeps working.
//
//   PLATFORM_DISABLE_RUNTIME_AUTOSAVE, PLATFORM_SKIP_NEW_WORLD_FULL_SAVE
//       Memory Card workarounds. libfat is fast enough to save normally -- but
//       not fast enough to save every two seconds, which is what the vanilla
//       period asked for; see PLATFORM_AUTOSAVE_PERIOD_TICKS above. Autosave
//       stays ON here, it just fires far less often.
//
//   PLATFORM_FORCE_GUI_SCALE
//       The framebuffer is 640x480, and ScaledResolution's auto-scale keeps
//       scale 2 at 480/2 = 240. The PS2 needs the override because 448/2 = 224
//       falls under the threshold; the Wii does not.
//
//   PLATFORM_MAX_RENDERED_SECTIONS_PER_PASS, PLATFORM_CHUNK_BUILD_BLOCKS_PER_STEP
//       The two of the four frame budgets that really are PS2-only.
//       _PER_PASS caps sections drawn in the PS2's immediate render path; the
//       Wii goes through RenderList and glCallLists, which never reaches that
//       loop. _BLOCKS_PER_STEP sizes one step of the partial-build state
//       machine, and there are no partial builds here.
//
//       The other two -- MAX_RENDERER_UPDATES_PER_FRAME and
//       CHUNK_BUILD_BUDGET_MS -- ARE set now, see PLATFORM_MESH_BUDGET above.
//       They turned out not to need the capture-slot machinery this note used to
//       assume: only the "did this step do any work" question was entangled with
//       it, and that question does not arise when a build is atomic.

// -----------------------------------------------------------------------------
// Frustum / distance culling (PLATFORM_WII)
// -----------------------------------------------------------------------------

// Límite de secciones dibujadas por paso. sortedWorldRenderers ya está
// ordenado por distancia al jugador (de cerca a lejos), así que esto
// simplemente trunca las lejanas. Con TINY (200 secciones) no hace nada,
// pero con SHORT (648) o NORMAL (2312) evita saturar la FIFO de GX.
#undef  PLATFORM_MAX_RENDERED_SECTIONS_PER_PASS
#define PLATFORM_MAX_RENDERED_SECTIONS_PER_PASS  48

// Radio de culling por distancia, en bloques. Secciones más allá de este
// radio desde la cámara se marcan como fuera de frustum aunque pasen el
// test de los 6 planos. El frustum de 90° FOV en un grid de 17x17 chunks
// incluye secciones diagonales a ~70 bloques que apenas se ven.
#undef  PLATFORM_WII_DISTANCE_CULL_RADIUS
#define PLATFORM_WII_DISTANCE_CULL_RADIUS        48.0
