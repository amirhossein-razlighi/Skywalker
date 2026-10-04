import React from "react";
import { AbsoluteFill, interpolate, staticFile, useCurrentFrame } from "remotion";
import { C, hash } from "../theme";

/** Film grain: a noise tile jumped to a new random offset every frame, overlay-blended. */
export const Grain: React.FC<{ opacity?: number }> = ({ opacity = 0.07 }) => {
  const frame = useCurrentFrame();
  const x = Math.floor(hash(frame) * 512);
  const y = Math.floor(hash(frame + 91.7) * 512);
  return (
    <AbsoluteFill
      style={{
        backgroundImage: `url(${staticFile("textures/grain.png")})`,
        backgroundPosition: `${x}px ${y}px`,
        backgroundSize: "512px 512px",
        mixBlendMode: "overlay",
        opacity,
        pointerEvents: "none",
      }}
    />
  );
};

export const Vignette: React.FC<{ strength?: number }> = ({ strength = 0.55 }) => (
  <AbsoluteFill
    style={{
      background: `radial-gradient(120% 95% at 50% 50%, transparent 55%, rgba(2,3,10,${strength}) 100%)`,
      pointerEvents: "none",
    }}
  />
);

/** A soft, warm light leak that drifts across the frame. `t` 0..1 is its life. */
export const LightLeak: React.FC<{ t: number; hue?: "warm" | "cool"; x0?: number; x1?: number; strength?: number }> = ({
  t,
  hue = "warm",
  x0 = -20,
  x1 = 120,
  strength = 0.85,
}) => {
  if (t <= 0 || t >= 1) return null;
  const a = Math.sin(Math.PI * t) * strength;
  const x = interpolate(t, [0, 1], [x0, x1]);
  const c1 = hue === "warm" ? "255,184,115" : "120,150,255";
  const c2 = hue === "warm" ? "237,107,122" : "154,134,255";
  return (
    <AbsoluteFill style={{ mixBlendMode: "screen", opacity: a, pointerEvents: "none" }}>
      <AbsoluteFill
        style={{
          background: `radial-gradient(45% 80% at ${x}% 40%, rgba(${c1},0.9) 0%, rgba(${c2},0.35) 35%, transparent 70%)`,
          filter: "blur(30px)",
        }}
      />
      <AbsoluteFill
        style={{
          background: `radial-gradient(25% 50% at ${x + 18}% 70%, rgba(${c1},0.6) 0%, transparent 70%)`,
          filter: "blur(40px)",
        }}
      />
    </AbsoluteFill>
  );
};

/** Deep brand background: void with a faint indigo glow. */
export const Backdrop: React.FC<{ glow?: number; x?: number; y?: number; children?: React.ReactNode }> = ({ glow = 1, x = 50, y = 110, children }) => (
  <AbsoluteFill style={{ background: C.void }}>
    <AbsoluteFill
      style={{
        background: `radial-gradient(80% 70% at ${x}% ${y}%, rgba(34,42,104,${0.75 * glow}) 0%, rgba(19,24,56,${0.5 * glow}) 40%, transparent 75%)`,
      }}
    />
    {children}
  </AbsoluteFill>
);

/** Thin letterbox bars for cinematic moments (0..1 amount). */
export const Letterbox: React.FC<{ amount: number }> = ({ amount }) => {
  const h = 70 * amount;
  return (
    <>
      <div style={{ position: "absolute", left: 0, right: 0, top: 0, height: h, background: "#000" }} />
      <div style={{ position: "absolute", left: 0, right: 0, bottom: 0, height: h, background: "#000" }} />
    </>
  );
};
