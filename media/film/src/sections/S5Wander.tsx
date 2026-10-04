import React from "react";
import { AbsoluteFill, useCurrentFrame, useVideoConfig } from "remotion";
import { Footage } from "../components/Footage";
import { Backdrop } from "../components/Overlays";
import { Kicker, Words } from "../components/Type";
import { C, EASE, FONT, SPRING, lerp, prog, sp } from "../theme";
import { Caret, Chip, CodeLine, typed, Window } from "../ui/kit";

/**
 * Words -> Wander -> native (9 bars, 648 frames).
 * Vocabulary: one continuous object that morphs (prompt -> code -> graph -> bytecode/C++), no hard cuts.
 * Code and bytecode are real: the docs' Coin behavior, its `wander_check disassemble` output,
 * and the `native_template` module the engine writes.
 */

const PROMPT = "Make the coins spin. When the player grabs one, add a point and make it vanish.";

const CODE = [
  "behavior Coin",
  '  intent "Spins; when the player touches it, it adds 1 to the score and disappears."',
  '  param spin = 90 in 0..360 "degrees per second"',
  "  on tick",
  "    rotate self by (0, spin * dt, 0)",
  "  end",
  '  on trigger_enter "player"',
  '    emit "score" with {points: 1}',
  "    destroy self",
  "  end",
  "end",
];

const BYTECODE = [
  "proto 1 Coin.on tick (params 0, regs 7)",
  "  0  L5  LOADSELF  0 0 0",
  "  1  L5  LOADK     R2 K1(0)",
  "  2  L5  GETVAR    5 0 0",
  "  3  L5  LOADENV   6 0 0",
  "  4  L5  MUL       R3 R5 R6",
  "  5  L5  LOADK     R4 K1(0)",
  "  6  L5  MAKEVEC   1 2 3",
  "  7  L5  CALL      R0 __rotate/2",
  "  8  L4  STOP      0 0 0",
];

const NATIVE = [
  "// A per-tick system: spins every entity tagged \"spinner\"",
  "void spinners(SkyWorld* w, double dt, void*) {",
  "    SkyEntity ids[256];",
  "    size_t n = sky_sdk_api->find_tagged(w, \"spinner\", ids, 256);",
  "    for (size_t i = 0; i < n && i < 256; ++i) {",
  "        float r[3];",
  "        if (sky_sdk_api->get_rotation(w, ids[i], r) == 0) {",
  "            r[1] = std::fmod(r[1] + static_cast<float>(90.0 * dt), 360.f);",
  "            sky_sdk_api->set_rotation(w, ids[i], r);",
  "        }",
  "    }",
  "}",
];

const LINE_H = 40;

/** Graph layout (from behavior_graph's node list, re-spaced for the frame). */
type GNode = { id: string; title: string; kind: "event" | "exec" | "data"; x: number; y: number; fromLine: number; pins?: string[] };
const NODES: GNode[] = [
  { id: "tick", title: "on tick", kind: "event", x: 40, y: 40, fromLine: 3 },
  { id: "mul", title: "spin × dt", kind: "data", x: 40, y: 200, fromLine: 4, pins: ["spin", "dt"] },
  { id: "vec", title: "vector (0, y, 0)", kind: "data", x: 300, y: 200, fromLine: 4 },
  { id: "rotate", title: "rotate self", kind: "exec", x: 560, y: 40, fromLine: 4, pins: ["degrees"] },
  { id: "enter", title: 'on trigger_enter "player"', kind: "event", x: 40, y: 380, fromLine: 6 },
  { id: "map", title: "{points: 1}", kind: "data", x: 300, y: 530, fromLine: 7 },
  { id: "emit", title: 'emit "score"', kind: "exec", x: 560, y: 380, fromLine: 7, pins: ["payload"] },
  { id: "destroy", title: "destroy self", kind: "exec", x: 860, y: 380, fromLine: 8 },
];
const NODE_W = 230;
const NODE_H = 96;
const LINKS: [string, string, "exec" | "data"][] = [
  ["tick", "rotate", "exec"],
  ["mul", "vec", "data"],
  ["vec", "rotate", "data"],
  ["enter", "emit", "exec"],
  ["map", "emit", "data"],
  ["emit", "destroy", "exec"],
];

const PANEL_X = 640;
const PANEL_Y = 250;

export const S5Wander: React.FC = () => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();

  // Phase timing (local frames)
  const P_PROMPT = 0;
  const P_CODE = 150;
  const P_GRAPH = 330;
  const P_NATIVE = 470;
  const END = 648;

  const typedPrompt = typed(frame, P_PROMPT + 24, PROMPT, 44);
  const promptDone = P_PROMPT + 24 + (PROMPT.length / 44) * fps;
  const weave = sp(frame, fps, promptDone + 4, SPRING.pop);
  const toCode = prog(frame, P_CODE - 10, P_CODE + 20, EASE.inOut);
  const toGraph = prog(frame, P_GRAPH, P_GRAPH + 34, EASE.inOut);
  const toNative = prog(frame, P_NATIVE, P_NATIVE + 30, EASE.inOut);
  const exit = prog(frame, END - 20, END, EASE.in);

  const headline =
    frame < P_CODE ? { k: "Natural language", t: "Say what you want.", at: 8, out: P_CODE - 14 }
    : frame < P_GRAPH ? { k: "Wander", t: "Get code you can read.", at: P_CODE + 6, out: P_GRAPH - 14 }
    : frame < P_NATIVE ? { k: "Graph view", t: "Or see it as a graph.", at: P_GRAPH + 6, out: P_NATIVE - 14 }
    : { k: "Native speed", t: "Then run it at native speed.", at: P_NATIVE + 6, out: END - 24 };

  return (
    <AbsoluteFill style={{ background: C.void }}>
      {/* the world the behavior drives, far behind */}
      <AbsoluteFill style={{ opacity: 0.22 * (1 - exit), filter: "blur(18px) saturate(1.2)" }}>
        <Footage slot="wander_coins" kb={{ from: [0, 0, 1.12], to: [-2, 0, 1.2] }} placeholderLabel={false} />
      </AbsoluteFill>
      <Backdrop glow={0.6} y={0}>
        <AbsoluteFill style={{ opacity: 1 - exit }}>
          <div style={{ position: "absolute", left: 96, top: 92 }} key={headline.k}>
            <Kicker text={headline.k} at={headline.at - 4} out={headline.out} color={C.violet} />
            <Words text={headline.t} at={headline.at} out={headline.out} size={68} weight={700} align="left" />
          </div>

          {/* 1. the prompt box */}
          {toCode < 1 && (
            <div
              style={{
                position: "absolute",
                left: lerp(260, PANEL_X, toCode),
                top: lerp(470, PANEL_Y, toCode),
                width: lerp(1400, 1180, toCode),
                opacity: 1 - toCode,
                transform: `scale(${lerp(1, 0.96, toCode)})`,
              }}
            >
              <div
                style={{
                  padding: "30px 36px",
                  borderRadius: 22,
                  background: "rgba(24,27,44,0.92)",
                  border: "1px solid rgba(154,134,255,0.35)",
                  boxShadow: "0 30px 90px rgba(0,0,0,0.5), 0 0 60px rgba(154,134,255,0.12)",
                  fontFamily: FONT.body,
                  fontSize: 38,
                  lineHeight: 1.35,
                  color: C.text,
                  minHeight: 120,
                }}
              >
                {typedPrompt}
                {frame < promptDone + 6 && <Caret h={38} color={C.violet} />}
              </div>
              <div style={{ display: "flex", justifyContent: "flex-end", marginTop: 18, gap: 12 }}>
                <div
                  style={{
                    padding: "12px 26px",
                    borderRadius: 12,
                    fontFamily: FONT.body,
                    fontWeight: 600,
                    fontSize: 24,
                    color: "#fff",
                    background: `linear-gradient(100deg, ${C.sky}, ${C.violet})`,
                    transform: `scale(${0.9 + 0.1 * weave + (frame > promptDone + 18 && frame < promptDone + 26 ? -0.04 : 0)})`,
                    opacity: weave,
                    boxShadow: `0 0 ${30 * weave}px rgba(154,134,255,0.6)`,
                  }}
                >
                  ✦ Weave
                </div>
              </div>
            </div>
          )}

          {/* 2. Wander code (stays during the graph phase as a fading ghost) */}
          {frame >= P_CODE - 10 && toNative < 1 && (
            <div style={{ position: "absolute", left: PANEL_X, top: PANEL_Y, opacity: toCode * (1 - toGraph) }}>
              <Window title="Coin.wander" width={1180} height={560} tint="#12141d">
                <div style={{ padding: "26px 30px", fontFamily: FONT.mono, fontSize: 24 }}>
                  {CODE.map((line, i) => {
                    const at = P_CODE + 4 + i * 5;
                    const chars = Math.max(0, Math.floor((frame - at) * 6));
                    const isIntent = i === 1;
                    return (
                      <div key={i} style={{ height: LINE_H, display: "flex", alignItems: "center", whiteSpace: "pre", position: "relative" }}>
                        <span style={{ width: 44, color: "rgba(200,205,230,0.25)", fontSize: 18 }}>{i + 1}</span>
                        {isIntent && (
                          <div
                            style={{
                              position: "absolute",
                              left: 40,
                              right: -10,
                              top: 2,
                              bottom: 2,
                              borderRadius: 6,
                              background: `rgba(154,134,255,${0.16 * prog(frame, P_CODE + 40, P_CODE + 60)})`,
                            }}
                          />
                        )}
                        <span style={{ position: "relative", fontSize: isIntent ? 21 : 24 }}>
                          <CodeLine line={line} lang="wander" chars={chars} />
                        </span>
                      </div>
                    );
                  })}
                </div>
              </Window>
            </div>
          )}
          {frame >= P_CODE + 70 && frame < P_GRAPH + 10 && (
            <div style={{ position: "absolute", left: 96, top: 330, display: "flex", flexDirection: "column", gap: 14, opacity: 1 - prog(frame, P_GRAPH - 10, P_GRAPH + 10) }}>
              {[
                { t: "intent", c: C.violet, at: P_CODE + 70 },
                { t: "✓ wander_test", c: C.green, at: P_CODE + 82 },
                { t: "deterministic", c: C.sky, at: P_CODE + 94 },
                { t: "always terminates", c: C.sky, at: P_CODE + 106 },
              ].map((x) => (
                <div key={x.t} style={{ opacity: prog(frame, x.at, x.at + 12), transform: `translateX(${(1 - prog(frame, x.at, x.at + 16)) * -16}px)` }}>
                  <Chip color={x.c} mono>{x.t}</Chip>
                </div>
              ))}
            </div>
          )}

          {/* 3. Graph: nodes fly out of their source lines */}
          {toGraph > 0 && toNative < 1 && <Graph t={toGraph} frame={frame - P_GRAPH} fade={1 - toNative} />}

          {/* 4. bytecode + native C++ */}
          {toNative > 0 && <Native t={toNative} frame={frame - P_NATIVE} />}
        </AbsoluteFill>
      </Backdrop>
    </AbsoluteFill>
  );
};

const Graph: React.FC<{ t: number; frame: number; fade: number }> = ({ t, frame, fade }) => {
  const ox = PANEL_X + 40;
  const oy = PANEL_Y + 10;
  const pos = (n: GNode) => {
    // start: on the code line it came from
    const sx = PANEL_X + 120 + (n.x / 1100) * 400;
    const sy = PANEL_Y + 40 + 26 + n.fromLine * LINE_H - NODE_H / 2;
    const k = EASE.inOut(Math.max(0, Math.min(1, t * 1.25 - (n.x / 1100) * 0.25)));
    return { x: lerp(sx, ox + n.x, k), y: lerp(sy, oy + n.y, k), k };
  };
  const byId = Object.fromEntries(NODES.map((n) => [n.id, n]));
  const wireP = prog(frame, 24, 56, EASE.inOut);
  const flow = (frame % 40) / 40;
  return (
    <AbsoluteFill style={{ opacity: fade }}>
      <div style={{ position: "absolute", left: PANEL_X, top: PANEL_Y - 62, display: "flex", gap: 4, padding: 4, borderRadius: 12, background: "rgba(255,255,255,0.06)", opacity: t }}>
        {["Code", "Graph"].map((l, i) => (
          <div key={l} style={{ padding: "8px 22px", borderRadius: 9, fontFamily: FONT.body, fontWeight: 600, fontSize: 18, color: i === 1 ? "#0b0d18" : C.dim, background: i === 1 ? "#fff" : "transparent" }}>
            {l}
          </div>
        ))}
      </div>
      <svg width={1920} height={1080} style={{ position: "absolute", inset: 0 }}>
        {LINKS.map(([a, b, kind], i) => {
          const A = pos(byId[a]);
          const B = pos(byId[b]);
          const x1 = A.x + NODE_W;
          const y1 = A.y + (kind === "exec" ? 30 : NODE_H - 26);
          const x2 = B.x;
          const y2 = B.y + (kind === "exec" ? 30 : NODE_H - 26);
          const dx = Math.max(60, Math.abs(x2 - x1) * 0.5);
          const d = `M ${x1} ${y1} C ${x1 + dx} ${y1} ${x2 - dx} ${y2} ${x2} ${y2}`;
          const color = kind === "exec" ? "#ffffff" : C.sky;
          return (
            <g key={i} opacity={wireP}>
              <path d={d} fill="none" stroke={color} strokeOpacity={0.55} strokeWidth={kind === "exec" ? 3.5 : 2.5} />
              {kind === "exec" && (
                <path d={d} fill="none" stroke={C.sunset} strokeWidth={4} strokeLinecap="round" pathLength={1} strokeDasharray="0.06 1" strokeDashoffset={-flow} />
              )}
            </g>
          );
        })}
      </svg>
      {NODES.map((n) => {
        const p = pos(n);
        const head = n.kind === "event" ? C.violet : n.kind === "exec" ? "#3a6fd8" : "#2b7a6a";
        return (
          <div
            key={n.id}
            style={{
              position: "absolute",
              left: p.x,
              top: p.y,
              width: NODE_W,
              height: NODE_H,
              borderRadius: 12,
              overflow: "hidden",
              background: "rgba(26,28,42,0.97)",
              border: "1px solid rgba(255,255,255,0.1)",
              boxShadow: "0 14px 40px rgba(0,0,0,0.45)",
              opacity: Math.min(1, p.k * 2),
            }}
          >
            <div style={{ height: 36, background: head, display: "flex", alignItems: "center", padding: "0 12px", fontFamily: FONT.mono, fontSize: 16, fontWeight: 700, color: "#fff", whiteSpace: "nowrap" }}>
              {n.title}
            </div>
            <div style={{ padding: "10px 12px", fontFamily: FONT.body, fontSize: 15, color: C.dim, display: "flex", gap: 10 }}>
              {n.kind !== "data" && <span>▶ exec</span>}
              {(n.pins ?? ["value"]).map((pin) => (
                <span key={pin} style={{ color: "#8fc0ff" }}>● {pin}</span>
              ))}
            </div>
          </div>
        );
      })}
      <div style={{ position: "absolute", left: 96, top: 330, width: 460, fontFamily: FONT.body, fontSize: 26, lineHeight: 1.45, color: C.dim, opacity: prog(frame, 40, 60) }}>
        Code and graph are two views of one behavior. Edit either; the round trip is lossless.
      </div>
    </AbsoluteFill>
  );
};

const Native: React.FC<{ t: number; frame: number }> = ({ t, frame }) => {
  const stats = [
    { v: "5–13.7×", l: "faster register VM than Wander 1", at: 70 },
    { v: "Up to 33×", l: "on hot loops, compiled ahead of time to C++", at: 82 },
    { v: "0.24 ms", l: "an empty tick across 5,000 entities", at: 94 },
  ];
  return (
    <AbsoluteFill style={{ opacity: t }}>
      <div style={{ position: "absolute", left: 96, top: 270, width: 800, transform: `translateY(${(1 - t) * 40}px)` }}>
        <Window title="wander_check · disassemble" width={800} height={470} tint="#101219">
          <div style={{ padding: "20px 24px", fontFamily: FONT.mono, fontSize: 20, lineHeight: 1.6 }}>
            {BYTECODE.map((l, i) => {
              const on = frame > 6 + i * 3;
              const hot = Math.floor(frame / 6) % (BYTECODE.length - 1) === i - 1 && frame > 40;
              return (
                <div key={i} style={{ whiteSpace: "pre", opacity: on ? 1 : 0, color: i === 0 ? C.violet : hot ? "#fff" : "rgba(200,210,240,0.72)", background: hot ? "rgba(79,141,255,0.18)" : "transparent", borderRadius: 4 }}>
                  {l}
                </div>
              );
            })}
          </div>
        </Window>
        <div style={{ marginTop: 14, fontFamily: FONT.body, fontSize: 20, color: C.faint }}>Register bytecode, 8-byte instructions</div>
      </div>
      <div style={{ position: "absolute", left: 940, top: 270, width: 890, transform: `translateY(${(1 - t) * 60}px)` }}>
        <Window title="native/module.cpp" width={890} height={470} tint="#101219">
          <div style={{ padding: "20px 24px", fontFamily: FONT.mono, fontSize: 17, lineHeight: 1.62 }}>
            {NATIVE.map((l, i) => (
              <div key={i} style={{ whiteSpace: "pre", opacity: prog(frame, 14 + i * 2, 24 + i * 2) }}>
                <CodeLine line={l} lang="cpp" />
              </div>
            ))}
          </div>
        </Window>
        <div style={{ marginTop: 14, fontFamily: FONT.body, fontSize: 20, color: C.faint }}>Native C++ modules, hot-reloaded by native_build</div>
      </div>
      <div style={{ position: "absolute", left: 96, right: 96, top: 830, display: "flex", justifyContent: "space-between" }}>
        {stats.map((s) => {
          const p = prog(frame, s.at, s.at + 18);
          return (
            <div key={s.v} style={{ opacity: p, transform: `translateY(${(1 - p) * 20}px)`, width: 540 }}>
              <div style={{ fontFamily: FONT.display, fontWeight: 700, fontSize: 64, letterSpacing: -2, color: C.text }}>{s.v}</div>
              <div style={{ fontFamily: FONT.body, fontSize: 22, color: C.dim, marginTop: 2 }}>{s.l}</div>
            </div>
          );
        })}
      </div>
      <div style={{ position: "absolute", right: 96, top: 1010, fontFamily: FONT.body, fontSize: 15, color: C.faint, opacity: prog(frame, 100, 120) }}>
        wander_bench, M1 Pro, release build
      </div>
    </AbsoluteFill>
  );
};

