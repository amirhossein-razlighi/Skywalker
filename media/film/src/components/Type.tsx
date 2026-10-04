import React from "react";
import { interpolate, spring, useCurrentFrame, useVideoConfig } from "remotion";
import { BRAND_GRADIENT, C, EASE, FONT, SPRING, prog } from "../theme";

const gradientText: React.CSSProperties = {
  backgroundImage: BRAND_GRADIENT,
  WebkitBackgroundClip: "text",
  backgroundClip: "text",
  color: "transparent",
};

/**
 * Headline that rises in word by word with a blur-to-sharp focus pull, and leaves with a soft blur.
 * Words wrapped in *asterisks* get the brand gradient. "\n" breaks a line.
 */
export const Words: React.FC<{
  text: string;
  at: number;
  out?: number;
  size?: number;
  weight?: number;
  stagger?: number;
  color?: string;
  tracking?: number;
  lineHeight?: number;
  align?: "left" | "center" | "right";
  font?: string;
  style?: React.CSSProperties;
  exitFrames?: number;
}> = ({
  text,
  at,
  out,
  size = 110,
  weight = 600,
  stagger = 3,
  color = C.text,
  tracking,
  lineHeight = 1.04,
  align = "center",
  font = FONT.display,
  style,
  exitFrames = 14,
}) => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();
  const lines = text.split("\n");
  let k = 0;
  const exit = out === undefined ? 0 : prog(frame, out, out + exitFrames, EASE.in);
  if (exit >= 1) return null;
  return (
    <div
      style={{
        fontFamily: font,
        fontSize: size,
        fontWeight: weight,
        lineHeight,
        letterSpacing: tracking ?? -0.035 * size,
        color,
        textAlign: align,
        opacity: 1 - exit,
        filter: exit > 0 ? `blur(${exit * 12}px)` : undefined,
        transform: `translateY(${-exit * 0.12 * size}px)`,
        ...style,
      }}
    >
      {lines.map((line, li) => (
        <div key={li} style={{ whiteSpace: "nowrap" }}>
          {line.split(" ").map((w, wi) => {
            const i = k++;
            const p = spring({ frame: frame - at - i * stagger, fps, config: SPRING.gentle });
            const grad = w.startsWith("*") && w.endsWith("*");
            const word = grad ? w.slice(1, -1) : w;
            return (
              <span
                key={wi}
                style={{
                  display: "inline-block",
                  opacity: interpolate(p, [0, 0.6], [0, 1], { extrapolateRight: "clamp" }),
                  transform: `translateY(${(1 - p) * 0.45 * size}px)`,
                  filter: p < 0.999 ? `blur(${(1 - p) * 16}px)` : undefined,
                  marginRight: wi < line.split(" ").length - 1 ? "0.24em" : 0,
                  // keep descenders of gradient words from clipping
                  paddingBottom: grad ? "0.08em" : 0,
                  ...(grad ? gradientText : {}),
                }}
              >
                {word}
              </span>
            );
          })}
        </div>
      ))}
    </div>
  );
};

/** Small uppercase label that slides up out of a mask. */
export const Kicker: React.FC<{ text: string; at: number; out?: number; color?: string; size?: number; style?: React.CSSProperties }> = ({
  text,
  at,
  out,
  color = C.violet,
  size = 26,
  style,
}) => {
  const frame = useCurrentFrame();
  const p = prog(frame, at, at + 20);
  const e = out === undefined ? 0 : prog(frame, out, out + 12, EASE.in);
  return (
    <div style={{ overflow: "hidden", paddingBottom: 4, ...style }}>
      <div
        style={{
          fontFamily: FONT.body,
          fontWeight: 600,
          fontSize: size,
          letterSpacing: interpolate(p, [0, 1], [0.5, 0.18]) + "em",
          textTransform: "uppercase",
          color,
          transform: `translateY(${(1 - p) * 110}%)`,
          opacity: (1 - e) * p,
        }}
      >
        {text}
      </div>
    </div>
  );
};

/** One line that slides up out of a mask (classic keynote reveal). */
export const MaskLine: React.FC<{
  children: React.ReactNode;
  at: number;
  out?: number;
  dur?: number;
  style?: React.CSSProperties;
}> = ({ children, at, out, dur = 24, style }) => {
  const frame = useCurrentFrame();
  const p = prog(frame, at, at + dur);
  const e = out === undefined ? 0 : prog(frame, out, out + 14, EASE.in);
  return (
    <div style={{ overflow: "hidden", paddingBottom: "0.12em", marginBottom: "-0.12em", ...style }}>
      <div style={{ transform: `translateY(${(1 - p) * 105 - e * 105}%)`, opacity: interpolate(p, [0, 0.3], [0, 1], { extrapolateRight: "clamp" }) }}>
        {children}
      </div>
    </div>
  );
};

export const Gradient: React.FC<{ children: React.ReactNode; gradient?: string }> = ({ children, gradient = BRAND_GRADIENT }) => (
  <span style={{ ...gradientText, backgroundImage: gradient, paddingBottom: "0.08em" }}>{children}</span>
);

/** Number that counts up with an ease. */
export const Counter: React.FC<{ to: number; at: number; dur?: number; suffix?: string; style?: React.CSSProperties }> = ({
  to,
  at,
  dur = 40,
  suffix = "",
  style,
}) => {
  const frame = useCurrentFrame();
  const v = Math.round(to * prog(frame, at, at + dur, EASE.out));
  return <span style={{ fontVariantNumeric: "tabular-nums", ...style }}>{v.toLocaleString("en-US") + suffix}</span>;
};
