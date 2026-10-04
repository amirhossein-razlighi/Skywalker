import React from "react";
import { evolvePath } from "@remotion/paths";
import { Img, interpolate, staticFile, useCurrentFrame, useVideoConfig } from "remotion";
import { C, EASE, SPRING, prog, sp } from "../theme";

/** The custom monoline wordmark (assets/brand/logo/wordmark-*.svg): one stroke path per letter. */
const LETTERS: { x: number; d: string }[] = [
  { x: -2.5, d: "M80 34 C74 18 60 10 45 10 C26 10 12 22 12 42 C12 62 30 70 46 80 C64 90 80 98 80 118 C80 138 66 150 46 150 C28 150 14 142 10 126" },
  { x: 104.5, d: "M12 10 V150 M64 58 L12 108 M30 90 L64 150" },
  { x: 199.5, d: "M10 58 L42 132 M74 58 L34 160 Q26 180 8 182" },
  { x: 302.5, d: "M10 58 L32 150 L55 74 L78 150 L100 58" },
  { x: 441.5, d: "M92 58 V150 M92 104 A46 46 0 1 0 0 104 A46 46 0 1 0 92 104" },
  { x: 566.5, d: "M12 10 V150" },
  { x: 611.5, d: "M12 10 V150 M64 58 L12 108 M30 90 L64 150" },
  { x: 714.5, d: "M0 104 H92 A46 46 0 1 0 79 137" },
  { x: 833.5, d: "M12 58 V150 M12 104 C12 76 30 60 56 62" },
];
const SPARKLE = "M929 6 Q930.92 20.08 945 22 Q930.92 23.92 929 38 Q927.08 23.92 913 22 Q927.08 20.08 929 6 Z";

/** Draws the wordmark letter by letter. `at` = first frame, `per` = frames between letters. */
export const Wordmark: React.FC<{ at: number; width: number; per?: number; color?: string; out?: number }> = ({
  at,
  width,
  per = 4,
  color = C.text,
  out,
}) => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();
  const h = (width * 240) / 999;
  const sparkle = sp(frame, fps, at + LETTERS.length * per + 8, SPRING.pop);
  const exit = out === undefined ? 0 : prog(frame, out, out + 16, EASE.in);
  return (
    <svg width={width} height={h} viewBox="-20 -20 999 240" style={{ overflow: "visible", opacity: 1 - exit }}>
      <g fill="none" stroke={color} strokeWidth={19} strokeLinecap="round" strokeLinejoin="round">
        {LETTERS.map((l, i) => {
          const p = prog(frame, at + i * per, at + i * per + 22, EASE.inOut);
          if (p <= 0) return null;
          const e = evolvePath(p, l.d);
          return <path key={i} transform={`translate(${l.x} 0)`} d={l.d} strokeDasharray={e.strokeDasharray} strokeDashoffset={e.strokeDashoffset} />;
        })}
      </g>
      <path
        d={SPARKLE}
        fill={C.sunset}
        style={{
          transformBox: "fill-box",
          transformOrigin: "center",
          transform: `scale(${sparkle}) rotate(${interpolate(sparkle, [0, 1], [-90, 0])}deg)`,
          filter: `drop-shadow(0 0 ${12 * sparkle}px rgba(255,184,115,0.9))`,
        }}
      />
    </svg>
  );
};

export const AppIcon: React.FC<{ size: number; at: number; glow?: number; style?: React.CSSProperties }> = ({ size, at, glow = 1, style }) => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();
  const s = sp(frame, fps, at, { damping: 14, mass: 0.7, stiffness: 120 });
  const o = interpolate(s, [0, 0.4], [0, 1], { extrapolateRight: "clamp" });
  return (
    <div style={{ width: size, height: size, position: "relative", transform: `scale(${0.6 + 0.4 * s})`, opacity: o, ...style }}>
      <div
        style={{
          position: "absolute",
          inset: -size * 0.25,
          background: `radial-gradient(closest-side, rgba(154,134,255,${0.35 * glow}), rgba(79,141,255,${0.12 * glow}) 60%, transparent)`,
          filter: "blur(20px)",
        }}
      />
      <Img src={staticFile("brand/app-icon.png")} style={{ position: "absolute", inset: 0, width: size, height: size }} />
    </div>
  );
};

/** Horizontal lockup: icon + drawn wordmark, centred. */
export const Lockup: React.FC<{ at: number; scale?: number; out?: number }> = ({ at, scale = 1, out }) => {
  const icon = 210 * scale;
  return (
    <div style={{ display: "flex", alignItems: "center", gap: 46 * scale }}>
      <AppIcon size={icon} at={at} />
      <div style={{ marginTop: 18 * scale }}>
        <Wordmark at={at + 8} width={620 * scale} out={out} />
      </div>
    </div>
  );
};
