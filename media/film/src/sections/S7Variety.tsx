import React from "react";
import { AbsoluteFill, Sequence, useCurrentFrame, useVideoConfig } from "remotion";
import { Footage, pickSlot, resolveSlot } from "../components/Footage";
import { Kicker, Words } from "../components/Type";
import { C, EASE, FONT, SPRING, hash, lerp, prog, sp } from "../theme";

/**
 * Variety (9 bars, 648 frames): a mosaic of every sample game (the flagships move, the older samples are
 * stills); the strategy map tile grows to full frame and keeps zooming down to the front line; then the
 * political drama's decree is signed; then Gloamwater, the same engine in 2D, with its own dialogue box.
 * Every HUD, map label, tooltip, document and dialogue box on screen here is the engine's UI, not an overlay.
 */

const TILES: { slot: string; label: string; fallback?: string[] }[] = [
  { slot: "rs_neon_avenue", label: "Neo-noir RPG" },
  { slot: "var_gloam_run", label: "Metroidvania" },
  { slot: "rs_isle_aerial", label: "Island adventure" },
  { slot: "var_drama", label: "Political drama" },
  { slot: "var_kart", label: "Kart racing" },
  { slot: "var_strategy", label: "Grand strategy" },
  { slot: "rs_peaks_forest", label: "Mythic action" },
  { slot: "var_farm", label: "Farming life sim" },
  { slot: "var_shmup", label: "Space shooter" },
  { slot: "var_horror", label: "Horror" },
  { slot: "var_zen", label: "Meditative" },
  { slot: "var_arena", label: "Arena shooter" },
  { slot: "var_frost", label: "Cozy exploration" },
  { slot: "var_park", label: "Park builder" },
  { slot: "rs_abyss", label: "Underwater horror" },
  { slot: "rs_canyon", label: "Survival" },
  { slot: "var_village", label: "Sky village" },
  { slot: "var_cozy", label: "Cozy life sim" },
];

const FOCUS = "var_strategy";
/** The map tile holds on the whole continent, then starts its zoom while the grid is still on screen. */
const FOCUS_PLAY = 100;
/** The map fills the frame on beat 11; from here the full-frame Strategy shot carries the same clip on. */
const MAP_CUT = 198;
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
  // the camera pulls back from a close-up of the grid, then the farm tile grows to fill the frame
  const pull = prog(frame, 0, 110, EASE.out);
  const s0 = lerp(1.45, 0.86, pull);
  const focusIdx = tiles.findIndex((t) => t.slot === FOCUS);
  const fly = prog(frame, 162, 196, EASE.inOut);
  // where the focus tile sits on screen (grid space -> screen space), so it can grow from there
  const fcol = focusIdx % COLS;
  const frow = Math.floor(focusIdx / COLS);
  const gx = 960 + (fcol * (TW + GAP) - (1920 - 192) / 2) * s0;
  const gy = 540 + lerp(0, 60, pull) * s0 + (frow * (TH + GAP) - gridH / 2) * s0;
  const fl = lerp(gx, 0, fly);
  const ft = lerp(gy, 0, fly);
  const fw = lerp(TW * s0, 1920, fly);
  const fh = lerp(TH * s0, 1080, fly);
  return (
    <AbsoluteFill style={{ background: C.void }}>
      <AbsoluteFill style={{ alignItems: "center", justifyContent: "center", transform: `scale(${s0}) translateY(${lerp(0, 60, pull)}px)` }}>
        <div style={{ position: "relative", width: 1920 - 192, height: gridH }}>
          {tiles.map((t, i) => {
            const col = i % COLS;
            const row = Math.floor(i / COLS);
            const s = sp(frame, fps, 4 + (col + row) * 3 + hash(i) * 6, SPRING.snappy);
            const isFocus = i === focusIdx;
            const lbl = prog(frame, 50 + i * 2, 64 + i * 2);
            return (
              <div
                key={t.slot}
                style={{
                  position: "absolute",
                  left: col * (TW + GAP),
                  top: row * (TH + GAP),
                  width: TW,
                  height: TH,
                  borderRadius: 14,
                  overflow: "hidden",
                  opacity: s * (1 - fly) * (isFocus && fly > 0 ? 0 : 1),
                  transform: `scale(${0.8 + 0.2 * s})`,
                  boxShadow: "0 18px 50px rgba(0,0,0,0.5)",
                }}
              >
                <Footage slot={t.slot} fallback={t.fallback} duration={220} offset={isFocus ? 0 : 8} playAt={isFocus ? FOCUS_PLAY : 0} kb={{ from: [0, 0, 1.08], to: [hash(i) * 4 - 2, 0, 1.0] }} placeholderLabel={false} />
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
                    opacity: lbl,
                  }}
                >
                  {t.label}
                </div>
              </div>
            );
          })}
        </div>
      </AbsoluteFill>
      <AbsoluteFill style={{ background: "radial-gradient(55% 45% at 50% 50%, rgba(4,5,13,0.82), rgba(4,5,13,0.2) 80%, transparent)", opacity: prog(frame, 70, 90) * (1 - prog(frame, 146, 162)) }} />
      <AbsoluteFill style={{ alignItems: "center", justifyContent: "center", opacity: 1 - prog(frame, 146, 160) }}>
        <Words text={"One engine.\nEvery kind of game."} at={74} size={120} weight={700} />
      </AbsoluteFill>
      {/* the map tile grows out of the grid to full frame (the same clip keeps playing) */}
      {fly > 0 && (
        <div style={{ position: "absolute", left: fl, top: ft, width: fw, height: fh, borderRadius: lerp(14 * s0, 0, fly), overflow: "hidden" }}>
          <Footage slot={FOCUS} playAt={FOCUS_PLAY} placeholderLabel={false} />
        </div>
      )}
    </AbsoluteFill>
  );
};

/** Lower-left caption on a soft scrim, clear of the games' own HUDs. */
const Caption: React.FC<{ kicker: string; title: string; color: string; at: number; out: number; top?: number }> = ({ kicker, title, color, at, out, top }) => {
  const frame = useCurrentFrame();
  const o = 1 - prog(frame, out, out + 12, EASE.in);
  return (
    <>
      <AbsoluteFill style={{ background: top === undefined ? "linear-gradient(0deg, rgba(0,0,0,0.6) 0%, rgba(0,0,0,0) 36%)" : "linear-gradient(90deg, rgba(0,0,0,0.55) 0%, rgba(0,0,0,0) 42%)", opacity: o }} />
      <div style={{ position: "absolute", left: 96, ...(top === undefined ? { bottom: 92 } : { top }), opacity: o }}>
        <Kicker text={kicker} at={at} color={color} />
        <div style={{ height: 12 }} />
        <Words text={title} at={at + 4} size={64} weight={700} align="left" style={{ textShadow: "0 3px 30px rgba(0,0,0,0.6)" }} />
      </div>
    </>
  );
};

/** Meridian Accord full frame: the zoom continues from the tile down to the river crossing (labels, tooltip, HUD). */
const Strategy: React.FC = () => (
  <AbsoluteFill>
    <Footage slot={FOCUS} offset={MAP_CUT - FOCUS_PLAY} placeholderLabel={false} />
    <Caption kicker="Meridian Accord · grand strategy" title={"From the continent\nto the front line."} color={C.sunset} at={8} out={112} />
  </AbsoluteFill>
);

/** The Chancellor's Desk: Decree No. 14 slides in and the cursor signs it. */
const Decree: React.FC = () => {
  const frame = useCurrentFrame();
  const p = prog(frame, 0, 16, EASE.out);
  return (
    <AbsoluteFill style={{ background: C.void }}>
      <AbsoluteFill style={{ transform: `scale(${lerp(1.06, 1, p)})` }}>
        <Footage slot="var_decree" offset={4} placeholderLabel={false} />
      </AbsoluteFill>
      <Caption kicker="The Chancellor's Desk · political drama" title={"Documents, choices,\nconsequences."} color={C.gold} at={10} out={130} top={430} />
    </AbsoluteFill>
  );
};

/** Gloamwater: the same engine in 2D. Wick runs the grove, then meets Bellwether; the dialogue box is the engine's UI. */
const Gloam: React.FC = () => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();
  const inn = sp(frame, fps, 0, SPRING.gentle);
  const RUN = 62;
  const capOut = prog(frame, RUN + 40, RUN + 54, EASE.in);
  return (
    <AbsoluteFill style={{ background: C.void }}>
      <AbsoluteFill style={{ clipPath: `inset(0 0 0 ${(1 - inn) * 100}%)` }}>
        <Sequence durationInFrames={RUN}>
          <Footage slot="var_gloam_run" offset={20} placeholderLabel={false} />
        </Sequence>
        <Sequence from={RUN}>
          <Footage slot="var_gloam_talk" offset={4} placeholderLabel={false} />
        </Sequence>
      </AbsoluteFill>
      <AbsoluteFill style={{ alignItems: "center", justifyContent: "center", opacity: 1 - capOut }}>
        <div style={{ marginTop: -40, textAlign: "center" }}>
          <Kicker text="Gloamwater · 2D metroidvania" at={14} color="#7ff4e4" />
          <div style={{ height: 14 }} />
          <Words text="Same engine. Two dimensions." at={18} size={78} weight={700} style={{ textShadow: "0 3px 40px rgba(0,0,0,0.7)" }} />
        </div>
      </AbsoluteFill>
    </AbsoluteFill>
  );
};

export const S7Variety: React.FC = () => (
  <AbsoluteFill style={{ background: C.void }}>
    <Sequence durationInFrames={MAP_CUT}>
      <Mosaic />
    </Sequence>
    <Sequence from={MAP_CUT} durationInFrames={324 - MAP_CUT}>
      <Strategy />
    </Sequence>
    <Sequence from={324} durationInFrames={144}>
      <Decree />
    </Sequence>
    <Sequence from={468} durationInFrames={180}>
      <Gloam />
    </Sequence>
  </AbsoluteFill>
);
