import React from "react";
import { AbsoluteFill, interpolate, useCurrentFrame } from "remotion";
import { Lockup } from "../components/Logo";
import { Backdrop, LightLeak } from "../components/Overlays";
import { SketchReveal } from "../components/SketchReveal";
import { MaskLine } from "../components/Type";
import { bars } from "../timeline";
import { C, EASE, FONT, prog } from "../theme";

/**
 * Cold open (11 bars). A world draws itself in light, takes shape in clay, then the sun blooms it into the
 * final render. On the downbeat of bar 8 we cut to black and the logo draws itself.
 */
export const LOGO_AT = bars(8);

const Caption: React.FC<{ text: string; at: number; out: number }> = ({ text, at, out }) => (
  <div style={{ position: "absolute", left: 0, right: 0, bottom: 118, display: "flex", justifyContent: "center" }}>
    <MaskLine at={at} out={out} dur={26}>
      <div
        style={{
          fontFamily: FONT.display,
          fontWeight: 500,
          fontSize: 54,
          letterSpacing: -1.4,
          color: C.text,
          textShadow: "0 2px 30px rgba(0,0,0,0.55)",
        }}
      >
        {text}
      </div>
    </MaskLine>
  </div>
);

export const S1Open: React.FC = () => {
  const frame = useCurrentFrame();
  const t = { draw: [8, 250] as [number, number], sketch: [150, 280] as [number, number], clay: [292, 372] as [number, number], final: [410, 492] as [number, number] };
  // dip to black right before the logo hit
  const dip = prog(frame, LOGO_AT - 14, LOGO_AT, EASE.in);
  const fadeIn = prog(frame, 0, 20);
  const logo = frame >= LOGO_AT;
  const end = prog(frame, bars(11) - 18, bars(11), EASE.in);

  return (
    <AbsoluteFill style={{ background: "#000" }}>
      {!logo && (
        <AbsoluteFill style={{ opacity: fadeIn * (1 - dip) }}>
          <SketchReveal
            slot="open_hero"
            t={t}
            origin={[66, 22]}
            wipeAngle={96}
            move={{ from: [0.6, 0.4, 1.02], to: [-0.8, -0.2, 1.13], duration: LOGO_AT }}
          />
          <LightLeak t={interpolate(frame, [440, 560], [0, 1], { extrapolateLeft: "clamp", extrapolateRight: "clamp" })} x0={80} x1={10} strength={0.5} />
          {/* gentle cinematic letterbox while the world forms */}
          <Caption text="Every world starts as a sketch." at={56} out={226} />
          <Caption text="Then it takes shape." at={300} out={392} />
          <Caption text="Then it comes to life." at={430} out={540} />
        </AbsoluteFill>
      )}
      {logo && (
        <Backdrop glow={interpolate(frame, [LOGO_AT, LOGO_AT + 60], [0, 1], { extrapolateRight: "clamp" })} y={70}>
          <AbsoluteFill style={{ alignItems: "center", justifyContent: "center", opacity: 1 - end }}>
            <div style={{ transform: `translateY(${-40 * prog(frame, LOGO_AT + 70, LOGO_AT + 110)}px)` }}>
              <Lockup at={LOGO_AT + 2} scale={1} />
            </div>
            <div style={{ position: "absolute", top: 640 }}>
              <MaskLine at={LOGO_AT + 92} dur={28}>
                <div style={{ fontFamily: FONT.display, fontWeight: 400, fontSize: 46, letterSpacing: -0.6, color: C.dim }}>
                  The game engine built for AI agents.
                </div>
              </MaskLine>
            </div>
          </AbsoluteFill>
        </Backdrop>
      )}
      {/* white flash on the hit */}
      <AbsoluteFill
        style={{
          background: "#dfe6ff",
          opacity: interpolate(frame, [LOGO_AT, LOGO_AT + 2, LOGO_AT + 14], [0, 0.32, 0], { extrapolateLeft: "clamp", extrapolateRight: "clamp" }),
          mixBlendMode: "screen",
        }}
      />
    </AbsoluteFill>
  );
};
