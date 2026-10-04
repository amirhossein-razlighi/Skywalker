import React from "react";
import { evolvePath, getLength } from "@remotion/paths";
import { AbsoluteFill, interpolate, Sequence, useCurrentFrame, useVideoConfig } from "remotion";
import toolsData from "../data/tools.json";
import { Footage } from "../components/Footage";
import { AppIcon } from "../components/Logo";
import { Backdrop } from "../components/Overlays";
import { Counter, Kicker, Words } from "../components/Type";
import { C, EASE, FONT, SPRING, hash, lerp, prog, sp } from "../theme";
import { Chip, ToolCall, usePublicJson, Window } from "../ui/kit";

/**
 * The idea (12 bars, 864 frames): meet Skywalker; everything is a tool; agents see; agents act; one surface.
 * Vocabulary: scale-through (we zoom *into* an element to reach the next beat) and blur dissolves.
 */

const TOOL_COUNT = toolsData.count;

const Meet: React.FC = () => {
  const frame = useCurrentFrame();
  const out = prog(frame, 128, 150, EASE.in);
  return (
    <Backdrop y={60}>
      <AbsoluteFill style={{ alignItems: "center", justifyContent: "center", opacity: 1 - out, transform: `scale(${1 + out * 0.15})`, filter: `blur(${out * 12}px)` }}>
        <AppIcon size={190} at={4} />
        <div style={{ height: 40 }} />
        <Words text="Meet Skywalker." at={18} size={120} weight={700} />
        <div style={{ height: 22 }} />
        <Words text="A game engine whose native user is an *agent.*" at={48} size={54} weight={500} color={C.dim} stagger={2} tracking={-1.2} />
      </AbsoluteFill>
    </Backdrop>
  );
};

/** 162 real tool names from the live registry, as a wall of chips seen in perspective. */
const ToolWall: React.FC = () => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();
  const tools = toolsData.tools;
  const cols = 9;
  const hero = tools.findIndex((t) => t.name === "viewport_capture");
  // scale-through into viewport_capture at the end
  const dive = prog(frame, 150, 180, EASE.in);
  const catColor: Record<string, string> = {
    studio: C.violet, render: C.sunset, view: C.sky, world: "#6fd3a8", wander: "#c792ff", asset: "#8fb3ff", dcc: "#ffd27a", physics: "#7fd4ff", animation: "#ff9fb4",
  };
  const heroCol = hero % cols;
  const heroRow = Math.floor(hero / cols);
  const cellW = 300;
  const cellH = 58;
  const gridW = cols * cellW;
  const hx = heroCol * cellW + cellW / 2 - gridW / 2;
  const hy = heroRow * cellH + cellH / 2 - (Math.ceil(tools.length / cols) * cellH) / 2;
  const drift = interpolate(frame, [0, 180], [140, -60]);
  const scale = lerp(1, 7, dive);
  return (
    <AbsoluteFill style={{ background: C.void, overflow: "hidden" }}>
      <AbsoluteFill style={{ perspective: 1400, alignItems: "center", justifyContent: "center" }}>
        <div
          style={{
            position: "relative",
            width: gridW,
            transform: `translate(${-hx * dive}px, ${drift * (1 - dive) - hy * dive}px) rotateX(${22 * (1 - dive)}deg) scale(${scale})`,
            transformStyle: "preserve-3d",
          }}
        >
          {tools.map((t, i) => {
            const col = i % cols;
            const row = Math.floor(i / cols);
            const d = (col + row * 0.6) * 1.2 + hash(i) * 8;
            const s = sp(frame, fps, d, SPRING.snappy);
            const isHero = i === hero;
            const lit = isHero ? prog(frame, 110, 130) : 0.25 + 0.75 * (hash(i * 3.1 + Math.floor(frame / 9)) > 0.93 ? 1 : 0);
            const color = catColor[t.cat] ?? C.uiText;
            return (
              <div
                key={t.name}
                style={{
                  position: "absolute",
                  left: col * cellW,
                  top: row * cellH,
                  width: cellW - 14,
                  height: cellH - 12,
                  borderRadius: 10,
                  display: "flex",
                  alignItems: "center",
                  padding: "0 14px",
                  fontFamily: FONT.mono,
                  fontSize: 19,
                  color: isHero ? "#fff" : color,
                  background: isHero ? `rgba(79,141,255,${0.15 + 0.4 * lit})` : "rgba(255,255,255,0.035)",
                  border: `1px solid ${isHero ? `rgba(127,180,255,${0.4 + 0.6 * lit})` : "rgba(255,255,255,0.06)"}`,
                  opacity: s * (isHero ? 1 : 0.35 + 0.45 * lit) * (1 - dive * (isHero ? 0 : 0.8)),
                  transform: `translateY(${(1 - s) * 30}px)`,
                  boxShadow: isHero ? `0 0 ${40 * lit}px rgba(79,141,255,${0.6 * lit})` : undefined,
                }}
              >
                {t.name}
              </div>
            );
          })}
        </div>
      </AbsoluteFill>
      <AbsoluteFill style={{ background: "radial-gradient(60% 55% at 50% 50%, rgba(4,5,13,0.88), rgba(4,5,13,0.35) 70%, transparent)", opacity: 1 - dive }} />
      <AbsoluteFill style={{ alignItems: "center", justifyContent: "center", opacity: 1 - prog(frame, 128, 146) }}>
        <Words text="Everything is a tool." at={6} size={124} weight={700} />
        <div style={{ height: 26 }} />
        <div style={{ fontFamily: FONT.display, fontSize: 46, fontWeight: 500, color: C.dim, letterSpacing: -0.8, opacity: prog(frame, 40, 60) }}>
          <Counter to={TOOL_COUNT} at={40} dur={50} style={{ color: C.text, fontWeight: 700 }} /> typed tools. One for every capability.
        </div>
      </AbsoluteFill>
    </AbsoluteFill>
  );
};

type Marks = { width: number; height: number; visible: { id: number; name: string; box: [number, number, number, number]; coverage: number }[] };

/** Pick a readable set of set-of-mark labels from the capture's entity list. */
const pickMarks = (m: Marks | null) => {
  if (!m) return [];
  const seen = new Set<string>();
  const out: Marks["visible"] = [];
  for (const v of [...m.visible].sort((a, b) => b.box[2] * b.box[3] - a.box[2] * a.box[3])) {
    const key = v.name.replace(/\s*[-\d]+$/, "").replace(/ \d.*$/, "");
    if (v.coverage > 0.2 || v.box[2] < 60 || seen.has(key)) continue;
    if (out.some((o) => Math.abs(o.box[0] - v.box[0]) < 90 && Math.abs(o.box[1] - v.box[1]) < 60)) continue;
    seen.add(key);
    out.push(v);
    if (out.length >= 12) break;
  }
  return out;
};

const VIEW_W = 1280;
const VIEW_H = 720;

/** Agents see: a capture with set-of-mark labels drawn from the real entity list the tool returns. */
const See: React.FC = () => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();
  const marks = usePublicJson<Marks>("footage/see_capture.marks.json");
  const picked = pickMarks(marks);
  const zoomIn = sp(frame, fps, 0, SPRING.gentle);
  const sx = VIEW_W / (marks?.width ?? 1920);
  const sy = VIEW_H / (marks?.height ?? 1080);
  const listItems = picked.slice(0, 8);
  return (
    <Backdrop y={20}>
      <div style={{ position: "absolute", left: 96, top: 92 }}>
        <Kicker text="Agents see" at={4} color={C.sky} />
        <Words text="It sees what it builds." at={10} size={64} weight={700} align="left" />
      </div>
      <div style={{ position: "absolute", left: 96, top: 270, transform: `scale(${lerp(1.25, 1, zoomIn)})`, transformOrigin: "30% 40%", opacity: zoomIn }}>
        <Window title="Harvest Fair — viewport_capture" width={VIEW_W} height={VIEW_H + 40}>
          <div style={{ position: "absolute", inset: 0 }}>
            <Footage slot="see_capture" kb={{ from: [0, 0, 1], to: [0, 0, 1.02] }} duration={190} />
            <svg width={VIEW_W} height={VIEW_H} style={{ position: "absolute", inset: 0 }}>
              {picked.map((v, i) => {
                const at = 40 + i * 5;
                const p = prog(frame, at, at + 16);
                if (p <= 0) return null;
                const [x, y, w, h] = v.box;
                const X = x * sx;
                const Y = y * sy;
                const Wd = w * sx;
                const Hd = h * sy;
                const per = 2 * (Wd + Hd);
                const color = i % 3 === 0 ? C.sunset : i % 3 === 1 ? "#7fb4ff" : C.violet;
                return (
                  <g key={v.id}>
                    <rect x={X} y={Y} width={Wd} height={Hd} rx={4} fill="none" stroke={color} strokeWidth={2} strokeDasharray={per} strokeDashoffset={per * (1 - p)} opacity={0.95} />
                    <g opacity={prog(frame, at + 8, at + 18)}>
                      <rect x={X} y={Y - 26} width={12 + String(v.id).length * 11.5 + 12} height={24} rx={5} fill={color} />
                      <text x={X + 8} y={Y - 8} fontFamily={FONT.mono} fontWeight={700} fontSize={17} fill="#0b0d18">
                        #{v.id}
                      </text>
                    </g>
                  </g>
                );
              })}
            </svg>
          </div>
        </Window>
      </div>
      <div style={{ position: "absolute", left: 1420, top: 270, width: 420 }}>
        <ToolCall tool="viewport_capture" args={'{"annotate": true}'} at={14} result="PNG + 714 entities with screen boxes" width={420} />
        <div style={{ marginTop: 22, fontFamily: FONT.mono, fontSize: 17, lineHeight: 1.75 }}>
          {listItems.map((v, i) => {
            const p = prog(frame, 70 + i * 6, 84 + i * 6);
            return (
              <div key={v.id} style={{ opacity: p, transform: `translateX(${(1 - p) * 16}px)`, color: C.dim, whiteSpace: "nowrap", overflow: "hidden" }}>
                <span style={{ color: "#7fb4ff" }}>#{v.id}</span> <span style={{ color: C.text }}>{v.name}</span> <span style={{ color: C.faint }}>[{v.box.join(",")}]</span>
              </div>
            );
          })}
        </div>
      </div>
    </Backdrop>
  );
};

/** Agents act: a tool call changes the world; history records who did what. */
const Act: React.FC = () => {
  const frame = useCurrentFrame();
  const wipe = prog(frame, 52, 96, EASE.inOut);
  const mask = `linear-gradient(180deg, black ${wipe * 120 - 20}%, transparent ${wipe * 120}%)`;
  const hist = [
    { actor: "agent:Aurora", tool: "environment_update", what: "Golden hour", c: C.violet, at: 100 },
    { actor: "mcp:claude-code", tool: "batch", what: "Scatter 120 pumpkins", c: C.sky, at: 110 },
    { actor: "agent:Cirro", tool: "entity_move", what: "Move the carousel", c: C.violet, at: 120 },
    { actor: "you", tool: "entity_set", what: "Lantern colour", c: C.sunset, at: 130 },
  ];
  return (
    <Backdrop y={20}>
      <div style={{ position: "absolute", left: 96, top: 92 }}>
        <Kicker text="Agents act" at={2} color={C.violet} />
        <Words text="And changes it, precisely." at={8} size={64} weight={700} align="left" />
      </div>
      <div style={{ position: "absolute", left: 96, top: 270 }}>
        <Window title="Harvest Fair" width={VIEW_W} height={VIEW_H + 40}>
          <div style={{ position: "absolute", inset: 0 }}>
            <Footage slot="act_before" kb={{ from: [0, 0, 1.02], to: [0, 0, 1.06] }} duration={180} />
            <div style={{ position: "absolute", inset: 0, WebkitMaskImage: mask, maskImage: mask }}>
              <Footage slot="act_after" kb={{ from: [0, 0, 1.02], to: [0, 0, 1.06] }} duration={180} />
            </div>
            <div
              style={{
                position: "absolute",
                inset: 0,
                background: `linear-gradient(180deg, transparent ${wipe * 120 - 8}%, rgba(255,184,115,${0.5 * Math.sin(Math.PI * wipe)}) ${wipe * 120 - 1}%, transparent ${wipe * 120 + 2}%)`,
                mixBlendMode: "screen",
              }}
            />
          </div>
        </Window>
      </div>
      <div style={{ position: "absolute", left: 1420, top: 270, width: 420 }}>
        <ToolCall tool="environment_update" args={'{"sunElevation": 7}'} at={12} actor="agent:Aurora" result="Sun set to 7°, undoable" width={420} />
        <div style={{ marginTop: 26, fontFamily: FONT.body, fontSize: 14, letterSpacing: 1.6, color: C.faint, opacity: prog(frame, 96, 110) }}>HISTORY</div>
        {hist.map((h, i) => {
          const p = prog(frame, h.at, h.at + 16);
          return (
            <div
              key={i}
              style={{
                marginTop: 10,
                padding: "10px 14px",
                borderRadius: 10,
                background: i === 0 ? "rgba(154,134,255,0.12)" : "rgba(255,255,255,0.04)",
                border: "1px solid rgba(255,255,255,0.06)",
                opacity: p,
                transform: `translateY(${(1 - p) * 12}px)`,
              }}
            >
              <div style={{ fontFamily: FONT.mono, fontSize: 15, color: h.c }}>{h.actor}</div>
              <div style={{ fontFamily: FONT.body, fontSize: 18, color: C.text, marginTop: 2 }}>
                {h.what} <span style={{ fontFamily: FONT.mono, fontSize: 14, color: C.faint }}>{h.tool}</span>
              </div>
            </div>
          );
        })}
      </div>
      <div style={{ position: "absolute", left: 96, bottom: 34, fontFamily: FONT.display, fontSize: 30, color: C.dim, opacity: prog(frame, 120, 140), letterSpacing: -0.4 }}>
        Every change is undoable, and signed by whoever made it.
      </div>
    </Backdrop>
  );
};

/** One surface: the editor, the crew, the CLI and external agents all call the same tools. */
const Surface: React.FC = () => {
  const frame = useCurrentFrame();
  const sources = [
    { label: "Editor", sub: "you" },
    { label: "Studio crew", sub: "in-editor agents" },
    { label: "CLI", sub: "skywalker call" },
    { label: "MCP clients", sub: "Claude Code · Codex · Gemini CLI · Cursor" },
  ];
  const cx = 1180;
  const cy = 600;
  const exit = prog(frame, 148, 164, EASE.in);
  return (
    <Backdrop y={50}>
      <AbsoluteFill style={{ opacity: 1 - exit, transform: `scale(${1 + exit * 0.3})`, filter: `blur(${exit * 10}px)` }}>
        <div style={{ position: "absolute", left: 0, right: 0, top: 110, display: "flex", justifyContent: "center" }}>
          <Words text="One surface. For people *and* *agents.*" at={4} size={78} weight={700} />
        </div>
        <svg width={1920} height={1080} style={{ position: "absolute", inset: 0 }}>
          {sources.map((s, i) => {
            const y = 360 + i * 135;
            const d = `M 640 ${y} C 860 ${y} 900 ${cy} ${cx - 150} ${cy}`;
            const p = prog(frame, 30 + i * 6, 70 + i * 6, EASE.inOut);
            const e = evolvePath(p, d);
            const pulse = ((frame - 70 - i * 9) % 45) / 45;
            return (
              <g key={i}>
                <path d={d} fill="none" stroke="rgba(127,180,255,0.35)" strokeWidth={2} strokeDasharray={e.strokeDasharray} strokeDashoffset={e.strokeDashoffset} />
                {frame > 76 + i * 6 && (
                  <path d={d} fill="none" stroke="#bcd4ff" strokeWidth={4} strokeLinecap="round" strokeDasharray={`18 ${getLength(d)}`} strokeDashoffset={-pulse * getLength(d)} />
                )}
              </g>
            );
          })}
        </svg>
        {sources.map((s, i) => {
          const y = 360 + i * 135;
          const p = prog(frame, 16 + i * 6, 36 + i * 6);
          return (
            <div key={i} style={{ position: "absolute", left: 230, top: y - 42, width: 410, opacity: p, transform: `translateX(${(1 - p) * -20}px)` }}>
              <div style={{ fontFamily: FONT.display, fontWeight: 600, fontSize: 36, color: C.text, letterSpacing: -0.6 }}>{s.label}</div>
              <div style={{ fontFamily: FONT.body, fontSize: 19, color: C.dim, marginTop: 4 }}>{s.sub}</div>
            </div>
          );
        })}
        <div
          style={{
            position: "absolute",
            left: cx - 150,
            top: cy - 150,
            width: 300,
            height: 300,
            borderRadius: 40,
            background: "linear-gradient(145deg, rgba(79,141,255,0.22), rgba(154,134,255,0.18))",
            border: "1px solid rgba(154,134,255,0.45)",
            boxShadow: `0 0 ${60 + 20 * Math.sin(frame / 8)}px rgba(120,130,255,0.35)`,
            display: "flex",
            flexDirection: "column",
            alignItems: "center",
            justifyContent: "center",
            opacity: prog(frame, 50, 70),
            transform: `scale(${0.9 + 0.1 * prog(frame, 50, 75)})`,
          }}
        >
          <div style={{ fontFamily: FONT.display, fontWeight: 700, fontSize: 110, color: C.text, letterSpacing: -4 }}>{TOOL_COUNT}</div>
          <div style={{ fontFamily: FONT.body, fontWeight: 600, fontSize: 22, color: C.dim, letterSpacing: 3 }}>TOOLS</div>
        </div>
        <div style={{ position: "absolute", left: cx + 220, top: cy - 120, width: 420, opacity: prog(frame, 80, 100) }}>
          {["Typed JSON schemas", "Did-you-mean errors", "Undoable, attributed edits", "Deterministic 60 Hz simulation"].map((t, i) => (
            <div key={t} style={{ opacity: prog(frame, 84 + i * 6, 100 + i * 6), marginBottom: 16 }}>
              <Chip color={i % 2 ? C.violet : C.sky} style={{ fontSize: 22 }}>{t}</Chip>
            </div>
          ))}
        </div>
      </AbsoluteFill>
    </Backdrop>
  );
};

export const S3Idea: React.FC = () => (
  <AbsoluteFill style={{ background: C.void }}>
    <Sequence durationInFrames={150}>
      <Meet />
    </Sequence>
    <Sequence from={150} durationInFrames={182}>
      <ToolWall />
    </Sequence>
    <Sequence from={326} durationInFrames={196}>
      <BlurIn>
        <See />
      </BlurIn>
    </Sequence>
    <Sequence from={516} durationInFrames={184}>
      <BlurIn>
        <Act />
      </BlurIn>
    </Sequence>
    <Sequence from={696} durationInFrames={168}>
      <BlurIn>
        <Surface />
      </BlurIn>
    </Sequence>
  </AbsoluteFill>
);

/** Blur-dissolve entrance used between beats in this section. */
const BlurIn: React.FC<{ children: React.ReactNode; d?: number }> = ({ children, d = 14 }) => {
  const frame = useCurrentFrame();
  const p = prog(frame, 0, d, EASE.out);
  return <AbsoluteFill style={{ opacity: p, filter: p < 1 ? `blur(${(1 - p) * 16}px)` : undefined }}>{children}</AbsoluteFill>;
};
