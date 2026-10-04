import React, { useEffect, useState } from "react";
import { AbsoluteFill, continueRender, delayRender, getStaticFiles, interpolate, staticFile, useCurrentFrame } from "remotion";
import { C, EASE, prog } from "../theme";
import { Footage, hasSlot } from "./Footage";

type Linework = { w: number; h: number; paths: [number, number, number, string][] };

const lineworkCache: Record<string, Linework | null> = {};

/** Loads public/linework/<slot>.json (traced from the engine's sketch view by scripts/trace_lines.py). */
export const useLinework = (slot: string): Linework | null => {
  const exists = getStaticFiles().some((f) => f.name === `linework/${slot}.json`);
  const [data, setData] = useState<Linework | null>(lineworkCache[slot] ?? null);
  const [handle] = useState(() => (exists && !lineworkCache[slot] ? delayRender(`linework ${slot}`) : null));
  useEffect(() => {
    if (!exists || lineworkCache[slot]) return;
    fetch(staticFile(`linework/${slot}.json`))
      .then((r) => r.json())
      .then((j: Linework) => {
        lineworkCache[slot] = j;
        setData(j);
        if (handle !== null) continueRender(handle);
      })
      .catch(() => handle !== null && continueRender(handle));
  }, [exists, handle, slot]);
  return data;
};

/** Strokes that draw themselves: progress 0..1 maps onto each stroke's own [t0, t1] window. */
export const DrawnLines: React.FC<{ data: Linework; progress: number; color: string; width?: number; glow?: boolean }> = ({
  data,
  progress,
  color,
  width = 1.7,
  glow,
}) => (
  <svg viewBox={`0 0 ${data.w} ${data.h}`} preserveAspectRatio="xMidYMid slice" style={{ position: "absolute", inset: 0, width: "100%", height: "100%" }}>
    {glow && (
      <defs>
        <filter id="lineglow" x="-5%" y="-5%" width="110%" height="110%">
          <feGaussianBlur stdDeviation="2.4" result="b" />
          <feMerge>
            <feMergeNode in="b" />
            <feMergeNode in="SourceGraphic" />
          </feMerge>
        </filter>
      </defs>
    )}
    <g fill="none" stroke={color} strokeWidth={width} strokeLinecap="round" strokeLinejoin="round" filter={glow ? "url(#lineglow)" : undefined}>
      {data.paths.map(([t0, t1, len, d], i) => {
        if (progress <= t0) return null;
        const p = Math.min(1, (progress - t0) / Math.max(0.001, t1 - t0));
        const e = EASE.inOut(p);
        return <path key={i} d={d} strokeDasharray={`${len} ${len}`} strokeDashoffset={len * (1 - e)} />;
      })}
    </g>
  </svg>
);

export interface RevealTiming {
  /** line art draws on */
  draw: [number, number];
  /** the engine's sketch render (hatching, full detail) fades in under the strokes */
  sketch: [number, number];
  /** clay wipes in */
  clay: [number, number];
  /** final render blooms in */
  final: [number, number];
}

/**
 * The signature sketch -> clay -> final reveal.
 * Layers stay registered (one shared camera move) so the drawing, the clay and the final image line up.
 *  - "luminous": light strokes on deep navy, sketch render inverted (a drawing made of light)
 *  - "paper": graphite strokes on paper, the sketch render as-is
 */
export const SketchReveal: React.FC<{
  slot: string;
  t: RevealTiming;
  look?: "luminous" | "paper";
  /** where the final image blooms from (percent of frame) */
  origin?: [number, number];
  /** clay wipe angle in degrees */
  wipeAngle?: number;
  /** shared camera move over the whole reveal */
  move?: { from: [number, number, number]; to: [number, number, number]; duration: number };
}> = ({ slot, t, look = "luminous", origin = [62, 30], wipeAngle = 100, move }) => {
  const frame = useCurrentFrame();
  const lines = useLinework(slot);
  const drawP = interpolate(frame, t.draw, [0, 1], { extrapolateLeft: "clamp", extrapolateRight: "clamp" });
  const sketchP = prog(frame, t.sketch[0], t.sketch[1], EASE.inOut);
  const clayP = prog(frame, t.clay[0], t.clay[1], EASE.inOut);
  const finalP = prog(frame, t.final[0], t.final[1], EASE.inOut);
  const mv = move ?? { from: [0, 0, 1.0], to: [0, 0, 1.08], duration: t.final[1] + 60 };
  const mt = prog(frame, 0, mv.duration, EASE.soft);
  const tx = interpolate(mt, [0, 1], [mv.from[0], mv.to[0]]);
  const ty = interpolate(mt, [0, 1], [mv.from[1], mv.to[1]]);
  const ts = interpolate(mt, [0, 1], [mv.from[2], mv.to[2]]);
  const luminous = look === "luminous";

  // Clay: a soft diagonal wipe with a bright leading edge.
  const wipePos = interpolate(clayP, [0, 1], [-25, 125]);
  const clayMask = `linear-gradient(${wipeAngle}deg, black ${wipePos - 12}%, transparent ${wipePos + 2}%)`;
  // Final: a radial bloom from the light source.
  const r = interpolate(finalP, [0, 1], [0, 170]);
  const finalMask = `radial-gradient(circle at ${origin[0]}% ${origin[1]}%, black ${Math.max(0, r - 28)}%, transparent ${r}%)`;
  const linesFade = 1 - prog(frame, t.clay[0], t.clay[0] + (t.clay[1] - t.clay[0]) * 0.7);
  const stack: React.CSSProperties = { transform: `translate(${tx}%, ${ty}%) scale(${ts})`, transformOrigin: "50% 50%" };
  const still = { from: [0, 0, 1] as [number, number, number], to: [0, 0, 1] as [number, number, number] };

  return (
    <AbsoluteFill style={{ background: luminous ? C.navy : "#f3f1ea", overflow: "hidden" }}>
      <AbsoluteFill style={stack}>
        {luminous && (
          <AbsoluteFill style={{ background: `radial-gradient(90% 80% at 50% 45%, ${C.deep} 0%, ${C.ink} 100%)` }} />
        )}
        {/* the engine's own sketch render, adding hatching and the fine detail */}
        {hasSlot(`${slot}_sketch`) && sketchP > 0 && (
          <AbsoluteFill
            style={{
              opacity: sketchP * (luminous ? 0.36 : 1),
              filter: luminous ? "invert(1) sepia(0.3) hue-rotate(190deg) saturate(1.6) brightness(0.9)" : undefined,
              mixBlendMode: luminous ? "screen" : "multiply",
            }}
          >
            <Footage slot={`${slot}_sketch`} kb={still} placeholderLabel={false} />
          </AbsoluteFill>
        )}
        {lines && linesFade > 0 && (
          <AbsoluteFill style={{ opacity: linesFade }}>
            <DrawnLines data={lines} progress={drawP} color={luminous ? "#e9edff" : "#2b2f42"} width={luminous ? 1.6 : 1.5} glow={luminous} />
          </AbsoluteFill>
        )}
        {clayP > 0 && (
          <AbsoluteFill style={{ WebkitMaskImage: clayMask, maskImage: clayMask }}>
            <Footage slot={`${slot}_clay`} kb={still} placeholderLabel={false} />
          </AbsoluteFill>
        )}
        {clayP > 0 && clayP < 1 && (
          <AbsoluteFill
            style={{
              background: `linear-gradient(${wipeAngle}deg, transparent ${wipePos - 14}%, rgba(200,210,255,${0.32 * Math.sin(Math.PI * clayP)}) ${wipePos - 3}%, transparent ${wipePos + 2}%)`,
              mixBlendMode: "screen",
            }}
          />
        )}
        {finalP > 0 && (
          <AbsoluteFill style={{ WebkitMaskImage: finalMask, maskImage: finalMask }}>
            <Footage slot={slot} kb={still} placeholderLabel={false} />
          </AbsoluteFill>
        )}
        {finalP > 0 && finalP < 1 && (
          <AbsoluteFill
            style={{
              background: `radial-gradient(circle at ${origin[0]}% ${origin[1]}%, transparent ${Math.max(0, r - 30)}%, rgba(255,214,170,${0.5 * Math.sin(Math.PI * finalP)}) ${r - 6}%, transparent ${r + 4}%)`,
              mixBlendMode: "screen",
            }}
          />
        )}
      </AbsoluteFill>
    </AbsoluteFill>
  );
};
