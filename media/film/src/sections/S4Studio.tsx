import React from "react";
import { evolvePath } from "@remotion/paths";
import { AbsoluteFill, interpolate, Sequence, useCurrentFrame, useVideoConfig } from "remotion";
import { Backdrop } from "../components/Overlays";
import { Kicker, Words } from "../components/Type";
import { C, EASE, FONT, SPRING, lerp, prog, sp } from "../theme";
import { Cloudling } from "../ui/kit";

/**
 * The Studio (10 bars, 720 frames): a crew of specialist agents, a board, feedback the director rules on,
 * and loops you define. Vocabulary: springy cards, things flying between people, a turning loop.
 */

type Face = "happy" | "focused" | "curious" | "wink" | "determined" | "dreamy";
const CREW: { name: string; role: string; color: string; face: Face }[] = [
  { name: "Nimbus", role: "Creative director", color: "#9a86ff", face: "determined" },
  { name: "Cirro", role: "Level designer", color: "#4f8dff", face: "curious" },
  { name: "Stratus", role: "Gameplay programmer", color: "#4fd1b5", face: "focused" },
  { name: "Aurora", role: "Lighting artist", color: "#ffb873", face: "dreamy" },
  { name: "Haze", role: "Writer", color: "#ff9fb4", face: "happy" },
  { name: "Drizzle", role: "Playtester", color: "#7fd4ff", face: "wink" },
  { name: "Vapor", role: "Critic", color: "#c3c8e6", face: "focused" },
];

const Crew: React.FC = () => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();
  const exit = prog(frame, 136, 156, EASE.in);
  return (
    <Backdrop y={80}>
      <AbsoluteFill style={{ opacity: 1 - exit }}>
        <div style={{ position: "absolute", top: 150, left: 0, right: 0, display: "flex", flexDirection: "column", alignItems: "center" }}>
          <Kicker text="The Studio" at={0} color={C.violet} />
          <div style={{ height: 14 }} />
          <Words text="Not one agent. A *studio.*" at={6} size={110} weight={700} />
        </div>
        {CREW.map((m, i) => {
          const n = CREW.length;
          const a = (i - (n - 1) / 2) / ((n - 1) / 2); // -1..1
          const x = 960 + a * 720;
          const y = 690 + a * a * -70 + 60;
          const s = sp(frame, fps, 26 + i * 4, SPRING.pop);
          const bob = Math.sin((frame + i * 13) / 14) * 5;
          return (
            <div key={m.name} style={{ position: "absolute", left: x - 90, top: y - 90 + bob, width: 180, textAlign: "center", opacity: Math.min(1, s * 1.5), transform: `scale(${0.5 + 0.5 * s})` }}>
              <Cloudling size={150} color={m.color} face={m.face} blink />
              <div style={{ fontFamily: FONT.display, fontWeight: 600, fontSize: 28, color: C.text, marginTop: 8, letterSpacing: -0.4 }}>{m.name}</div>
              <div style={{ fontFamily: FONT.body, fontSize: 17, color: C.dim, marginTop: 2 }}>{m.role}</div>
            </div>
          );
        })}
      </AbsoluteFill>
    </Backdrop>
  );
};

const COLS = ["Backlog", "Todo", "Doing", "Review", "Done"];
type Card = { id: string; title: string; who: number; path: [number, number][] };
// path: [frame, column]
const CARDS: Card[] = [
  { id: "T-12", title: "Block out the canyon run", who: 1, path: [[0, 2], [60, 3], [120, 4]] },
  { id: "T-13", title: "Golden-hour lighting pass", who: 3, path: [[0, 1], [40, 2], [150, 3]] },
  { id: "T-14", title: "Coin + checkpoint behaviors", who: 2, path: [[0, 1], [80, 2], [170, 3]] },
  { id: "T-15", title: "Write the ferryman's lines", who: 4, path: [[0, 0], [100, 1], [190, 2]] },
  { id: "T-16", title: "Playtest: first five minutes", who: 5, path: [[0, 0], [130, 1]] },
  { id: "T-17", title: "Art review of the pier", who: 6, path: [[0, 0]] },
];

const colAt = (c: Card, f: number) => {
  let i = 0;
  while (i < c.path.length - 1 && c.path[i + 1][0] <= f) i++;
  const cur = c.path[i];
  const next = c.path[i + 1];
  if (!next) return cur[1];
  const t = EASE.inOut(Math.max(0, Math.min(1, (f - (next[0] - 18)) / 18)));
  return lerp(cur[1], next[1], f >= next[0] - 18 ? t : 0);
};

const Board: React.FC = () => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();
  const x0 = 380;
  const colW = 290;
  const y0 = 330;
  const msgs = [
    { who: 0, text: "@Cirro the ledge after the bridge reads as a dead end.", at: 50 },
    { who: 1, text: "Moving it 4 m left and adding a lantern as a lure.", at: 96 },
    { who: 2, text: "Checkpoint behaviors pass wander_test. 6/6.", at: 150 },
  ];
  const exit = prog(frame, 214, 230, EASE.in);
  return (
    <Backdrop y={10}>
      <AbsoluteFill style={{ opacity: 1 - exit, filter: exit ? `blur(${exit * 10}px)` : undefined }}>
        <div style={{ position: "absolute", left: 96, top: 92 }}>
          <Kicker text="A shared board" at={0} color={C.sky} />
          <Words text="Directors, designers, developers, playtesters." at={6} size={60} weight={700} align="left" stagger={2} />
        </div>
        {/* crew column */}
        {CREW.slice(0, 6).map((m, i) => {
          const s = sp(frame, fps, i * 3, SPRING.snappy);
          const talking = msgs.some((g) => g.who === i && frame >= g.at && frame < g.at + 60);
          return (
            <div key={m.name} style={{ position: "absolute", left: 120, top: 300 + i * 104, display: "flex", alignItems: "center", gap: 14, opacity: s }}>
              <div style={{ transform: `scale(${talking ? 1.08 : 1})` }}>
                <Cloudling size={74} color={m.color} face={m.face} blink />
              </div>
              <div style={{ fontFamily: FONT.body, fontWeight: 600, fontSize: 18, color: talking ? C.text : C.dim }}>{m.name}</div>
            </div>
          );
        })}
        {COLS.map((c, i) => (
          <div key={c} style={{ position: "absolute", left: x0 + i * colW, top: y0 - 50, width: colW - 24, opacity: prog(frame, 6 + i * 3, 22 + i * 3) }}>
            <div style={{ fontFamily: FONT.body, fontWeight: 600, fontSize: 16, letterSpacing: 2, color: C.faint, textTransform: "uppercase" }}>{c}</div>
            <div style={{ marginTop: 12, height: 560, borderRadius: 16, background: "rgba(255,255,255,0.025)", border: "1px solid rgba(255,255,255,0.05)" }} />
          </div>
        ))}
        {CARDS.map((card, k) => {
          const col = colAt(card, frame);
          const sameColBefore = CARDS.slice(0, k).filter((o) => Math.round(colAt(o, frame)) === Math.round(col)).length;
          const s = sp(frame, fps, 12 + k * 4, SPRING.snappy);
          const m = CREW[card.who];
          const moving = Math.abs(col - Math.round(col)) > 0.02;
          return (
            <div
              key={card.id}
              style={{
                position: "absolute",
                left: x0 + 12 + col * colW,
                top: y0 + 14 + sameColBefore * 118,
                width: colW - 48,
                padding: "14px 16px",
                borderRadius: 12,
                background: moving ? "rgba(60,64,96,0.95)" : "rgba(34,37,56,0.92)",
                border: `1px solid ${moving ? m.color : "rgba(255,255,255,0.07)"}`,
                boxShadow: moving ? `0 20px 50px rgba(0,0,0,0.5), 0 0 30px ${m.color}55` : "0 8px 24px rgba(0,0,0,0.3)",
                transform: `scale(${(moving ? 1.05 : 1) * (0.9 + 0.1 * s)}) rotate(${moving ? -1.5 : 0}deg)`,
                opacity: s,
                transition: "none",
              }}
            >
              <div style={{ display: "flex", justifyContent: "space-between", fontFamily: FONT.mono, fontSize: 14, color: C.faint }}>
                <span>{card.id}</span>
                <span style={{ color: m.color }}>{m.name}</span>
              </div>
              <div style={{ fontFamily: FONT.body, fontWeight: 500, fontSize: 18, color: C.text, marginTop: 6, lineHeight: 1.3 }}>{card.title}</div>
              {Math.round(col) === 4 && <div style={{ marginTop: 6, fontFamily: FONT.mono, fontSize: 13, color: C.green }}>✓ done</div>}
            </div>
          );
        })}
        {msgs.map((g, i) => {
          const p = sp(frame, fps, g.at, SPRING.snappy);
          const out = prog(frame, g.at + 64, g.at + 76);
          if (frame < g.at || out >= 1) return null;
          return (
            <div
              key={i}
              style={{
                position: "absolute",
                left: 222,
                top: 300 + g.who * 104 + 4,
                maxWidth: 560,
                padding: "12px 18px",
                borderRadius: "18px 18px 18px 4px",
                background: "rgba(255,255,255,0.95)",
                color: "#151826",
                fontFamily: FONT.body,
                fontSize: 19,
                fontWeight: 500,
                boxShadow: "0 16px 40px rgba(0,0,0,0.45)",
                opacity: p * (1 - out),
                transform: `translateX(${(1 - p) * -14}px) scale(${0.92 + 0.08 * p})`,
                transformOrigin: "left center",
                zIndex: 10,
              }}
            >
              <span style={{ color: CREW[g.who].color === "#c3c8e6" ? "#555" : CREW[g.who].color, fontWeight: 700 }}>{CREW[g.who].name} </span>
              {g.text}
            </div>
          );
        })}
      </AbsoluteFill>
    </Backdrop>
  );
};

const FEEDBACK = [
  { id: "F-7", from: 5, cat: "difficulty", text: "6 deaths at the canyon ledge. Feels unfair.", verdict: "ACT", note: "→ T-31, T-32", color: C.green },
  { id: "F-8", from: 6, cat: "visuals", text: "Sunset washes out the water.", verdict: "DEFER", note: "after the lighting pass", color: C.sunset },
  { id: "F-9", from: 5, cat: "fun", text: "Add a jetpack?", verdict: "DROP", note: "breaks the core fantasy", color: C.red },
];

const Triage: React.FC = () => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();
  const exit = prog(frame, 206, 222, EASE.in);
  return (
    <Backdrop y={30}>
      <AbsoluteFill style={{ opacity: 1 - exit }}>
        <div style={{ position: "absolute", left: 96, top: 92 }}>
          <Kicker text="Feedback" at={0} color={C.sunset} />
          <Words text="Playtesters report. The director decides." at={6} size={60} weight={700} align="left" stagger={2} />
        </div>
        {/* director */}
        <div style={{ position: "absolute", left: 180, top: 420, textAlign: "center", width: 240, opacity: prog(frame, 6, 20) }}>
          <Cloudling size={220} color={CREW[0].color} face="determined" blink />
          <div style={{ fontFamily: FONT.display, fontWeight: 600, fontSize: 32, color: C.text, marginTop: 8 }}>Nimbus</div>
          <div style={{ fontFamily: FONT.body, fontSize: 18, color: C.dim }}>Creative director</div>
        </div>
        {FEEDBACK.map((f, i) => {
          const at = 20 + i * 24;
          const s = sp(frame, fps, at, SPRING.snappy);
          const vAt = 100 + i * 22;
          const v = sp(frame, fps, vAt, { damping: 9, mass: 0.6, stiffness: 180 });
          const dropped = f.verdict === "DROP";
          const fade = dropped ? prog(frame, vAt + 30, vAt + 50) : 0;
          const y = 330 + i * 200;
          return (
            <div key={f.id} style={{ position: "absolute", left: 640, top: y, width: 900, opacity: s * (1 - 0.55 * fade), transform: `translateX(${(1 - s) * 120}px)` }}>
              <div
                style={{
                  padding: "22px 28px",
                  borderRadius: 18,
                  background: "rgba(30,33,52,0.92)",
                  border: "1px solid rgba(255,255,255,0.08)",
                  display: "flex",
                  alignItems: "center",
                  gap: 22,
                  boxShadow: "0 18px 50px rgba(0,0,0,0.4)",
                }}
              >
                <Cloudling size={70} color={CREW[f.from].color} face={CREW[f.from].face} />
                <div style={{ flex: 1 }}>
                  <div style={{ fontFamily: FONT.mono, fontSize: 16, color: C.faint }}>
                    {f.id} · {CREW[f.from].name} · <span style={{ color: C.violet }}>{f.cat}</span>
                  </div>
                  <div style={{ fontFamily: FONT.body, fontSize: 28, fontWeight: 500, color: C.text, marginTop: 4, textDecoration: fade > 0.5 ? "line-through" : "none" }}>{f.text}</div>
                </div>
              </div>
              {frame >= vAt && (
                <div
                  style={{
                    position: "absolute",
                    right: -40,
                    top: -16,
                    padding: "8px 18px",
                    borderRadius: 10,
                    border: `3px solid ${f.color}`,
                    color: f.color,
                    fontFamily: FONT.display,
                    fontWeight: 800,
                    fontSize: 30,
                    letterSpacing: 3,
                    background: "rgba(10,12,24,0.92)",
                    transform: `rotate(${-8 + 8 * (1 - v)}deg) scale(${lerp(2.2, 1, v)})`,
                    opacity: Math.min(1, v * 2),
                  }}
                >
                  {f.verdict}
                  <div style={{ fontFamily: FONT.body, fontWeight: 500, fontSize: 15, letterSpacing: 0, color: C.dim, marginTop: 2 }}>{f.note}</div>
                </div>
              )}
            </div>
          );
        })}
      </AbsoluteFill>
    </Backdrop>
  );
};

const STAGES = ["Playtest", "Triage", "Fix", "Verify"];

const Loop: React.FC = () => {
  const frame = useCurrentFrame();
  const cx = 1240;
  const cy = 560;
  const R = 250;
  const ring = `M ${cx} ${cy - R} A ${R} ${R} 0 1 1 ${cx - 0.01} ${cy - R}`;
  const draw = prog(frame, 6, 50, EASE.inOut);
  const e = evolvePath(draw, ring);
  const spin = Math.max(0, frame - 40) / 75; // laps
  const ang = spin * Math.PI * 2 - Math.PI / 2;
  const metric = prog(frame, 60, 110, EASE.inOut);
  return (
    <Backdrop y={40}>
      <div style={{ position: "absolute", left: 120, top: 320, width: 680 }}>
        <Kicker text="Loops" at={0} color={C.sky} />
        <Words text={"Loops you\ndefine."} at={6} size={110} weight={700} align="left" />
        <div style={{ marginTop: 34, fontFamily: FONT.mono, fontSize: 21, color: C.dim, opacity: prog(frame, 30, 46), lineHeight: 1.6 }}>
          studio_loop_define
          <br />
          {'{"template": "playtest_fix_verify"}'}
        </div>
        <div style={{ marginTop: 34, opacity: prog(frame, 56, 72), fontFamily: FONT.body, fontSize: 24, color: C.dim }}>
          completion rate{" "}
          <span style={{ fontFamily: FONT.display, fontWeight: 700, fontSize: 44, color: C.text, fontVariantNumeric: "tabular-nums" }}>{lerp(0.62, 0.94, metric).toFixed(2)}</span>{" "}
          <span style={{ color: metric >= 1 ? C.green : C.faint, fontWeight: 600 }}>{metric >= 1 ? "✓ verified" : "measuring"}</span>
        </div>
      </div>
      <svg width={1920} height={1080} style={{ position: "absolute", inset: 0 }}>
        <defs>
          <linearGradient id="ringg" x1="0" y1="0" x2="1" y2="1">
            <stop offset="0" stopColor={C.sky} />
            <stop offset="0.6" stopColor={C.violet} />
            <stop offset="1" stopColor={C.sunset} />
          </linearGradient>
        </defs>
        <path d={ring} fill="none" stroke="url(#ringg)" strokeWidth={6} strokeLinecap="round" strokeDasharray={e.strokeDasharray} strokeDashoffset={e.strokeDashoffset} opacity={0.9} />
        {frame > 40 && <circle cx={cx + Math.cos(ang) * R} cy={cy + Math.sin(ang) * R} r={14} fill="#fff" style={{ filter: "drop-shadow(0 0 14px #9a86ff)" }} />}
      </svg>
      {STAGES.map((s, i) => {
        const a = (i / STAGES.length) * Math.PI * 2 - Math.PI / 2;
        const x = cx + Math.cos(a) * (R + 0);
        const y = cy + Math.sin(a) * (R + 0);
        const p = prog(frame, 14 + i * 8, 30 + i * 8);
        const lit = (((spin % 1) + 1) % 1) * 4;
        const active = frame > 40 && Math.floor(lit + 0.5) % 4 === i;
        return (
          <div
            key={s}
            style={{
              position: "absolute",
              left: x - 100,
              top: y - 34,
              width: 200,
              height: 68,
              borderRadius: 34,
              display: "flex",
              alignItems: "center",
              justifyContent: "center",
              fontFamily: FONT.display,
              fontWeight: 600,
              fontSize: 28,
              color: active ? "#0b0d18" : C.text,
              background: active ? "#fff" : "rgba(20,23,40,0.95)",
              border: "1px solid rgba(154,134,255,0.45)",
              boxShadow: active ? "0 0 40px rgba(154,134,255,0.7)" : "none",
              opacity: p,
              transform: `scale(${0.8 + 0.2 * p})`,
            }}
          >
            {s}
          </div>
        );
      })}
    </Backdrop>
  );
};

export const S4Studio: React.FC = () => (
  <AbsoluteFill style={{ background: C.void }}>
    <Sequence durationInFrames={158}>
      <Crew />
    </Sequence>
    <Sequence from={152} durationInFrames={232}>
      <SlideIn>
        <Board />
      </SlideIn>
    </Sequence>
    <Sequence from={378} durationInFrames={224}>
      <SlideIn>
        <Triage />
      </SlideIn>
    </Sequence>
    <Sequence from={596} durationInFrames={124}>
      <SlideIn>
        <Loop />
      </SlideIn>
    </Sequence>
  </AbsoluteFill>
);

/** Section transition: the new beat slides up from below with a spring, the old one is already fading. */
const SlideIn: React.FC<{ children: React.ReactNode }> = ({ children }) => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();
  const s = sp(frame, fps, 0, SPRING.gentle);
  return <AbsoluteFill style={{ transform: `translateY(${(1 - s) * 140}px)`, opacity: interpolate(s, [0, 0.4], [0, 1], { extrapolateRight: "clamp" }) }}>{children}</AbsoluteFill>;
};
