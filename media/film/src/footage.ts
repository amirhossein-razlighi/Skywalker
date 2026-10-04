/**
 * Footage manifest: every engine shot the film uses, as a slot.
 *
 * `<Footage slot="id" />` plays `public/footage/<id>.mp4` when it exists, otherwise `public/footage/<id>.png`
 * with a slow Ken Burns move, otherwise an elegant placeholder. This file is also the render list:
 *   - `scripts/render_stills.sh` renders every slot that has `still` (stills, today);
 *   - the movie-render tool should render every slot as a clip of `frames` frames at `resolution`,
 *     following `shot` (camera move) and `mode` (final / clay / sketch).
 * Camera coordinates come from the showcase shot lists in media/demo/games/*.py.
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

export interface Slot {
  id: string;
  section: "open" | "idea" | "wander" | "render" | "variety" | "tools" | "finale";
  /** Example project in examples/, or a flagship scene that is still being built. */
  scene: string;
  mode: Mode;
  /** Clip length the edit needs (30 fps). */
  frames: number;
  resolution: [number, number];
  /** The shot we want from the movie renderer. */
  shot: string;
  /** Scene not in the repo yet: placeholder until it lands. */
  pending?: boolean;
  still?: StillSpec;
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

/** A hero shot rendered three ways for the sketch -> clay -> final reveal. */
const triple = (id: string, section: Slot["section"], scene: string, frames: number, shot: string, still: StillSpec): Slot[] => [
  s(`${id}_sketch`, section, scene, frames, `${shot} (pencil-contour sketch view: debug_view "sketch")`, still, "sketch"),
  s(`${id}_clay`, section, scene, frames, `${shot} (matte clay: clay true)`, still, "clay"),
  s(id, section, scene, frames, `${shot} (final render)`, still, "final"),
];

export const FOOTAGE: Slot[] = [
  // 1. Cold open: one world drawn, filled with clay, then lit.
  ...triple("open_hero", "open", "smugglers_cove", 300,
    "Slow push from the sea toward the cove at golden hour: the galleon at anchor, the jetty, cliffs and the FFT ocean. Locked-off feel, 6 m of dolly over 10 s",
    { eye: [46, 11.5, 76], target: [4, 4, -30], fov: 40, warmup: 90 }),

  // 3. The idea: agents see and act.
  s("see_capture", "idea", "harvest_fair", 240,
    "viewport_capture of the fair from above, annotate:false; the film draws the set-of-mark #id boxes from the returned entity list. Very slow orbit",
    { eye: [21, 14, 24], target: [-3, 3, -6], fov: 50, warmup: 90, marks: true }),
  s("act_before", "idea", "harvest_fair", 120, "Same framing as see_capture at midday (sun 55 deg)",
    { eye: [21, 14, 24], target: [-3, 3, -6], fov: 50, warmup: 90, setup: [{ tool: "environment_update", args: { sunElevation: 55 } }] }),
  s("act_after", "idea", "harvest_fair", 120, "Same framing after the agent's environment_update: golden hour (sun 7 deg). Ideally a 4 s time-of-day sweep",
    { eye: [21, 14, 24], target: [-3, 3, -6], fov: 50, warmup: 90, setup: [{ tool: "environment_update", args: { sunElevation: 7 } }] }),

  // 5. Wander backdrop.
  s("wander_coins", "wander", "cloudhopper", 300, "Coins spinning over the floating islands, slow lateral drift; sits blurred behind the code",
    { eye: [8.5, 4.5, 11], target: [0, 6, 0], warmup: 120 }),

  // 6. Rendering showcase (fast cuts, one feature callout each).
  s("rs_ocean", "render", "smugglers_cove", 72, "Drift in from the sea toward the cove: FFT ocean, whitecaps, sun glitter",
    { eye: [52, 13, 87], target: [0, 4, -35], fov: 42, warmup: 90 }),
  s("rs_galleon", "render", "smugglers_cove", 72, "Low along the waterline past the galleon hull, 1.6 m above the swell",
    { eye: [5, 1.6, 58], target: [22, 7, 40], fov: 50, warmup: 90 }),
  s("rs_clouds", "render", "ashen_peaks", 72, "Wide valley under drifting volumetric clouds, sun low and behind the ridge (god rays)",
    { eye: [600, 250, 600], target: [0, 100, 0], fov: 50, warmup: 30 }),
  s("rs_terrain", "render", "ashen_peaks", 72, "Snow line on an erosion-carved ridge, forest below, clouds drifting past",
    { eye: [0, 200, 0], target: [800, 250, 400], fov: 55, warmup: 30 }),
  s("rs_valley", "render", "flagship_mythic_valley", 96, "Flagship: mythic mountain valley. Crane up over a forest edge to reveal the valley, cloud shadows moving", undefined, "final", { pending: true }),
  s("rs_beach", "render", "flagship_island_beach", 96, "Flagship: island beach. Low dolly along the wet sand as a wave breaks; foam lines, palms in wind", undefined, "final", { pending: true }),
  s("rs_neon_city", "render", "flagship_neon_rain_city", 96, "Flagship: neon rain city. Push down a street in the rain; hundreds of clustered lights, reflections in puddles", undefined, "final", { pending: true }),
  s("rs_rain", "render", "hidden_alley", 72, "Walk into the alley in the rain: GPU rain, mist, neon, wet street",
    { eye: [1.1, 1.65, 9], target: [0.2, 2.6, -24], fov: 52, warmup: 120 }),
  s("rs_puddle", "render", "hidden_alley", 72, "Low over a puddle full of neon: screen-space reflections",
    { eye: [0.4, 0.32, 4.0], target: [-0.6, 2.2, -22], fov: 50, warmup: 120 }),
  s("rs_fire", "render", "hidden_alley", 72, "Orbit the burning barrel: GPU fluid fire and steam",
    { eye: [-2.6, 1.5, -12.5], target: [-1.6, 1.4, -15], fov: 50, warmup: 180 }),
  s("rs_gi", "render", "hearthside", 72, "Fireside: firelight bouncing across the room (screen-space GI)",
    { eye: [1.75, 1.42, 2.7], target: [-4.4, 0.9, -0.2], warmup: 120 }),
  s("rs_canyon", "render", "namaqua_canyon", 72, "The canyon at sunset, silhouetted, long lens",
    { eye: [2.5, 5.5, 62], target: [0, 6, -70], fov: 36, warmup: 60 }),
  s("rs_campfire", "render", "namaqua_canyon", 72, "Orbit the camp fire in the canyon",
    { eye: [6.5, 1.6, 12], target: [3.5, 0.7, 6], fov: 48, warmup: 180 }),
  s("rs_aurora", "render", "frostlight", 72, "Pan across the aurora above the ice",
    { eye: [-2, 1.6, 14], target: [0, 22, -60], warmup: 60 }),
  s("rs_fog", "render", "hollow_manor", 72, "Push through the fog toward the manor, lantern light",
    { eye: [0, 4.9, 24], target: [0, 5, -18], warmup: 60 }),
  s("rs_abyss", "render", "abyss", 72, "The angler's lure in the deep: depth of field, emissive light",
    { eye: [12, 4, 5], target: [5, 4.2, -6], warmup: 200 }),
  s("rs_hair", "render", "hair_lookdev", 72, "Strand hair: long straight groom, slow turntable in wind (100k strands, Marschner shading)",
    { copyFrom: "docs/images/hair_straight_blond.jpg" }),
  s("rs_fur", "render", "hair_lookdev", 72, "Fur creature close-up, slow orbit",
    { copyFrom: "docs/images/fur_closeup.jpg" }),
  s("rs_particles", "render", "vfx_lookdev", 72, "Ember storm: a million GPU particles with curl noise",
    { copyFrom: "docs/images/vfx_ember_storm.jpg" }),
  s("rs_vortex", "render", "vfx_lookdev", 72, "Magic vortex: GPU particles, ribbons, light emission",
    { copyFrom: "docs/images/vfx_magic_vortex.jpg" }),

  // 7. Variety.
  s("var_platformer2d", "variety", "sky_dash", 96, "2D platformer gameplay from the game camera, the hero running and collecting coins",
    { sceneCamera: true, warmup: 150 }),
  s("var_platformer3d", "variety", "cloudhopper", 96, "Aerial orbit over six floating islands",
    { eye: [-3, 18, 34], target: [0, 0, 0], warmup: 60 }),
  s("var_kart", "variety", "toy_kart_rally", 96, "Stadium orbit at dusk, karts racing, fireworks",
    { eye: [-29.6, 16, 26], target: [0, 2, -4], warmup: 120 }),
  s("var_shmup", "variety", "star_lancer", 96, "Wide over the ringed giant, ships in formation",
    { eye: [-3, 8, 40], target: [10, -6, -40], warmup: 240 }),
  s("var_park", "variety", "harvest_fair", 96, "Carousel orbit at golden hour",
    { eye: [-6.5, 2.6, 4.5], target: [-12, 2.4, -2], warmup: 90 }),
  s("var_zen", "variety", "zen_garden", 96, "Blossoms falling over the pond",
    { eye: [12.5, 1.4, 5], target: [8, 4.2, 0], warmup: 120 }),
  s("var_noir", "variety", "cyber_alley", 96, "Crane up the neon street in the rain; the film overlays a dialogue box",
    { eye: [0.5, 5.7, 12], target: [0, 8, -30], warmup: 90 }),
  s("var_horror", "variety", "hollow_manor", 96, "Graveyard in the fog, lantern sway",
    { eye: [-16, 1.9, 8], target: [-10, 1.4, -1], warmup: 60 }),
  s("var_cozy", "variety", "hearthside", 96, "The tree and the window, snow outside",
    { eye: [1.65, 1.1, 0], target: [3.6, 1.3, -2.7], warmup: 90 }),
  s("var_arena", "variety", "neon_drift", 96, "Low orbit of the neon arena, drones swarming",
    { eye: [-10.3, 2.2, 14.7], target: [0, 1.5, -4], warmup: 240 }),
  s("var_frost", "variety", "frostlight", 96, "Penguins heading home across the ice",
    { eye: [8, 0.8, 9], target: [2, 0.8, 0], warmup: 200 }),
  s("var_village", "variety", "sky_village", 96, "Sky Village orbit (built live by agents, 69 undo steps)",
    { eye: [32, 12.5, 11], target: [0, 1.2, 0], warmup: 120 }),
  s("var_strategy", "variety", "flagship_strategy_map", 96, "A fictional continent as a strategy map: borders, unit tokens, fog of war, slow zoom", undefined, "final", { pending: true }),
  s("var_farm", "variety", "flagship_cozy_farm", 96, "Cozy farm sim: crops swaying, a farmer, morning light", undefined, "final", { pending: true }),

  // 8. Works with your tools.
  s("tools_blender", "tools", "dcc_tower", 120, "dcc_generate tower (ruin 0.6) arriving from Blender into the scene, then placed on the cliff", undefined, "final", { pending: true }),
  s("tools_app", "tools", "sky_dash", 120, "The packaged Sky Dash.app running full screen", { sceneCamera: true, warmup: 300 }),

  // 9. Finale: three worlds, sketch -> clay -> final, then the hero.
  ...triple("fin_a", "finale", "hidden_alley", 90, "Crane up above the lamps in the rain",
    { eye: [0.3, 6.5, 10.5], target: [0, 1.25, -30], fov: 50, warmup: 120 }),
  ...triple("fin_b", "finale", "namaqua_canyon", 90, "Sweep over the canyon toward the sun",
    { eye: [18, 18, 50], target: [0, 2, -40], fov: 45, warmup: 60 }),
  ...triple("fin_c", "finale", "zen_garden", 90, "Through the torii gate",
    { eye: [4, 1.8, 3], target: [4, 3, -16], warmup: 120 }),
  s("fin_hero", "finale", "flagship_mythic_valley", 180, "Final hero: the mythic valley at dawn, slow rise into the clouds", undefined, "final", { pending: true }),
];

export const SLOT = Object.fromEntries(FOOTAGE.map((x) => [x.id, x])) as Record<string, Slot>;
