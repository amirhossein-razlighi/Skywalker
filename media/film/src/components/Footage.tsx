import React from "react";
import { AbsoluteFill, getStaticFiles, Img, interpolate, OffthreadVideo, staticFile, useCurrentFrame, useVideoConfig } from "remotion";
import { SLOT } from "../footage";
import { C, EASE, FONT } from "../theme";

/** Which asset a slot resolves to right now. */
export type Resolved = { kind: "video" | "still"; src: string } | { kind: "none" };

let cache: Set<string> | null = null;
const files = () => {
  if (!cache) cache = new Set(getStaticFiles().map((f) => f.name));
  return cache;
};

export const resolveSlot = (slot: string): Resolved => {
  const f = files();
  if (f.has(`footage/${slot}.mp4`)) return { kind: "video", src: staticFile(`footage/${slot}.mp4`) };
  if (f.has(`footage/${slot}.png`)) return { kind: "still", src: staticFile(`footage/${slot}.png`) };
  if (f.has(`footage/${slot}.jpg`)) return { kind: "still", src: staticFile(`footage/${slot}.jpg`) };
  return { kind: "none" };
};

export const hasSlot = (slot: string) => resolveSlot(slot).kind !== "none";

/** First slot in the chain that has media (so pending flagship shots borrow an existing one in drafts). */
export const pickSlot = (slot: string, fallback: string[] = []) => [slot, ...fallback].find(hasSlot) ?? slot;

/** Ken Burns move for stills: scale and drift in normalized units, over the sequence. */
export interface KenBurns {
  from?: [number, number, number]; // [x%, y%, scale]
  to?: [number, number, number];
}

export const Footage: React.FC<{
  slot: string;
  /** Total frames this shot is on screen (drives the Ken Burns timing). Defaults to the sequence length. */
  duration?: number;
  kb?: KenBurns;
  /** Start offset into a video clip, in frames. */
  offset?: number;
  style?: React.CSSProperties;
  /** Fit inside a smaller frame (e.g. a UI window). */
  fit?: "cover" | "contain";
  placeholderLabel?: boolean;
  /** Slots to use, in order, while this one has no media yet. */
  fallback?: string[];
}> = ({ slot: wanted, duration, kb, offset = 0, style, fit = "cover", placeholderLabel = true, fallback }) => {
  const slot = pickSlot(wanted, fallback);
  const frame = useCurrentFrame();
  const { durationInFrames } = useVideoConfig();
  const d = duration ?? durationInFrames;
  const r = resolveSlot(slot);
  const from = kb?.from ?? [0, 0, 1.04];
  const to = kb?.to ?? [-1.2, -0.6, 1.12];
  const t = interpolate(frame, [0, d], [0, 1], { extrapolateLeft: "clamp", extrapolateRight: "clamp", easing: EASE.soft });
  const x = interpolate(t, [0, 1], [from[0], to[0]]);
  const y = interpolate(t, [0, 1], [from[1], to[1]]);
  const s = interpolate(t, [0, 1], [from[2], to[2]]);

  if (r.kind === "video") {
    return (
      <AbsoluteFill style={{ background: C.void, ...style }}>
        <OffthreadVideo src={r.src} muted startFrom={offset} style={{ width: "100%", height: "100%", objectFit: fit }} />
      </AbsoluteFill>
    );
  }
  if (r.kind === "still") {
    return (
      <AbsoluteFill style={{ background: C.void, overflow: "hidden", ...style }}>
        <Img
          src={r.src}
          style={{
            width: "100%",
            height: "100%",
            objectFit: fit,
            transform: `translate(${x}%, ${y}%) scale(${s})`,
            willChange: "transform",
          }}
        />
      </AbsoluteFill>
    );
  }
  return <Placeholder slot={slot} label={placeholderLabel} style={style} />;
};

/** Elegant stand-in for footage that has not been rendered yet. */
export const Placeholder: React.FC<{ slot: string; label?: boolean; style?: React.CSSProperties }> = ({ slot, label = true, style }) => {
  const frame = useCurrentFrame();
  const info = SLOT[slot];
  const sweep = (frame * 0.6) % 160;
  return (
    <AbsoluteFill
      style={{
        background: `radial-gradient(120% 90% at 30% 20%, ${C.night} 0%, ${C.deep} 40%, ${C.ink} 100%)`,
        overflow: "hidden",
        ...style,
      }}
    >
      <AbsoluteFill
        style={{
          backgroundImage: `linear-gradient(${C.hair} 1px, transparent 1px), linear-gradient(90deg, ${C.hair} 1px, transparent 1px)`,
          backgroundSize: "80px 80px",
          opacity: 0.5,
          maskImage: "radial-gradient(70% 70% at 50% 50%, black, transparent)",
        }}
      />
      <AbsoluteFill
        style={{
          background: `linear-gradient(105deg, transparent ${sweep - 30}%, rgba(154,134,255,0.10) ${sweep - 10}%, transparent ${sweep + 10}%)`,
        }}
      />
      {label && (
        <div style={{ position: "absolute", left: 64, bottom: 56, right: 64, fontFamily: FONT.mono, color: C.faint }}>
          <div style={{ fontSize: 22, letterSpacing: 2, textTransform: "uppercase", color: C.dim }}>footage · {slot}</div>
          {info && <div style={{ fontSize: 20, marginTop: 10, maxWidth: 1100, lineHeight: 1.45 }}>{info.shot}</div>}
        </div>
      )}
    </AbsoluteFill>
  );
};
