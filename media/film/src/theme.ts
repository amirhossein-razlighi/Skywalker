import { loadFont as loadInterTight } from "@remotion/google-fonts/InterTight";
import { loadFont as loadInter } from "@remotion/google-fonts/Inter";
import { loadFont as loadMono } from "@remotion/google-fonts/JetBrainsMono";
import { Easing, interpolate, spring, SpringConfig } from "remotion";

const display = loadInterTight("normal", { weights: ["300", "400", "500", "600", "700", "800"], subsets: ["latin"] });
const body = loadInter("normal", { weights: ["400", "500", "600"], subsets: ["latin"] });
const mono = loadMono("normal", { weights: ["400", "500", "700"], subsets: ["latin"] });

export const FONT = {
  display: display.fontFamily,
  body: body.fontFamily,
  mono: mono.fontFamily,
};

/** Brand palette (docs/BRAND.md): navy/indigo field, sky blue + AI violet, one warm sunset accent. */
export const C = {
  void: "#04050d",
  ink: "#070919",
  navy: "#0b1030",
  deep: "#131838",
  night: "#222a68",
  sky: "#4f8dff",
  violet: "#9a86ff",
  sunset: "#ffb873",
  rose: "#ed6b7a",
  gold: "#e2b062",
  text: "#f4f5fb",
  dim: "rgba(226, 230, 255, 0.58)",
  faint: "rgba(226, 230, 255, 0.28)",
  hair: "rgba(226, 230, 255, 0.12)",
  // editor UI surfaces (editor/Sources/Theme/Theme.swift)
  window: "#151619",
  panel: "#1c1d21",
  panel2: "#24252b",
  uiText: "#e3e4e8",
  selection: "#ff7a1a",
  green: "#5fd68a",
  red: "#ff6b6b",
};

export const BRAND_GRADIENT = `linear-gradient(100deg, ${C.sky} 0%, ${C.violet} 55%, ${C.sunset} 100%)`;
export const COOL_GRADIENT = `linear-gradient(100deg, #7fb0ff 0%, ${C.violet} 100%)`;

/** Apple-like curves. */
export const EASE = {
  out: Easing.bezier(0.16, 1, 0.3, 1), // expo-ish out: fast start, long settle
  inOut: Easing.bezier(0.65, 0, 0.35, 1),
  in: Easing.bezier(0.7, 0, 0.84, 0),
  soft: Easing.bezier(0.33, 1, 0.68, 1),
};

export const SPRING: Record<string, Partial<SpringConfig>> = {
  gentle: { damping: 200, mass: 1, stiffness: 80 },
  snappy: { damping: 18, mass: 0.6, stiffness: 140 },
  pop: { damping: 12, mass: 0.5, stiffness: 160 },
  smooth: { damping: 200 },
};

export const clamp01 = (x: number) => Math.max(0, Math.min(1, x));

/** Eased 0..1 progress between two frames. */
export const prog = (frame: number, from: number, to: number, ease = EASE.out) =>
  interpolate(frame, [from, to], [0, 1], { easing: ease, extrapolateLeft: "clamp", extrapolateRight: "clamp" });

export const sp = (frame: number, fps: number, delay = 0, cfg: Partial<SpringConfig> = SPRING.gentle, durationInFrames?: number) =>
  spring({ frame: frame - delay, fps, config: cfg, durationInFrames });

export const lerp = (a: number, b: number, t: number) => a + (b - a) * t;

/** Deterministic hash noise in [0,1). */
export const hash = (n: number) => {
  const s = Math.sin(n * 127.1 + 311.7) * 43758.5453;
  return s - Math.floor(s);
};
