import React from "react";
import { evolvePath } from "@remotion/paths";
import { AbsoluteFill, Sequence, useCurrentFrame, useVideoConfig } from "remotion";
import { Footage } from "../components/Footage";
import { Backdrop } from "../components/Overlays";
import { Kicker, Words } from "../components/Type";
import { C, EASE, FONT, SPRING, lerp, prog, sp } from "../theme";
import { Caret, Chip, ToolCall, typed, Window } from "../ui/kit";

/**
 * Works with your tools (8 bars, 576 frames): setup for coding agents, the Blender bridge, shipping a Mac app.
 * Vocabulary: terminals and tool calls; push-in and slide-over between beats.
 */

const SETUP = "skywalker setup claude";
const OUT = [
  { t: "✓ MCP server   skywalker  (attaches to the editor, else headless)", c: C.green },
  { t: "✓ 12 skills    agent loop, world building, look dev, Blender…", c: C.green },
  { t: "✓ 8 subagents  director, level designer, playtester, critic…", c: C.green },
  { t: "✓ 5 commands   /new-game  /look-dev  /playtest-loop …", c: C.green },
];
const CLIENTS = ["Claude Code", "Codex", "Gemini CLI", "Cursor"];

const Setup: React.FC = () => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();
  const cmd = typed(frame, 30, SETUP, 26);
  const cmdDone = 30 + (SETUP.length / 26) * fps;
  const exit = prog(frame, 186, 202, EASE.in);
  const hl = Math.floor(Math.max(0, frame - 70) / 22) % CLIENTS.length;
  return (
    <Backdrop y={40}>
      <AbsoluteFill style={{ opacity: 1 - exit, transform: `scale(${1 - exit * 0.04})` }}>
        <div style={{ position: "absolute", left: 96, top: 92 }}>
          <Kicker text="Integrations" at={0} color={C.sky} />
          <Words text="Works with the agents you already use." at={6} size={64} weight={700} align="left" stagger={2} />
        </div>
        <div style={{ position: "absolute", left: 96, top: 300 }}>
          <Window title="Terminal — my-game" width={1080} height={430} tint="#0d0f16">
            <div style={{ padding: "26px 30px", fontFamily: FONT.mono, fontSize: 24, lineHeight: 1.75, color: C.uiText }}>
              <div>
                <span style={{ color: C.violet }}>~/my-game</span> <span style={{ color: C.faint }}>$</span> {cmd}
                {frame < cmdDone + 8 && <Caret h={24} color={C.uiText} />}
              </div>
              {OUT.map((o, i) => {
                const at = cmdDone + 10 + i * 9;
                const p = prog(frame, at, at + 8);
                const [head, ...rest] = o.t.split("  ");
                return (
                  <div key={i} style={{ opacity: p, whiteSpace: "pre", fontSize: 21 }}>
                    <span style={{ color: o.c }}>{head}</span>
                    <span style={{ color: "rgba(227,228,232,0.68)" }}>{"  " + rest.join("  ")}</span>
                  </div>
                );
              })}
              <div style={{ opacity: prog(frame, cmdDone + 52, cmdDone + 60), marginTop: 6, fontSize: 21, color: C.dim }}>
                Ready. Ask your agent to build something.
              </div>
            </div>
          </Window>
        </div>
        <div style={{ position: "absolute", left: 1290, top: 312, width: 540 }}>
          {CLIENTS.map((c, i) => {
            const p = sp(frame, fps, 50 + i * 5, SPRING.snappy);
            const on = frame > 70 && hl === i;
            return (
              <div
                key={c}
                style={{
                  fontFamily: FONT.display,
                  fontWeight: 700,
                  fontSize: 64,
                  letterSpacing: -2,
                  lineHeight: 1.45,
                  color: on ? C.text : "rgba(226,230,255,0.32)",
                  opacity: p,
                  transform: `translateX(${(1 - p) * 30}px)`,
                }}
              >
                {c}
              </div>
            );
          })}
          <div style={{ marginTop: 18, fontFamily: FONT.body, fontSize: 22, color: C.dim, opacity: prog(frame, 80, 96) }}>…and any MCP client.</div>
        </div>
      </AbsoluteFill>
    </Backdrop>
  );
};

/** A procedural ruined tower drawing itself, as if arriving from Blender. */
const TOWER = [
  "M 760 880 L 760 420 L 800 380 L 800 330 L 840 330 L 840 360 L 880 360 L 880 320 L 920 320 L 920 370 L 960 370 L 960 330 L 990 330 L 1000 880",
  "M 760 880 L 1000 880",
  "M 790 470 L 790 520 L 830 520 L 830 470 Q 810 450 790 470",
  "M 900 560 L 900 620 L 940 620 L 940 560 Q 920 540 900 560",
  "M 830 700 L 830 880 M 930 700 L 930 880 M 830 700 Q 880 640 930 700",
  "M 760 600 L 1000 610 M 760 740 L 1000 745 M 760 480 L 860 482",
  "M 1000 880 L 1060 880 L 1040 860 L 1080 870 M 700 880 L 740 860 L 760 880",
  "M 600 900 L 1200 900",
];

const Blender: React.FC = () => {
  const frame = useCurrentFrame();
  const draw = prog(frame, 30, 120, EASE.inOut);
  const shade = prog(frame, 110, 150, EASE.inOut);
  const exit = prog(frame, 174, 188, EASE.in);
  return (
    <Backdrop y={60}>
      <AbsoluteFill style={{ opacity: 1 - exit }}>
        <div style={{ position: "absolute", left: 96, top: 92 }}>
          <Kicker text="DCC bridge" at={0} color={C.sunset} />
          <Words text="Round-trip with Blender." at={6} size={64} weight={700} align="left" />
        </div>
        <div style={{ position: "absolute", left: 96, top: 300, width: 600 }}>
          <ToolCall tool="dcc_generate" args={'{"recipe": "tower", "params": {"ruin": 0.6}}'} at={14} actor="agent:Terra" result="RuinedTower.glb placed at (10, 0, 4)" width={600} />
          <div style={{ marginTop: 30, display: "flex", flexWrap: "wrap", gap: 12, width: 600 }}>
            {["FBX", "OBJ", "USD", "Alembic", ".blend", "→ glTF"].map((f, i) => (
              <div key={f} style={{ opacity: prog(frame, 60 + i * 5, 72 + i * 5) }}>
                <Chip color={i === 5 ? C.sunset : C.sky} mono>{f}</Chip>
              </div>
            ))}
          </div>
          <div style={{ marginTop: 26, fontFamily: FONT.body, fontSize: 22, color: C.dim, lineHeight: 1.5, opacity: prog(frame, 90, 110) }}>
            Headless Blender scripts and a live-session add-on. Every asset lands in glTF, with its provenance.
          </div>
        </div>
        <svg width={1920} height={1080} style={{ position: "absolute", inset: 0 }} viewBox="-300 -40 1920 1080">
          <defs>
            <linearGradient id="towerfill" x1="0" y1="0" x2="0" y2="1">
              <stop offset="0" stopColor="#c9b79a" />
              <stop offset="1" stopColor="#6d5d4c" />
            </linearGradient>
          </defs>
          <path d={TOWER[0]} fill="url(#towerfill)" opacity={shade * 0.92} />
          {TOWER.map((d, i) => {
            const p = Math.max(0, Math.min(1, draw * 1.6 - i * 0.08));
            const e = evolvePath(EASE.inOut(p), d);
            return <path key={i} d={d} fill="none" stroke={lerp(0, 1, shade) > 0.5 ? "#3a2f25" : "#e9edff"} strokeWidth={3} strokeLinecap="round" strokeLinejoin="round" strokeDasharray={e.strokeDasharray} strokeDashoffset={e.strokeDashoffset} />;
          })}
        </svg>
      </AbsoluteFill>
    </Backdrop>
  );
};

const BUILD = "skywalker build --project examples/sky_dash --release";

const Ship: React.FC = () => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();
  const cmd = typed(frame, 16, BUILD, 40);
  const cmdDone = 16 + (BUILD.length / 40) * fps;
  const bar = prog(frame, cmdDone + 6, cmdDone + 50, EASE.inOut);
  const icon = sp(frame, fps, cmdDone + 52, SPRING.pop);
  const launch = prog(frame, cmdDone + 80, cmdDone + 110, EASE.inOut);
  return (
    <Backdrop y={50}>
      <div style={{ position: "absolute", left: 96, top: 92, opacity: 1 - launch }}>
        <Kicker text="Shipping" at={0} color={C.green} />
        <Words text="Ship it as a Mac app." at={6} size={64} weight={700} align="left" />
      </div>
      <div style={{ position: "absolute", left: 96, top: 300, opacity: 1 - launch }}>
        <Window title="Terminal" width={1000} height={250} tint="#0d0f16">
          <div style={{ padding: "24px 28px", fontFamily: FONT.mono, fontSize: 22, lineHeight: 1.8, color: C.uiText }}>
            <div style={{ whiteSpace: "pre" }}>
              <span style={{ color: C.faint }}>$</span> {cmd}
              {frame < cmdDone + 4 && <Caret h={22} color={C.uiText} />}
            </div>
            {bar > 0 && (
              <div style={{ display: "flex", alignItems: "center", gap: 18, marginTop: 8 }}>
                <div style={{ width: 520, height: 10, borderRadius: 5, background: "rgba(255,255,255,0.08)", overflow: "hidden" }}>
                  <div style={{ width: `${bar * 100}%`, height: "100%", background: `linear-gradient(90deg, ${C.sky}, ${C.violet})` }} />
                </div>
                <span style={{ color: C.dim, fontSize: 19 }}>{bar < 1 ? "packaging scenes, assets, behaviors…" : <span style={{ color: C.green }}>✓ Sky Dash.app</span>}</span>
              </div>
            )}
          </div>
        </Window>
        <div style={{ marginTop: 26, display: "flex", gap: 12, opacity: prog(frame, cmdDone + 50, cmdDone + 64) }}>
          {["Standalone player", "Gamepads", "MetalFX", "Signed .app"].map((c, i) => (
            <Chip key={c} color={i % 2 ? C.violet : C.sky}>{c}</Chip>
          ))}
        </div>
      </div>
      {/* the app icon, then the app launching full screen */}
      <div
        style={{
          position: "absolute",
          left: lerp(1330, 0, launch),
          top: lerp(300, 0, launch),
          width: lerp(380, 1920, launch),
          height: lerp(380, 1080, launch),
          borderRadius: lerp(84, 0, launch),
          overflow: "hidden",
          transform: `scale(${0.4 + 0.6 * icon})`,
          opacity: Math.min(1, icon * 2),
          boxShadow: "0 40px 100px rgba(0,0,0,0.6)",
        }}
      >
        <Footage slot="tools_app" duration={200} kb={{ from: [0, 0, 1.6 - 0.6 * launch], to: [0, 0, 1.6 - 0.6 * launch] }} placeholderLabel={false} />
        <div style={{ position: "absolute", inset: 0, background: "linear-gradient(160deg, rgba(255,255,255,0.25), transparent 40%)", opacity: 1 - launch }} />
      </div>
      <div style={{ position: "absolute", left: 1330, top: 700, width: 380, textAlign: "center", fontFamily: FONT.body, fontWeight: 600, fontSize: 26, color: C.text, opacity: icon * (1 - launch) }}>
        Sky Dash
      </div>
    </Backdrop>
  );
};

export const S8Tools: React.FC = () => (
  <AbsoluteFill style={{ background: C.void }}>
    <Sequence durationInFrames={204}>
      <Setup />
    </Sequence>
    <Sequence from={198} durationInFrames={190}>
      <PushIn>
        <Blender />
      </PushIn>
    </Sequence>
    <Sequence from={384} durationInFrames={192}>
      <PushIn>
        <Ship />
      </PushIn>
    </Sequence>
  </AbsoluteFill>
);

const PushIn: React.FC<{ children: React.ReactNode }> = ({ children }) => {
  const frame = useCurrentFrame();
  const p = prog(frame, 0, 18, EASE.out);
  return <AbsoluteFill style={{ opacity: p, transform: `scale(${lerp(0.94, 1, p)})`, filter: p < 1 ? `blur(${(1 - p) * 10}px)` : undefined }}>{children}</AbsoluteFill>;
};

