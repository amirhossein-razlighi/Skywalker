import React from "react";
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
  { t: "✓ 13 skills    agent loop, world building, look dev, Blender…", c: C.green },
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

/**
 * The Blender bridge, shown on real footage: the Ashen Peaks monastery kit was modelled by an agent in headless
 * Blender (dcc_run_script) and imported as glTF. The orbit plays in clay, then the lit final wipes across it.
 */
const Blender: React.FC = () => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();
  const exit = prog(frame, 176, 190, EASE.in);
  const card = sp(frame, fps, 8, SPRING.gentle);
  const wipe = prog(frame, 92, 132, EASE.inOut);
  const wpos = lerp(-12, 112, wipe);
  const mask = `linear-gradient(100deg, black ${wpos - 10}%, transparent ${wpos}%)`;
  const label = wipe < 0.5 ? "Clay" : "Lit";
  return (
    <Backdrop y={60}>
      <AbsoluteFill style={{ opacity: 1 - exit }}>
        <div style={{ position: "absolute", left: 96, top: 92 }}>
          <Kicker text="DCC bridge" at={0} color={C.sunset} />
          <Words text="Round-trip with Blender." at={6} size={64} weight={700} align="left" />
        </div>
        <div style={{ position: "absolute", left: 96, top: 300, width: 600 }}>
          <ToolCall
            tool="dcc_run_script"
            args={'{"script": "monastery.py", "out_dir": "dcc/monastery"}'}
            at={14}
            actor="agent:Pixel"
            result="Monastery kit → glTF, imported"
            width={600}
          />
          <div style={{ marginTop: 30, display: "flex", flexWrap: "wrap", gap: 12, width: 600 }}>
            {["FBX", "OBJ", "USD", "Alembic", ".blend", "→ glTF"].map((f, i) => (
              <div key={f} style={{ opacity: prog(frame, 60 + i * 5, 72 + i * 5) }}>
                <Chip color={i === 5 ? C.sunset : C.sky} mono>{f}</Chip>
              </div>
            ))}
          </div>
          <div style={{ marginTop: 26, fontFamily: FONT.body, fontSize: 22, color: C.dim, lineHeight: 1.5, opacity: prog(frame, 90, 110) }}>
            Headless Blender scripts and a live-session add-on. This pagoda was modelled that way, by an agent.
          </div>
        </div>
        {/* the footage card: clay orbit, then the lit final wipes across it */}
        <div
          style={{
            position: "absolute",
            left: 776,
            top: 286,
            width: 1048,
            height: 590,
            borderRadius: 18,
            overflow: "hidden",
            boxShadow: "0 40px 100px rgba(0,0,0,0.55), 0 0 0 1px rgba(255,255,255,0.08)",
            opacity: card,
            transform: `translateY(${(1 - card) * 40}px) scale(${0.96 + 0.04 * card})`,
          }}
        >
          <Footage slot="tools_blender_clay" placeholderLabel={false} />
          <AbsoluteFill style={{ WebkitMaskImage: mask, maskImage: mask }}>
            <Footage slot="tools_blender" placeholderLabel={false} />
          </AbsoluteFill>
          {wipe > 0 && wipe < 1 && (
            <AbsoluteFill
              style={{
                background: `linear-gradient(100deg, transparent ${wpos - 12}%, rgba(255,214,170,${0.4 * Math.sin(Math.PI * wipe)}) ${wpos - 2}%, transparent ${wpos + 1}%)`,
                mixBlendMode: "screen",
              }}
            />
          )}
          <div
            style={{
              position: "absolute",
              left: 18,
              top: 16,
              padding: "6px 12px",
              borderRadius: 8,
              background: "rgba(8,10,20,0.6)",
              backdropFilter: "blur(8px)",
              fontFamily: FONT.mono,
              fontSize: 17,
              color: "#fff",
            }}
          >
            {label} · Ashen Peaks
          </div>
        </div>
      </AbsoluteFill>
    </Backdrop>
  );
};

const BUILD = "skywalker build --project examples/gloamwater --out dist --release";

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
                <span style={{ color: C.dim, fontSize: 19 }}>{bar < 1 ? "packaging scenes, assets, behaviors…" : <span style={{ color: C.green }}>✓ Gloamwater.app</span>}</span>
              </div>
            )}
          </div>
        </Window>
        <div style={{ marginTop: 26, display: "flex", gap: 12, opacity: prog(frame, cmdDone + 50, cmdDone + 64) }}>
          {["Standalone player", "Gamepads", "MetalFX", "Native full screen"].map((c, i) => (
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
        <Footage slot="tools_app" offset={12} playAt={cmdDone + 52} moveVideo duration={200} kb={{ from: [0, 0, 1.6 - 0.6 * launch], to: [0, 0, 1.6 - 0.6 * launch] }} placeholderLabel={false} />
        <div style={{ position: "absolute", inset: 0, background: "linear-gradient(160deg, rgba(255,255,255,0.25), transparent 40%)", opacity: 1 - launch }} />
      </div>
      <div style={{ position: "absolute", left: 1330, top: 700, width: 380, textAlign: "center", fontFamily: FONT.body, fontWeight: 600, fontSize: 26, color: C.text, opacity: icon * (1 - launch) }}>
        Gloamwater
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

