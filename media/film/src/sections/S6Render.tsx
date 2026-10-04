import React from "react";
import { AbsoluteFill, interpolate, Sequence, useCurrentFrame } from "remotion";
import { Footage, pickSlot, resolveSlot } from "../components/Footage";
import { Backdrop, LightLeak } from "../components/Overlays";
import { Kicker, MaskLine, Words } from "../components/Type";
import { BAR, BEAT } from "../timeline";
import { C, EASE, FONT, prog } from "../theme";

/**
 * Rendering showcase (14 bars). Bar 1: title on the drop. Bars 2-13: fast montage cut on the beat, one
 * feature callout per shot. Bar 14: Apple-silicon numbers.
 * Vocabulary: whip pans with directional motion blur, zoom-throughs, flash cuts; all on the beat.
 */

type Shot = { slot: string; beats: number; title: string; sub: string; fallback?: string[]; tr?: "whip" | "zoom" | "flash" };

const SHOTS: Shot[] = [
  { slot: "rs_ocean", beats: 4, title: "FFT ocean", sub: "JONSWAP waves, whitecaps, refraction", tr: "flash" },
  { slot: "rs_galleon", beats: 2, title: "Volumetric light", sub: "Sun shafts through the rigging", tr: "whip" },
  { slot: "rs_beach", beats: 2, title: "Coastlines", sub: "Shore surf, wet sand, foam lines", tr: "zoom" },
  { slot: "rs_clouds", beats: 4, title: "Volumetric clouds", sub: "Ray-marched, multiple scattering", tr: "whip" },
  { slot: "rs_terrain", beats: 2, title: "Eroded terrain", sub: "Heightfields up to 4097², eight blended layers", tr: "zoom" },
  { slot: "rs_valley", beats: 2, title: "Instanced foliage", sub: "Wind-animated forests, automatic LODs", tr: "whip" },
  { slot: "rs_canyon", beats: 2, title: "Atmosphere", sub: "Aerial perspective and god rays", tr: "flash" },
  { slot: "rs_campfire", beats: 2, title: "Fluid fire", sub: "3D GPU simulation, blackbody flames", tr: "zoom" },
  { slot: "rs_rain", beats: 4, title: "Clustered lighting", sub: "Up to 1,024 lights, rain and mist", tr: "whip" },
  { slot: "rs_puddle", beats: 2, title: "Screen-space reflections", sub: "GGX importance-sampled", tr: "zoom" },
  { slot: "rs_neon_city", beats: 2, title: "Neon in the rain", sub: "Every light reflected, every drop lit", tr: "whip" },
  { slot: "rs_fire", beats: 2, title: "Fire and steam", sub: "Combustion, buoyancy, vorticity", tr: "flash" },
  { slot: "rs_gi", beats: 4, title: "Global illumination", sub: "Firelight bouncing through the room", tr: "zoom" },
  { slot: "rs_fog", beats: 2, title: "Height fog", sub: "Lanterns in the mist", tr: "whip" },
  { slot: "rs_aurora", beats: 2, title: "HDR and bloom", sub: "AgX, ACES and filmic tonemapping", tr: "zoom" },
  { slot: "rs_abyss", beats: 2, title: "Emissive light", sub: "Three hundred meters down", tr: "whip" },
  { slot: "rs_hair", beats: 4, title: "Strand hair", sub: "100k strands, Marschner shading", tr: "flash" },
  { slot: "rs_fur", beats: 2, title: "Fur", sub: "GPU-simulated, self-shadowed", tr: "zoom" },
  { slot: "rs_particles", beats: 2, title: "GPU particles", sub: "Millions per emitter", tr: "whip" },
  { slot: "rs_vortex", beats: 4, title: "Curl noise and ribbons", sub: "Particles that emit light", tr: "zoom" },
];

const MONTAGE_FRAMES = 12 * BAR;

/** Lay the shots out on the beat grid, dropping pending shots that have no media yet and
 *  stretching the rest (in whole beats) so the montage always fills its 12 bars. */
const layout = () => {
  const live = SHOTS.filter((s) => resolveSlot(pickSlot(s.slot, s.fallback)).kind !== "none");
  const totalBeats = MONTAGE_FRAMES / BEAT;
  let used = live.reduce((a, s) => a + s.beats, 0);
  const beats = live.map((s) => s.beats);
  // add beats to the 2-beat shots first, round-robin, until we fill the grid
  let i = 0;
  while (used < totalBeats && live.length) {
    const k = i % live.length;
    if (beats[k] < 4 || i > live.length * 2) {
      beats[k] += 2;
      used += 2;
    }
    i++;
  }
  while (used > totalBeats) {
    const k = beats.indexOf(Math.max(...beats));
    beats[k] -= 2;
    used -= 2;
  }
  let at = 0;
  return live.map((s, k) => {
    const from = at;
    const dur = beats[k] * BEAT;
    at += dur;
    return { ...s, from, dur };
  });
};

/** Directional (horizontal) motion blur via an SVG filter, for whip pans. */
const MotionBlurDefs: React.FC<{ id: string; amount: number }> = ({ id, amount }) => (
  <svg width={0} height={0} style={{ position: "absolute" }}>
    <filter id={id} x="-10%" y="0" width="120%" height="100%">
      <feGaussianBlur stdDeviation={`${amount} 0`} />
    </filter>
  </svg>
);

const TR = 8; // transition frames (overlap with the next shot)

const ShotView: React.FC<{ shot: ReturnType<typeof layout>[number]; index: number; next?: ReturnType<typeof layout>[number] }> = ({ shot, index, next }) => {
  const frame = useCurrentFrame(); // local to this shot, 0..dur+TR
  const dir = index % 2 ? 1 : -1;
  // incoming
  const tIn = shot.tr === "whip" ? prog(frame, 0, TR, EASE.out) : 1;
  const zoomIn = shot.tr === "zoom" ? prog(frame, 0, TR + 4, EASE.out) : 1;
  // outgoing (into the next shot's transition)
  const outT = next ? prog(frame, shot.dur - 0, shot.dur + TR, EASE.in) : 0;
  const nextTr = next?.tr;
  let tx = 0;
  let scale = 1;
  let blur = 0;
  let opacity = 1;
  if (shot.tr === "whip" && tIn < 1) {
    tx = (1 - tIn) * 100 * -dir;
    blur = (1 - tIn) * 90;
  }
  if (nextTr === "whip" && outT > 0) {
    tx = outT * 100 * dir;
    blur = outT * 90;
  }
  if (shot.tr === "zoom" && zoomIn < 1) {
    scale = interpolate(zoomIn, [0, 1], [0.86, 1]);
    opacity = zoomIn;
  }
  if (nextTr === "zoom" && outT > 0) {
    scale = 1 + outT * 0.35;
    opacity = 1 - outT;
  }
  const fid = `mb${index}`;
  const kbFrom: [number, number, number] = index % 3 === 0 ? [1.5 * dir, 0, 1.06] : index % 3 === 1 ? [0, 1, 1.12] : [-1 * dir, -0.5, 1.03];
  const kbTo: [number, number, number] = index % 3 === 0 ? [-1.5 * dir, 0, 1.12] : index % 3 === 1 ? [0, -0.5, 1.04] : [1.2 * dir, 0.4, 1.1];
  const capIn = Math.min(8, shot.dur / 4);
  return (
    <AbsoluteFill style={{ zIndex: 1 }}>
      {blur > 0.5 && <MotionBlurDefs id={fid} amount={blur} />}
      <AbsoluteFill
        style={{
          transform: `translateX(${tx}%) scale(${scale})`,
          filter: blur > 0.5 ? `url(#${fid})` : undefined,
          opacity,
        }}
      >
        <Footage slot={shot.slot} fallback={shot.fallback} duration={shot.dur + TR} kb={{ from: kbFrom, to: kbTo }} />
        {/* lower-left callout */}
        <AbsoluteFill style={{ background: "linear-gradient(0deg, rgba(0,0,0,0.55) 0%, rgba(0,0,0,0) 32%)" }} />
        <div style={{ position: "absolute", left: 96, bottom: 92 }}>
          <div style={{ width: interpolate(prog(frame, capIn, capIn + 14), [0, 1], [0, 56]), height: 3, background: `linear-gradient(90deg, ${C.sky}, ${C.violet})`, marginBottom: 18, borderRadius: 2 }} />
          <MaskLine at={capIn} dur={16}>
            <div style={{ fontFamily: FONT.display, fontWeight: 600, fontSize: 58, letterSpacing: -1.8, color: "#fff", textShadow: "0 2px 24px rgba(0,0,0,0.5)" }}>{shot.title}</div>
          </MaskLine>
          <MaskLine at={capIn + 4} dur={16}>
            <div style={{ fontFamily: FONT.body, fontWeight: 500, fontSize: 25, color: "rgba(240,242,255,0.78)", marginTop: 6 }}>{shot.sub}</div>
          </MaskLine>
        </div>
      </AbsoluteFill>
      {shot.tr === "flash" && (
        <AbsoluteFill style={{ background: "#fff", opacity: interpolate(frame, [0, 1, 6], [0.38, 0.12, 0], { extrapolateLeft: "clamp", extrapolateRight: "clamp" }), mixBlendMode: "screen" }} />
      )}
    </AbsoluteFill>
  );
};

const Montage: React.FC = () => {
  const shots = layout();
  return (
    <AbsoluteFill style={{ background: "#000" }}>
      {shots.map((s, i) => (
        <Sequence key={s.slot} from={s.from} durationInFrames={s.dur + (i < shots.length - 1 ? TR : 0)} name={s.slot}>
          <ShotView shot={s} index={i} next={shots[i + 1]} />
        </Sequence>
      ))}
    </AbsoluteFill>
  );
};

const Title: React.FC = () => {
  const frame = useCurrentFrame();
  const out = prog(frame, BAR - 10, BAR, EASE.in);
  return (
    <Backdrop y={50} glow={1.2}>
      <AbsoluteFill style={{ alignItems: "center", justifyContent: "center", opacity: 1 - out, transform: `scale(${1 + 0.25 * out})`, filter: `blur(${out * 8}px)` }}>
        <Kicker text="The Metal renderer" at={0} color={C.sunset} />
        <div style={{ height: 18 }} />
        <Words text={"Light, *simulated.*"} at={0} size={150} weight={700} stagger={4} />
      </AbsoluteFill>
      <LightLeak t={interpolate(frame, [0, BAR], [0.1, 0.9])} strength={0.6} />
    </Backdrop>
  );
};

const Silicon: React.FC = () => {
  const frame = useCurrentFrame();
  const stats = [
    { v: "7.4 ms", l: "a full 1080p frame of the baseline scene" },
    { v: "1M", l: "GPU particles in a 16 ms frame" },
    { v: "100k", l: "hair strands, simulated and shaded live" },
  ];
  return (
    <Backdrop y={100}>
      <div style={{ position: "absolute", left: 0, right: 0, top: 250, display: "flex", flexDirection: "column", alignItems: "center" }}>
        <Words text="Tuned for Apple silicon." at={2} size={96} weight={700} />
        <div style={{ height: 18 }} />
        <div style={{ fontFamily: FONT.body, fontSize: 26, color: C.dim, opacity: prog(frame, 14, 26) }}>
          MetalFX upscaling · temporal AA · one renderer, from the editor to the shipped game
        </div>
      </div>
      <div style={{ position: "absolute", left: 180, right: 180, top: 560, display: "flex", justifyContent: "space-between" }}>
        {stats.map((s, i) => {
          const p = prog(frame, 16 + i * 5, 34 + i * 5);
          return (
            <div key={s.v} style={{ width: 460, textAlign: "center", opacity: p, transform: `translateY(${(1 - p) * 24}px)` }}>
              <div style={{ fontFamily: FONT.display, fontWeight: 700, fontSize: 104, letterSpacing: -4, color: C.text }}>{s.v}</div>
              <div style={{ fontFamily: FONT.body, fontSize: 24, color: C.dim, marginTop: 4 }}>{s.l}</div>
            </div>
          );
        })}
      </div>
      <div style={{ position: "absolute", left: 0, right: 0, bottom: 70, textAlign: "center", fontFamily: FONT.body, fontSize: 16, color: C.faint, opacity: prog(frame, 30, 44) }}>
        fx_benchmark and engine measurements on an M1 Pro
      </div>
    </Backdrop>
  );
};

export const S6Render: React.FC = () => (
  <AbsoluteFill style={{ background: "#000" }}>
    <Sequence durationInFrames={BAR}>
      <Title />
    </Sequence>
    <Sequence from={BAR} durationInFrames={MONTAGE_FRAMES}>
      <Montage />
    </Sequence>
    <Sequence from={13 * BAR} durationInFrames={BAR}>
      <Silicon />
    </Sequence>
  </AbsoluteFill>
);
