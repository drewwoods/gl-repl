## Backdrop Scenes

### PR

See https://github.com/drewwoods/OpenGL-vibe/pull/23 for more scenes

https://github.com/drewwoods/OpenGL-vibe/pull/24 is what landed

### What This Is Trying To Achieve

The immediate-mode REPL spends most of its life with the user looking at a nearly-black viewport while writing geometry.  The workspace is inherently focused - code panel on one side, 3D view on the other - and the 3D view is mostly empty space between edits.

Backdrop scenes turn that empty space into something alive.  The goal is not to add visual noise or distract from the geometry the user is building.  It is the opposite: to give the empty viewport a sense of depth and habitation so that the *absence* of user geometry feels like a stage rather than a void.  When a user pauses to think, or leaves the session running, the world around their work should feel like it is quietly going on without them.

The constraint that drives all the rendering decisions is **subtlety**.  Backdrop elements must not compete with anything the user draws.  They live at the periphery - on the horizon, overhead, or barely distinguishable from the background colour.  Their motion must be slow enough that the eye accepts them as environmental rather than interactive.

### Design Principles

**Colours close to the clear colour.**  The background is `(0.10, 0.10, 0.13)`.  Every backdrop element starts from that as its darkest point.  Stars are only slightly brighter.  City buildings are only marginally above the background.  Ship silhouettes are dark enough that they read as shapes rather than objects.  This keeps the user's geometry - which is usually much more saturated - always perceptually dominant.

**Slow time.**  Human perception is very sensitive to things moving at coding speed (30-60 fps animation cycles).  Backdrop motion is measured in minutes, not seconds.  The star field twinkles with amplitudes measured in hundredths of a colour unit.  The city's time-zone cycle takes ten minutes per revolution.  Ships orbit the scene over thirteen-minute laps.  Battles happen every 45-90 seconds.  The giant squid appears every 75-150 seconds.  None of these are things a user will watch happen in real time; they are things a user will notice have changed after a break.

**Spatial layering.**  Everything in the backdrop occupies different depth ranges so it does not interfere with user geometry near the origin:
- Stars: sky dome at radius 45, depth test disabled (always behind everything).
- Starships: world space at altitude 6-14, fly over in seconds and are gone.
- City: ring at radius 26-36, buildings 0.5-9.7 units tall.
- Pirates: ring at radius 17-24, at sea level.

The user works near the origin.  The backdrop fills the distance.

**Independent toggles.**  Each scene is a separate `g_cfg_items[]` entry so it can be switched off cleanly without affecting any other scene or the user's geometry.  Sessions that need a clean dark void can have one.

### Current Scenes

#### Star Field (`scene_backdrop.c` - `draw_starry_sky`)
220 points on a sky-dome sphere (radius 45).  Only camera rotation is applied - no world translation - so the stars appear fixed at infinity as the user orbits.  Colours are `(0.10-0.26, 0.10-0.26, 0.13-0.32)`, barely above the clear colour with a slight blue bias.  Per-star twinkle phases at very low amplitude (±0.012) ensure no two stars pulse together.  Point sizes are either 1 px (85% of stars) or 1.5 px.

#### Starships - not yet implemented
Design: up to two ships fly overhead at altitude 6-14, in random horizontal headings, at 3.5-7.5 units/sec.  Each ship is a diamond-shaped hull outline with an additive engine glow point and a 24-sample ring-buffer trail that fades from engine colour to transparent.  Three colour palettes: blue-white, warm amber, pale green.  Ships spawn infrequently (12-37 s between appearances) and fade out over the last 20% of their crossing.

#### City Skyline (`scene_backdrop.c` - `draw_cityscape`)
128 buildings arranged in two concentric rings (radius 26 and 31.5) around the origin.  Three height classes - low suburban (0.5-1.7), midrise (1.6-4.0), skyscraper (4.2-9.7) - give a varied silhouette.  Building colour is `(0.105, 0.105, 0.135)`, just above the background.

Window lights are driven by a **600-second cosine wave** that sweeps a "night zone" once around the full ring per cycle.  At any moment roughly half the horizon is in night (windows lit) and half in day (windows dark), like looking down at the Earth from orbit.  Each window has an individual random threshold so lights appear and extinguish at different moments as the wave passes - no mass switching.  Transitions use smoothstep.

Window colours: 65% warm incandescent yellow, 23% cool white, 12% cold office blue.  A faint haze quad at the base of lit buildings adds ground glow.

#### Pirate Sea Battle - not yet implemented (`scene_pirates.c` does not exist)
Design: six ships - three pirate, three navy - patrol slow circular orbits (radius 17-24) at sea level.  Each is a dark silhouette: elongated hex hull, raised sides, mast with yard and backstay, triangular sail.

A state machine drives engagement: every 45-90 s a pirate and a navy ship break from patrol, close on each other, exchange cannon fire for 12-24 s, and one sinks.  Cannon shots are additive `GL_POINTS` on a ballistic arc with a brief muzzle flash at the bow.  The losing ship sinks over 20 s with alpha fade, then respawns off-screen after 18-40 s.

The giant squid rises near a sailing ship every 75-150 s.  It surfaces over ~6 s (filled ellipse mantle + 8 sinusoidal `GL_LINE_STRIP` tentacles that reach toward the target during the attack phase), then submerges over ~5 s.

### What It Is Not

Backdrop scenes are not part of the REPL's command language.  They do not export to `output.c`.  They do not interact with user geometry, lighting, or transforms.  They are environmental dressing only.

They are also not a simulation.  Timing is approximate, randomness is deterministic (seeded LCGs), and state machines are simple enough to be understood in a single read.  The goal is atmosphere, not accuracy.

### Potential Future Scenes

The pattern is easy to extend.  A new scene needs:
1. A `.c` / `.h` file pair with a `draw_*()` entry point.
2. A `g_show_*` global in `repl_core.c` + extern in `sample.h`.
3. A `ReplConfigItem` descriptor row in `repl_actions.c` plus the matching
   `ReplConfigKey` case in `repl_config.h` / `repl_config.c`.
4. A `draw_*()` call in `render_3d_scene()` after `execute_commands()`.
5. An entry in `SRCS` / `CORE_TEST_SRCS` in the Makefile.

Ideas that would fit the design principles:
- **Aurora borealis** - sinusoidal curtains of faint colour sweeping slowly across the sky dome, triggered at low frequency.
- **Weather** - very faint rain lines in the distance, or a fog layer that rolls in and out over several minutes.
- **Wildlife** - birds crossing the scene in loose Vformation, or a whale breaching at the horizon once every few minutes.
- **Space debris** - slow tumbling objects in high orbit, distinguishable from the starships by their irregular rotation and lack of thrust.
