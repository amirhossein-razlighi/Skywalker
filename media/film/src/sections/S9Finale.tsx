import React from "react";
import { AbsoluteFill, interpolate, Sequence, useCurrentFrame } from "remotion";
import { clipOffset, Footage } from "../components/Footage";
import { Lockup } from "../components/Logo";
import { Backdrop, LightLeak } from "../components/Overlays";
import { SketchReveal } from "../components/SketchReveal";
import { MaskLine, Words } from "../components/Type";
import { BAR, BEAT } from "../timeline";
import { C, EASE, FONT, prog } from "../theme";

/**
 * Finale (8 bars, 576 frames).
 *  bars 0-2: three worlds reveal side by side, sketch -> clay -> final, staggered
 *  bars 2-4: accelerating beat cuts over the riser
 *  bars 4-6: the hero shot on the impact
 *  bars 6-8: end card
 */

const PANELS = [
  { slot: "fin_a", word: "Sketch it." },
  { slot: "fin_b", word: "Shape it." },
  { slot: "fin_c", word: "Light it." },
];

const Triptych: React.FC = () => {
  const frame = useCurrentFrame();
  const out = prog(frame, 2 * BAR - 12, 2 * BAR, EASE.in);
  return (
    <AbsoluteFill style={{ background: "#000" }}>
      {PANELS.map((p, i) => {
        const o = i * 10;
        const open = prog(frame, o, o + 22, EASE.out);
        return (
          <div
            key={p.slot}
            style={{
              position: "absolute",
              top: 0,
              bottom: 0,
              left: i * 640 + 3,
              width: 634,
              overflow: "hidden",
              clipPath: `inset(${(1 - open) * 50}% 0 ${(1 - open) * 50}% 0)`,
              opacity: 1 - out,
            }}
          >
            <SketchReveal
              slot={p.slot}
              t={{ draw: [o, o + 60], sketch: [o + 24, o + 60], clay: [o + 52, o + 82], final: [o + 80, o + 112] }}
              origin={[50, 20]}
              wipeAngle={180}
              playAt={o + 40}
              move={{ from: [0, 0, 1.0], to: [0, 0, 1.04], duration: 2 * BAR }}
            />
            <div style={{ position: "absolute", inset: 0, background: "linear-gradient(0deg, rgba(0,0,0,0.6) 0%, rgba(0,0,0,0) 35%)" }} />
            <div style={{ position: "absolute", left: 0, right: 0, bottom: 90, display: "flex", justifyContent: "center" }}>
              <MaskLine at={o + 20} dur={20}>
                <div style={{ fontFamily: FONT.display, fontWeight: 700, fontSize: 64, letterSpacing: -2, color: "#fff", textShadow: "0 2px 30px rgba(0,0,0,0.6)" }}>{p.word}</div>
              </MaskLine>
            </div>
          </div>
        );
      })}
    </AbsoluteFill>
  );
};

/** Every world once more, faster and faster: city, sea, front, valley, rain, pond, 2D, fire, hall, signs, berries, sun. */
const CUTS = ["rs_neon_skyline", "rs_isle_swash", "var_front", "rs_peaks_lake", "rs_neon_puddle", "rs_farm_pond", "var_gloam_run", "rs_isle_campfire", "rs_peaks_hall", "rs_neon_holo", "rs_farm_rows", "rs_isle_palms"];

/** Cuts that accelerate into the impact: 2 beats, then 1 beat, then half beats. */
const Accelerate: React.FC = () => {
  const frame = useCurrentFrame();
  const lens = [2, 2, 1, 1, 1, 1, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5].map((b) => b * BEAT);
  let at = 0;
  const shots = CUTS.map((s, i) => {
    const r = { slot: s, from: at, dur: lens[i] };
    at += lens[i];
    return r;
  });
  return (
    <AbsoluteFill style={{ background: "#000" }}>
      {shots.map((s, i) => (
        <Sequence key={s.slot} from={s.from} durationInFrames={s.dur}>
          <AbsoluteFill style={{ transform: `scale(${1.04 + i * 0.01})` }}>
            <Footage slot={s.slot} duration={s.dur} offset={clipOffset(s.slot, s.dur)} kb={{ from: [0, 0, 1.0], to: [i % 2 ? 1 : -1, 0, 1.06] }} />
          </AbsoluteFill>
        </Sequence>
      ))}
      <AbsoluteFill style={{ background: "#fff", opacity: interpolate(frame, [2 * BAR - 10, 2 * BAR], [0, 0.9], { extrapolateLeft: "clamp", extrapolateRight: "clamp" }) }} />
    </AbsoluteFill>
  );
};

const Hero: React.FC = () => {
  const frame = useCurrentFrame();
  const flash = interpolate(frame, [0, 10], [0.9, 0], { extrapolateRight: "clamp" });
  return (
    <AbsoluteFill style={{ background: "#000" }}>
      <Footage slot="fin_hero" fallback={["open_hero"]} duration={2 * BAR + 20} kb={{ from: [1, 0.6, 1.16], to: [-1, -0.4, 1.04] }} />
      <AbsoluteFill style={{ background: "radial-gradient(45% 40% at 50% 50%, rgba(5,8,22,0.5) 0%, rgba(5,8,22,0.18) 60%, rgba(0,0,0,0.35) 100%)" }} />
      <AbsoluteFill style={{ alignItems: "center", justifyContent: "center" }}>
        <Words text={"Build worlds\nwith agents."} at={14} out={2 * BAR - 18} size={130} weight={700} style={{ textShadow: "0 4px 50px rgba(0,0,0,0.45)" }} />
      </AbsoluteFill>
      <LightLeak t={interpolate(frame, [0, 2 * BAR], [0.05, 0.75])} x0={90} x1={20} strength={0.55} />
      <AbsoluteFill style={{ background: "#fff", opacity: flash, mixBlendMode: "screen" }} />
    </AbsoluteFill>
  );
};

const EndCard: React.FC = () => {
  const frame = useCurrentFrame();
  const fadeIn = prog(frame, 0, 18);
  const out = prog(frame, 2 * BAR - 30, 2 * BAR, EASE.inOut);
  return (
    <Backdrop y={75} glow={fadeIn}>
      <AbsoluteFill style={{ alignItems: "center", justifyContent: "center", opacity: 1 - out }}>
        <div style={{ marginTop: -60 }}>
          <Lockup at={4} scale={1.05} />
        </div>
        <div style={{ position: "absolute", top: 650 }}>
          <MaskLine at={44} dur={26}>
            <div style={{ fontFamily: FONT.display, fontWeight: 400, fontSize: 46, letterSpacing: -0.6, color: C.dim, textAlign: "center" }}>The game engine built for AI agents.</div>
          </MaskLine>
        </div>
        <div style={{ position: "absolute", bottom: 110, fontFamily: FONT.body, fontWeight: 500, fontSize: 22, letterSpacing: 4, color: C.faint, textTransform: "uppercase", opacity: prog(frame, 70, 90) }}>
          macOS · Apple silicon · Metal
        </div>
      </AbsoluteFill>
    </Backdrop>
  );
};

export const S9Finale: React.FC = () => (
  <AbsoluteFill style={{ background: "#000" }}>
    <Sequence durationInFrames={2 * BAR}>
      <Triptych />
    </Sequence>
    <Sequence from={2 * BAR} durationInFrames={2 * BAR}>
      <Accelerate />
    </Sequence>
    <Sequence from={4 * BAR} durationInFrames={2 * BAR}>
      <Hero />
    </Sequence>
    <Sequence from={6 * BAR} durationInFrames={2 * BAR}>
      <EndCard />
    </Sequence>
  </AbsoluteFill>
);
