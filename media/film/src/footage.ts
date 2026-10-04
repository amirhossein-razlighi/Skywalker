/**
 * Footage manifest: every engine shot the film uses, as a slot.
 *
 * `<Footage slot="id" />` plays `public/footage/<id>.mp4` when it exists, otherwise `public/footage/<id>.png`
 * (with a slow Ken Burns move), otherwise an elegant placeholder. This file is also the render list:
 *   - `scripts/render_footage.py` renders every slot that has a `clip` through the engine's Movie Render Queue
 *     (`movie_render`): HEVC, 1920x1080, 30 fps, 8 samples, 180-degree shutter, one clip at a time, resumable;
 *   - `scripts/render_stills.sh` renders the slots that only have a `still` (UI-framed captures, older scenes).
 * Hero moves come from the flagship scenes' own `*.sequence.json` shots, so the film and the scenes agree.
 */

export type Mode = "final" | "clay" | "sketch";

export interface StillSpec {
  eye?: [number, number, number];
  target?: [number, number, number];
  fov?: number;
  /** Simulation ticks (60 Hz) to run before the capture, so fire, particles and behaviors are alive. */
  warmup?: number;
  samples?: number;
  /** Use the scene's own game camera instead of eye/target. */
  sceneCamera?: boolean;
  /** Tool calls applied before the capture (e.g. a time-of-day change). */
  setup?: { tool: string; args: Record<string, unknown> }[];
  /** Also save the set-of-mark entity boxes as public/footage/<id>.marks.json. */
  marks?: boolean;
  /** Copy an existing image instead of rendering (repo-relative path). */
  copyFrom?: string;
}

/** A moving clip, rendered with `movie_render`. Either `sequence` or `camera`. */
export interface ClipSpec {
  /** Project folder name (searched in SKY_EXAMPLE_ROOTS, then examples/). Defaults to the slot's scene. */
  project?: string;
  /** Scene file inside the project. */
  scene?: string;
  /** One of the scene's hero moves (camera, mood and events come with it). */
  sequence?: string;
  /** Seconds into the sequence (the simulation pre-rolls to it). */
  start?: number;
  /** Inline camera path (movie_render `camera`), for moves the scenes don't have. */
  camera?: { keys: Record<string, unknown>[] } | { shots: Record<string, unknown>[] };
  /** With `camera`: a sequence whose environment keys (the mood) are applied first. */
  mood?: string;
  /** Run the whole game (scripts, autopilots, physics). Default true. */
  simulate?: boolean;
  samples?: number;
  shutter?: number;
  setup?: { tool: string; args: Record<string, unknown> }[];
}

export interface Slot {
  id: string;
  section: "open" | "idea" | "wander" | "render" | "variety" | "tools" | "finale";
  /** Example project in examples/ (or a flagship checkout listed in SKY_EXAMPLE_ROOTS). */
  scene: string;
  mode: Mode;
  /** Clip length the edit needs (30 fps). */
  frames: number;
  resolution: [number, number];
  /** What the shot is, in words. */
  shot: string;
  /** Scene not in the repo yet: placeholder until it lands. */
  pending?: boolean;
  still?: StillSpec;
  clip?: ClipSpec;
}

const HD: [number, number] = [1920, 1080];

const s = (
  id: string,
  section: Slot["section"],
  scene: string,
  frames: number,
  shot: string,
  still?: StillSpec,
  mode: Mode = "final",
  extra: Partial<Slot> = {},
): Slot => ({ id, section, scene, mode, frames, resolution: HD, shot, still, ...extra });

/** A clip slot: `scene` + one hero sequence (or an inline camera). */
const c = (id: string, section: Slot["section"], scene: string, frames: number, shot: string, clip: ClipSpec, mode: Mode = "final"): Slot =>
  s(id, section, scene, frames, shot, undefined, mode, { clip });

/**
 * A hero move rendered three ways for the sketch -> clay -> final reveal. The three clips start on the same
 * frame of the same move, so the layers stay registered while the camera moves; each is only as long as the
 * reveal shows it (`frames` = [sketch, clay, final]).
 */
const triple = (id: string, section: Slot["section"], scene: string, frames: [number, number, number], shot: string, clip: ClipSpec): Slot[] => [
  c(`${id}_sketch`, section, scene, frames[0], `${shot} (pencil-contour sketch view: debug_view "sketch")`, clip, "sketch"),
  c(`${id}_clay`, section, scene, frames[1], `${shot} (matte clay: clay true)`, clip, "clay"),
  c(id, section, scene, frames[2], `${shot} (final render)`, clip, "final"),
];

const ISLE = "tidebreak_isle";
const NEON = "neon_requiem";
const PEAKS = "ashen_peaks";
const GLOAM = "gloamwater";
const FARM = "berrybrook";

/** A montage clip: 3.2 s from the middle of a hero move. */
const m = (id: string, scene: string, seq: string, start: number, shot: string, frames = 96) =>
  c(id, "render", scene, frames, shot, { sequence: seq, start });

export const FOOTAGE: Slot[] = [
  // 1. Cold open: the brig at anchor off Tidebreak Isle, drawn, filled with clay, then lit by the low sun.
  //    A slow 14 s lateral drift (the brig hero move, slowed down) with the shot's golden-hour mood.
  ...triple("open_hero", "open", ISLE, [180, 300, 430],
    "The brig riding at anchor, low over the swell at golden hour; slow lateral drift past the bow",
    {
      mood: "sequences/brig.sequence.json",
      camera: {
        keys: [
          { t: 0, eye: [-8.5, 1.72, 291.8], target: [34, 9, 330], fov: 35 },
          { t: 14.4, eye: [5.5, 1.62, 289.9], target: [34, 9.6, 330], fov: 33.5 },
        ],
      },
    }),

  // 3. The idea: agents see and act (stills inside UI windows: a viewport_capture is a still).
  s("see_capture", "idea", "harvest_fair", 240,
    "viewport_capture of the fair from above, annotate:false; the film draws the set-of-mark #id boxes from the returned entity list",
    { eye: [21, 14, 24], target: [-3, 3, -6], fov: 50, warmup: 90, marks: true }),
  s("act_before", "idea", "harvest_fair", 120, "Same framing as see_capture at midday (sun 55 deg)",
    { eye: [21, 14, 24], target: [-3, 3, -6], fov: 50, warmup: 90, setup: [{ tool: "environment_update", args: { sunElevation: 55 } }] }),
  s("act_after", "idea", "harvest_fair", 120, "Same framing after the agent's environment_update: golden hour (sun 7 deg)",
    { eye: [21, 14, 24], target: [-3, 3, -6], fov: 50, warmup: 90, setup: [{ tool: "environment_update", args: { sunElevation: 7 } }] }),

  // 5. Wander backdrop (blurred behind the code).
  s("wander_coins", "wander", "cloudhopper", 300, "Coins spinning over the floating islands, slow lateral drift",
    { eye: [8.5, 4.5, 11], target: [0, 6, 0], warmup: 120 }),

  // 6. Rendering showcase: one feature per shot, all moving, all from the flagship scenes.
  m("rs_isle_swash", ISLE, "sequences/swash.sequence.json", 1.4, "Low along the waterline as the surf runs up the sand"),
  m("rs_isle_palms", ISLE, "sequences/palms.sequence.json", 1.4, "Through the palms toward the low sun: god rays"),
  m("rs_isle_campfire", ISLE, "sequences/campfire.sequence.json", 2.0, "Driftwood campfire on the beach at dusk"),
  m("rs_isle_aerial", ISLE, "sequences/establishing.sequence.json", 2.4, "Aerial approach over the cove, the brig and the headland"),
  m("rs_peaks_forest", PEAKS, "sequences/establishing.sequence.json", 2.4, "Aerial over misty forests toward the cliff-top monastery at sunrise"),
  m("rs_peaks_lake", PEAKS, "sequences/meadow.sequence.json", 2.0, "Low over the misty meadow toward the monastery ridge at sunrise"),
  m("rs_peaks_gate", PEAKS, "sequences/crane_gate.sequence.json", 2.6, "Crane up the stairs to the lantern-lit gate at sunset"),
  m("rs_peaks_hall", PEAKS, "sequences/courtyard.sequence.json", 2.0, "Across the courtyard to the glowing main hall"),
  m("rs_neon_avenue", NEON, "cinematics/avenue_dolly.sequence.json", 2.0, "Dolly down the rain-soaked neon avenue"),
  m("rs_neon_puddle", NEON, "cinematics/puddle_mirror.sequence.json", 1.4, "Inches above a puddle full of neon"),
  m("rs_neon_holo", NEON, "cinematics/hologram.sequence.json", 2.0, "Up among the signs to the dancing hologram"),
  m("rs_neon_noodle", NEON, "cinematics/noodle_stall.sequence.json", 2.0, "Past the noodle stall: lanterns and wet tiles"),
  m("rs_neon_skyline", NEON, "cinematics/skyline.sequence.json", 2.4, "High over the skyline: thousands of lit windows"),
  m("rs_farm_rows", FARM, "cinematics/berry_rows.sequence.json", 2.0, "Ground level down the berry rows, shallow depth of field"),
  m("rs_farm_pond", FARM, "cinematics/lily_pond.sequence.json", 2.0, "Over the lily pond toward the low sun"),
  m("rs_farm_mini", FARM, "cinematics/miniature.sequence.json", 2.4, "Tilt-shift aerial: the farm as a miniature"),

  // 7. Variety: the mosaic, then the cozy farm with its HUD, then Gloamwater in 2D with its dialogue.
  c("var_farm", "variety", FARM, 180, "Berrybrook gameplay framing with the HUD (season, coins, toolbar)",
    { sequence: "cinematics/farm_day.sequence.json", start: 0 }),
  c("var_gloam_run", "variety", GLOAM, 108, "Gloamwater: Wick runs through the Lumen Grove (autopilot)",
    { sequence: "cinematics/grove.sequence.json", start: 7.0 }),
  c("var_gloam_talk", "variety", GLOAM, 150, "Gloamwater: Wick meets Bellwether; the dialogue box types in",
    { sequence: "cinematics/bellwether.sequence.json", start: 14.0 }),
  // Meridian Accord: from the whole continent down to the river crossing at Pont-Aurel (labels, tooltip, HUD).
  c("var_strategy", "variety", "meridian_accord", 240, "Meridian Accord: zoom from the continent to the front at Pont-Aurel, with the campaign HUD",
    { sequence: "cinematics/zoom_to_pont_aurel.sequence.json", start: 0 }),
  c("var_front", "variety", "meridian_accord", 96, "Meridian Accord: low over the front line, offensive arrows and unit counters, shallow focus",
    { sequence: "cinematics/front_line.sequence.json", start: 1.6 }),
  // The Chancellor's Desk: the decree arrives, the cursor signs it (the engine's own UI).
  c("var_drama", "variety", "chancellors_desk", 96, "The Chancellor's Desk: the office at night, rain on the glass",
    { sequence: "cinematics/the_office.sequence.json", start: 2.0 }),
  c("var_decree", "variety", "chancellors_desk", 165, "The Chancellor's Desk: Decree No. 14 slides in and is signed",
    { sequence: "cinematics/the_decree.sequence.json", start: 0 }),
  // Older samples, stills in the mosaic tiles.
  s("var_kart", "variety", "toy_kart_rally", 96, "Stadium orbit at dusk, karts racing, fireworks", { eye: [-29.6, 16, 26], target: [0, 2, -4], warmup: 120 }),
  s("var_shmup", "variety", "star_lancer", 96, "Wide over the ringed giant, ships in formation", { eye: [-3, 8, 40], target: [10, -6, -40], warmup: 240 }),
  s("var_park", "variety", "harvest_fair", 96, "Carousel at golden hour", { eye: [-6.5, 2.6, 4.5], target: [-12, 2.4, -2], warmup: 90 }),
  s("var_zen", "variety", "zen_garden", 96, "Blossoms falling over the pond", { eye: [12.5, 1.4, 5], target: [8, 4.2, 0], warmup: 120 }),
  s("var_horror", "variety", "hollow_manor", 96, "Graveyard in the fog, lantern sway", { eye: [-16, 1.9, 8], target: [-10, 1.4, -1], warmup: 60 }),
  s("var_cozy", "variety", "hearthside", 96, "The tree and the window, snow outside", { eye: [1.65, 1.1, 0], target: [3.6, 1.3, -2.7], warmup: 90 }),
  s("var_arena", "variety", "neon_drift", 96, "Low orbit of the neon arena, drones swarming", { eye: [-10.3, 2.2, 14.7], target: [0, 1.5, -4], warmup: 240 }),
  s("var_frost", "variety", "frostlight", 96, "Penguins heading home across the ice", { eye: [8, 0.8, 9], target: [2, 0.8, 0], warmup: 200 }),
  s("var_village", "variety", "sky_village", 96, "Sky Village (built live by agents)", { eye: [32, 12.5, 11], target: [0, 1.2, 0], warmup: 120 }),
  s("rs_abyss", "variety", "abyss", 72, "The angler's lure in the deep", { eye: [12, 4, 5], target: [5, 4.2, -6], warmup: 200 }),
  s("rs_canyon", "variety", "namaqua_canyon", 72, "The canyon at sunset, long lens", { eye: [2.5, 5.5, 62], target: [0, 6, -70], fov: 36, warmup: 60 }),

  // 8. Works with your tools: the Blender-built monastery (clay -> final on the orbit), and Gloamwater shipped as an app.
  c("tools_blender_clay", "tools", PEAKS, 186, "Orbit of the five-tier pagoda (Blender-built kit) at sunset, matte clay",
    { sequence: "sequences/ember_pagoda.sequence.json", start: 0.4 }, "clay"),
  c("tools_blender", "tools", PEAKS, 186, "Orbit of the five-tier pagoda (Blender-built kit) at sunset",
    { sequence: "sequences/ember_pagoda.sequence.json", start: 0.4 }),
  c("tools_app", "tools", GLOAM, 138, "Gloamwater's title shot: the shipped game starting up",
    { sequence: "cinematics/title.sequence.json", start: 0 }),

  // 9. Finale: three worlds, sketch -> clay -> final on moving cameras, then the hero.
  ...triple("fin_a", "finale", NEON, [112, 142, 150], "Neon Requiem: crane up from the street into the signs",
    { sequence: "cinematics/crane_reveal.sequence.json", start: 1.2 }),
  ...triple("fin_b", "finale", PEAKS, [112, 142, 150], "Ashen Peaks: up the pilgrim stairs toward the gate at sunset",
    { sequence: "sequences/pilgrim_stairs.sequence.json", start: 1.0 }),
  ...triple("fin_c", "finale", FARM, [112, 142, 150], "Berrybrook: rising past the windmill's turning sails",
    { sequence: "cinematics/windmill.sequence.json", start: 1.0 }),
  c("fin_hero", "finale", ISLE, 180, "Tidebreak Isle: aerial over the turquoise shallows, the jetty and the cove at golden hour",
    { sequence: "sequences/shallows.sequence.json", start: 0.4 }),
];

export const SLOT = Object.fromEntries(FOOTAGE.map((x) => [x.id, x])) as Record<string, Slot>;
