import React from "react";
import { AbsoluteFill, interpolate, Sequence, useCurrentFrame, useVideoConfig } from "remotion";
import { Footage, pickSlot, resolveSlot } from "../components/Footage";
import { Kicker, Words } from "../components/Type";
import { C, EASE, FONT, SPRING, hash, lerp, prog, sp } from "../theme";
import { typed } from "../ui/kit";

/**
 * Variety (7 bars, 504 frames): a mosaic of every sample game; then UI + dialogue on the noir street;
 * then the 2D platformer. Vocabulary: the grid (tiles springing in, one tile scaling up to fill the frame).
 */

const TILES: { slot: string; label: string; fallback?: string[] }[] = [
  { slot: "var_platformer2d", label: "2D platformer" },
  { slot: "var_park", label: "Park builder" },
  { slot: "var_noir", label: "Neo-noir adventure" },
  { slot: "var_kart", label: "Kart racing" },
  { slot: "var_shmup", label: "Space shooter" },
  { slot: "var_zen", label: "Meditative" },
  { slot: "var_horror", label: "Horror" },
  { slot: "var_cozy", label: "Cozy life sim" },
  { slot: "var_strategy", label: "Strategy" },
  { slot: "var_arena", label: "Arena shooter" },
  { slot: "var_platformer3d", label: "3D platformer" },
  { slot: "var_frost", label: "Cozy exploration" },
  { slot: "var_farm", label: "Farm sim" },
  { slot: "var_village", label: "Sky village" },
  { slot: "rs_abyss", label: "Underwater horror" },
  { slot: "rs_canyon", label: "Open-world survival" },
  { slot: "rs_ocean", label: "Pirate adventure" },
  { slot: "rs_rain", label: "Stealth" },
  { slot: "rs_clouds", label: "Open world" },
];

const COLS = 4;
const GAP = 14;
const TW = (1920 - 2 * 96 - (COLS - 1) * GAP) / COLS; // 421
const TH = TW * (9 / 16);

const Mosaic: React.FC = () => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();
  const avail = TILES.filter((t) => resolveSlot(pickSlot(t.slot, t.fallback)).kind !== "none");
  const tiles = avail.slice(0, Math.min(16, avail.length - (avail.length % COLS)));
  const rows = Math.ceil(tiles.length / COLS);
  const gridH = rows * TH + (rows - 1) * GAP;
  // the camera pulls back from a close-up of the grid, then the noir tile flies to full frame
  const pull = prog(frame, 0, 110, EASE.out);
  const s0 = lerp(1.45, 0.86, pull);
  const focusIdx = tiles.findIndex((t) => t.slot === "var_noir");
  const fly = prog(frame, 168, 200, EASE.inOut);
  return (
    <AbsoluteFill style={{ background: C.void }}>
      <AbsoluteFill style={{ alignItems: "center", justifyContent: "center", transform: `scale(${s0}) translateY(${lerp(0, 60, pull)}px)` }}>
        <div style={{ position: "relative", width: 1920 - 192, height: gridH }}>
          {tiles.map((t, i) => {
            const col = i % COLS;
            const row = Math.floor(i / COLS);
            const s = sp(frame, fps, 4 + (col + row) * 3 + hash(i) * 6, SPRING.snappy);
            const isFocus = i === focusIdx;
            const x = col * (TW + GAP);
            const y = row * (TH + GAP);
            const lbl = prog(frame, 50 + i * 2, 64 + i * 2);
            return (
              <div
                key={t.slot}
                style={{
                  position: "absolute",
                  left: x,
                  top: y,
                  width: TW,
                  height: TH,
                  borderRadius: 14,
                  overflow: "hidden",
                  opacity: s * (isFocus ? 1 : 1 - fly),
                  transform: `scale(${0.8 + 0.2 * s})`,
                  boxShadow: "0 18px 50px rgba(0,0,0,0.5)",
                }}
              >
                <Footage slot={t.slot} fallback={t.fallback} duration={220} kb={{ from: [0, 0, 1.08], to: [hash(i) * 4 - 2, 0, 1.0] }} placeholderLabel={false} />
                <div
                  style={{
                    position: "absolute",
                    left: 14,
                    bottom: 12,
                    padding: "6px 12px",
                    borderRadius: 8,
                    background: "rgba(8,10,20,0.62)",
                    backdropFilter: "blur(8px)",
                    fontFamily: FONT.body,
                    fontWeight: 600,
                    fontSize: 17,
                    color: "#fff",
                    opacity: lbl * (1 - fly),
                  }}
                >
                  {t.label}
                </div>
              </div>
            );
          })}
        </div>
      </AbsoluteFill>
      <AbsoluteFill style={{ background: "radial-gradient(55% 45% at 50% 50%, rgba(4,5,13,0.82), rgba(4,5,13,0.2) 80%, transparent)", opacity: prog(frame, 70, 90) * (1 - prog(frame, 150, 168)) }} />
      <AbsoluteFill style={{ alignItems: "center", justifyContent: "center", opacity: 1 - prog(frame, 150, 166) }}>
        <Words text={"One engine.\nEvery kind of game."} at={74} size={120} weight={700} />
      </AbsoluteFill>
      {/* the focus tile grows to full frame */}
      {fly > 0 && (
        <AbsoluteFill style={{ opacity: fly }}>
          <Footage slot="var_noir" duration={300} kb={{ from: [0, 0, 1.0], to: [0, -1, 1.06] }} />
        </AbsoluteFill>
      )}
    </AbsoluteFill>
  );
};

const LINES = [
  "Mara, the rain keeps no secrets. Neither should you.",
];

/** UI + dialogue recreated over the noir street (original characters and lines). */
const Dialogue: React.FC = () => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();
  const box = sp(frame, fps, 6, SPRING.snappy);
  const line = typed(frame, 20, LINES[0], 34);
  const choices = ["Tell him about the ledger.", "Say nothing.", "Ask who sent him."];
  const pick = frame > 118 ? 0 : -1;
  const exit = prog(frame, 176, 190, EASE.in);
  return (
    <AbsoluteFill>
      <Footage slot="var_noir" duration={200} kb={{ from: [0, -1, 1.06], to: [0, -1.6, 1.1] }} />
      <AbsoluteFill style={{ background: "linear-gradient(0deg, rgba(0,0,0,0.75), rgba(0,0,0,0) 55%)" }} />
      <div style={{ position: "absolute", left: 96, top: 92, opacity: 1 - exit }}>
        <Kicker text="2D · UI · dialogue" at={4} color={C.sunset} />
        <Words text="UI and dialogue, built in." at={8} size={62} weight={700} align="left" />
      </div>
      {/* HUD */}
      <div style={{ position: "absolute", right: 96, top: 96, display: "flex", gap: 14, opacity: prog(frame, 10, 24) * (1 - exit) }}>
        {["Case 3 · The Ledger", "23:41"].map((t) => (
          <div key={t} style={{ padding: "10px 18px", borderRadius: 10, background: "rgba(10,12,26,0.6)", border: "1px solid rgba(255,90,170,0.4)", fontFamily: FONT.mono, fontSize: 20, color: "#ffd6ef" }}>{t}</div>
        ))}
      </div>
      <div
        style={{
          position: "absolute",
          left: 260,
          right: 260,
          bottom: 90,
          opacity: interpolate(box, [0, 0.5], [0, 1], { extrapolateRight: "clamp" }) * (1 - exit),
          transform: `translateY(${(1 - box) * 40}px)`,
        }}
      >
        <div style={{ display: "flex", gap: 26, padding: "26px 30px", borderRadius: 20, background: "rgba(12,14,28,0.82)", backdropFilter: "blur(14px)", border: "1px solid rgba(120,200,255,0.25)" }}>
          <div style={{ width: 120, height: 120, borderRadius: 16, background: "linear-gradient(160deg, #2b3a6b, #6b2b5a)", flexShrink: 0, display: "flex", alignItems: "flex-end", justifyContent: "center", overflow: "hidden" }}>
            <svg width={100} height={100} viewBox="0 0 100 100">
              <circle cx={50} cy={40} r={20} fill="#151826" />
              <path d="M14 100 Q50 55 86 100 Z" fill="#151826" />
              <path d="M26 30 L74 30 L66 22 L34 22 Z" fill="#0b0d18" />
              <rect x={20} y={28} width={60} height={5} rx={2} fill="#0b0d18" />
            </svg>
          </div>
          <div style={{ flex: 1 }}>
            <div style={{ fontFamily: FONT.display, fontWeight: 600, fontSize: 24, color: "#7fd4ff", letterSpacing: 0.5 }}>DETECTIVE ROOK</div>
            <div style={{ fontFamily: FONT.body, fontSize: 32, color: "#fff", marginTop: 8, lineHeight: 1.35, minHeight: 44 }}>{line}</div>
            <div style={{ marginTop: 16, display: "flex", gap: 12 }}>
              {choices.map((c, i) => {
                const p = prog(frame, 84 + i * 6, 96 + i * 6);
                const sel = pick === i;
                return (
                  <div
                    key={c}
                    style={{
                      padding: "10px 16px",
                      borderRadius: 10,
                      fontFamily: FONT.body,
                      fontWeight: 500,
                      fontSize: 21,
                      color: sel ? "#0b0d18" : "#e8ecff",
                      background: sel ? "#7fd4ff" : "rgba(255,255,255,0.07)",
                      border: "1px solid rgba(255,255,255,0.12)",
                      opacity: p,
                      transform: `translateY(${(1 - p) * 10}px) scale(${sel ? 1.04 : 1})`,
                    }}
                  >
                    {i + 1}. {c}
                  </div>
                );
              })}
            </div>
          </div>
        </div>
      </div>
    </AbsoluteFill>
  );
};

/** The 2D platformer with a HUD; the frame splits to show it is the same engine. */
const Platformer: React.FC = () => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();
  const inn = sp(frame, fps, 0, SPRING.gentle);
  const coins = Math.min(12, Math.floor(Math.max(0, frame - 20) / 9));
  const exit = prog(frame, 100, 114, EASE.in);
  return (
    <AbsoluteFill style={{ background: C.void }}>
      <AbsoluteFill style={{ clipPath: `inset(0 0 0 ${(1 - inn) * 100}%)` }}>
        <Footage slot="var_platformer2d" duration={120} kb={{ from: [2, 0, 1.06], to: [-2, 0, 1.06] }} />
        <div style={{ position: "absolute", left: 60, top: 50, display: "flex", gap: 22, alignItems: "center", fontFamily: FONT.display, fontWeight: 700, fontSize: 40, color: "#fff", textShadow: "0 3px 0 rgba(0,0,0,0.25)" }}>
          <span style={{ color: "#ffd75e" }}>● {String(coins).padStart(2, "0")}</span>
          <span style={{ color: "#ff6b8a" }}>♥♥♥</span>
        </div>
      </AbsoluteFill>
      <AbsoluteFill style={{ alignItems: "center", justifyContent: "flex-end", paddingBottom: 110, opacity: 1 - exit }}>
        <div style={{ padding: "18px 34px", borderRadius: 20, background: "rgba(8,10,24,0.55)", backdropFilter: "blur(10px)" }}>
          <Words text="Same engine. Two dimensions." at={16} size={64} weight={700} />
        </div>
      </AbsoluteFill>
    </AbsoluteFill>
  );
};

export const S7Variety: React.FC = () => (
  <AbsoluteFill style={{ background: C.void }}>
    <Sequence durationInFrames={204}>
      <Mosaic />
    </Sequence>
    <Sequence from={200} durationInFrames={190}>
      <Dialogue />
    </Sequence>
    <Sequence from={388} durationInFrames={116}>
      <Platformer />
    </Sequence>
  </AbsoluteFill>
);
