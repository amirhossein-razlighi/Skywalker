import React from "react";
import { AbsoluteFill, interpolate, useCurrentFrame } from "remotion";
import { Backdrop } from "../components/Overlays";
import { Words } from "../components/Type";
import { C, EASE, FONT, hash, lerp, prog } from "../theme";
import { Cursor } from "../ui/kit";

/**
 * The problem (7 bars, 504 frames). A dense, grey, hands-on editor; a cursor doing chores on the beat;
 * then the cursor dissolves into a point of light and the new builder arrives.
 * Vocabulary: hard cuts on the beat, typographic.
 */

const W = 1920;
const H = 1080;

/** A generic, deliberately busy DCC-style editor (neutral greys: the old way). */
const LegacyEditor: React.FC<{ highlight?: number }> = ({ highlight = -1 }) => {
  const rows = Array.from({ length: 22 });
  const fields = Array.from({ length: 16 });
  const g = (a: number) => `rgba(200,204,214,${a})`;
  return (
    <div style={{ position: "absolute", inset: 0, background: "#1d1e22", fontFamily: FONT.body, color: g(0.6) }}>
      {/* menu bar */}
      <div style={{ height: 34, background: "#26272c", display: "flex", gap: 26, alignItems: "center", padding: "0 20px", fontSize: 15 }}>
        {["File", "Edit", "Assets", "GameObject", "Component", "Window", "Tools", "Build", "Help"].map((m, i) => (
          <span key={m} style={{ color: highlight === 100 + i ? "#fff" : g(0.65) }}>{m}</span>
        ))}
      </div>
      {/* toolbar */}
      <div style={{ height: 46, background: "#232428", display: "flex", gap: 8, alignItems: "center", padding: "0 14px", borderBottom: "1px solid #111" }}>
        {Array.from({ length: 28 }).map((_, i) => (
          <div key={i} style={{ width: 30, height: 30, borderRadius: 5, background: highlight === 200 + i ? "#4a4c55" : "#2d2e34", border: "1px solid #3a3b42" }} />
        ))}
      </div>
      {/* hierarchy */}
      <div style={{ position: "absolute", left: 0, top: 80, width: 330, bottom: 260, background: "#202125", borderRight: "1px solid #111", padding: 12 }}>
        <div style={{ fontSize: 13, letterSpacing: 1, color: g(0.45), marginBottom: 10 }}>HIERARCHY</div>
        {rows.map((_, i) => (
          <div key={i} style={{ height: 25, display: "flex", alignItems: "center", gap: 8, paddingLeft: 10 + (i % 4) * 14, background: highlight === i ? "#3b5b9a" : "transparent", borderRadius: 3 }}>
            <div style={{ width: 12, height: 12, background: g(0.25), borderRadius: 2 }} />
            <div style={{ height: 8, width: 70 + hash(i) * 120, background: g(0.22), borderRadius: 4 }} />
          </div>
        ))}
      </div>
      {/* inspector */}
      <div style={{ position: "absolute", right: 0, top: 80, width: 400, bottom: 0, background: "#202125", borderLeft: "1px solid #111", padding: 14 }}>
        <div style={{ fontSize: 13, letterSpacing: 1, color: g(0.45), marginBottom: 12 }}>INSPECTOR</div>
        {fields.map((_, i) => (
          <div key={i} style={{ display: "flex", alignItems: "center", gap: 10, height: 34 }}>
            <div style={{ width: 110, height: 8, background: g(0.2), borderRadius: 4 }} />
            {[0, 1, 2].map((k) => (
              <div key={k} style={{ flex: 1, height: 24, background: highlight === 300 + i * 3 + k ? "#3b5b9a" : "#2b2c32", border: "1px solid #3a3b42", borderRadius: 4 }} />
            ))}
          </div>
        ))}
        <div style={{ marginTop: 16, height: 10, background: "#2b2c32", borderRadius: 5, position: "relative" }}>
          <div style={{ position: "absolute", left: 0, top: 0, bottom: 0, width: "42%", background: "#59617a", borderRadius: 5 }} />
        </div>
      </div>
      {/* viewport */}
      <div style={{ position: "absolute", left: 331, right: 401, top: 80, bottom: 260, background: "linear-gradient(#3a3f4b, #2a2d34)", overflow: "hidden" }}>
        <svg width="100%" height="100%" viewBox="0 0 1189 740">
          {Array.from({ length: 24 }).map((_, i) => (
            <line key={i} x1={-400 + i * 80} y1={740} x2={594 + (i - 12) * 18} y2={360} stroke="rgba(255,255,255,0.08)" />
          ))}
          {Array.from({ length: 10 }).map((_, i) => (
            <line key={`h${i}`} x1={0} x2={1189} y1={380 + i * i * 4.2} y2={380 + i * i * 4.2} stroke="rgba(255,255,255,0.07)" />
          ))}
          <g transform="translate(594 430)">
            <rect x={-90} y={-90} width={180} height={180} fill="#6e7380" stroke="#9aa0ae" />
            <line x1={0} y1={0} x2={150} y2={0} stroke="#e05555" strokeWidth={5} />
            <line x1={0} y1={0} x2={0} y2={-150} stroke="#5bd16a" strokeWidth={5} />
            <line x1={0} y1={0} x2={-90} y2={80} stroke="#4f8dff" strokeWidth={5} />
          </g>
        </svg>
      </div>
      {/* project browser */}
      <div style={{ position: "absolute", left: 0, right: 401, bottom: 0, height: 259, background: "#202125", borderTop: "1px solid #111", padding: 14, display: "flex", flexWrap: "wrap", gap: 14 }}>
        {Array.from({ length: 18 }).map((_, i) => (
          <div key={i} style={{ width: 96, height: 96, background: "#2b2c32", borderRadius: 6, border: highlight === 400 + i ? "2px solid #4f8dff" : "1px solid #3a3b42" }} />
        ))}
      </div>
    </div>
  );
};

/** Cursor path: [frame, x, y, click?] */
const PATH: [number, number, number, boolean?][] = [
  [0, 1500, 700],
  [30, 140, 190, true],
  [54, 1660, 216, true],
  [78, 1700, 252, true],
  [100, 760, 480, false],
  [126, 900, 440, true],
  [150, 1000, 400, false],
];

const cursorAt = (f: number) => {
  let i = 0;
  while (i < PATH.length - 1 && PATH[i + 1][0] <= f) i++;
  const a = PATH[i];
  const b = PATH[Math.min(i + 1, PATH.length - 1)];
  const t = b[0] === a[0] ? 1 : EASE.inOut(Math.min(1, (f - a[0]) / (b[0] - a[0])));
  const click = b[3] ? interpolate(f, [b[0], b[0] + 12], [0, 1], { extrapolateLeft: "clamp", extrapolateRight: "clamp" }) : 0;
  return { x: lerp(a[1], b[1], t), y: lerp(a[2], b[2], t), click: f >= b[0] ? click : 0 };
};

const BEAT_WORDS = [
  { w: "Click.", at: 162, zoom: [0.3, 0.2] },
  { w: "Drag.", at: 180, zoom: [0.5, 0.45] },
  { w: "Tweak.", at: 198, zoom: [0.85, 0.35] },
  { w: "Repeat.", at: 216, zoom: [0.25, 0.85] },
];

export const S2Problem: React.FC = () => {
  const frame = useCurrentFrame();
  const intro = prog(frame, 0, 26);
  const c = cursorAt(Math.min(frame, 150));
  const highlight = frame < 40 ? -1 : frame < 60 ? 3 : frame < 84 ? 102 : frame < 128 ? 304 : 305;
  const beat = BEAT_WORDS.filter((b) => frame >= b.at).pop();
  const inBeats = frame >= 162 && frame < 234;
  // after the beats: everything recedes, cursor becomes a point of light
  const recede = prog(frame, 234, 300, EASE.inOut);
  const lightP = prog(frame, 250, 320, EASE.out);
  const dot = { x: lerp(1000, W / 2, lightP), y: lerp(400, H / 2 - 150, lightP) };
  const exit = prog(frame, 480, 504, EASE.in);

  return (
    <AbsoluteFill style={{ background: "#000" }}>
      {/* the old way */}
      {frame < 300 && (
        <AbsoluteFill
          style={{
            opacity: intro * (1 - recede * 0.85),
            transform: inBeats
              ? `scale(1.9) translate(${(0.5 - (beat?.zoom[0] ?? 0.5)) * 50}%, ${(0.5 - (beat?.zoom[1] ?? 0.5)) * 50}%)`
              : `scale(${lerp(1.04, 1, intro) - recede * 0.08})`,
            filter: inBeats ? "brightness(0.45) blur(2px)" : `blur(${recede * 14}px) saturate(${1 - recede})`,
          }}
        >
          <LegacyEditor highlight={highlight} />
          {!inBeats && frame < 240 && <Cursor x={c.x} y={c.y} click={c.click} opacity={1 - prog(frame, 220, 240)} />}
        </AbsoluteFill>
      )}
      {frame < 160 && (
        <AbsoluteFill style={{ justifyContent: "flex-start", alignItems: "center", paddingTop: 0 }}>
          <AbsoluteFill style={{ background: "linear-gradient(rgba(0,0,0,0.0), rgba(0,0,0,0.65) 45%, rgba(0,0,0,0.65) 55%, rgba(0,0,0,0))", opacity: prog(frame, 20, 50) * (1 - prog(frame, 146, 160)) }} />
          <AbsoluteFill style={{ alignItems: "center", justifyContent: "center" }}>
            <Words text={"Game engines were built\nfor hands on a mouse."} at={26} out={144} size={96} weight={600} />
          </AbsoluteFill>
        </AbsoluteFill>
      )}
      {inBeats && beat && (
        <AbsoluteFill style={{ alignItems: "center", justifyContent: "center" }}>
          <div style={{ fontFamily: FONT.display, fontWeight: 700, fontSize: 200, letterSpacing: -8, color: C.text, transform: `scale(${1 + 0.04 * prog(frame, beat.at, beat.at + 18)})` }}>{beat.w}</div>
        </AbsoluteFill>
      )}
      {/* the new builder */}
      {frame >= 234 && (
        <Backdrop glow={recede} y={60}>
          <AbsoluteFill style={{ opacity: 1 - exit, filter: exit ? `blur(${exit * 10}px)` : undefined }}>
            <div
              style={{
                position: "absolute",
                left: dot.x - 9,
                top: dot.y - 9,
                width: 18,
                height: 18,
                borderRadius: 9,
                background: "#fff",
                boxShadow: `0 0 ${30 + 40 * lightP}px ${10 + 20 * lightP}px rgba(154,134,255,${0.6 * lightP}), 0 0 ${120 * lightP}px rgba(79,141,255,0.6)`,
                opacity: lightP * (1 - prog(frame, 340, 372)),
                transform: `scale(${1 + 0.15 * Math.sin(frame / 6)})`,
              }}
            />
            <ToolStream from={330} />
            <AbsoluteFill style={{ alignItems: "center", justifyContent: "center" }}>
              <Words text="But the builders have changed." at={252} out={350} size={92} weight={600} />
            </AbsoluteFill>
            <AbsoluteFill style={{ alignItems: "center", justifyContent: "center" }}>
              <Words text={"Agents don't click.\nThey *call* *tools.*"} at={368} size={118} weight={700} />
            </AbsoluteFill>
          </AbsoluteFill>
        </Backdrop>
      )}
    </AbsoluteFill>
  );
};

/** Faint tool calls streaming upward behind the headline. */
const CALLS = [
  'scene_overview {"max_entities": 200}',
  'entity_create {"name": "Lantern", "prefab": "lantern"}',
  'viewport_capture {"annotate": true}',
  'terrain_create {"preset": "mountain_valley"}',
  'foliage_add {"entity": "Valley", "density": 0.006}',
  'behavior_set {"entity": "#41", "intent": "Flicker when the player is near"}',
  'sim_control {"action": "step", "ticks": 600}',
  'environment_update {"sunElevation": 7}',
  'fx_create {"preset": "volume_fire"}',
  'studio_decide {"feedback": "F-7", "verdict": "act"}',
  'history {"limit": 20}',
  'wander_test {"entity": "Coin"}',
];

const ToolStream: React.FC<{ from: number }> = ({ from }) => {
  const frame = useCurrentFrame();
  if (frame < from) return null;
  const t = frame - from;
  const a = prog(frame, from, from + 30);
  return (
    <AbsoluteFill style={{ opacity: 0.42 * a, maskImage: "linear-gradient(transparent, black 25%, black 75%, transparent)" }}>
      {Array.from({ length: 3 }).map((_, col) => (
        <div key={col} style={{ position: "absolute", left: 120 + col * 600, top: 1080 - ((t * (1.6 + col * 0.35)) % 1500), fontFamily: FONT.mono, fontSize: 20, lineHeight: 2.4, color: col === 1 ? C.violet : C.sky }}>
          {CALLS.concat(CALLS).map((l, i) => (
            <div key={i} style={{ whiteSpace: "nowrap", opacity: 0.5 + 0.5 * hash(i + col * 7) }}>
              {l}
            </div>
          ))}
        </div>
      ))}
    </AbsoluteFill>
  );
};
