import React, { useEffect, useState } from "react";
import { continueRender, delayRender, getStaticFiles, interpolate, staticFile, useCurrentFrame, useVideoConfig } from "remotion";
import { C, EASE, FONT, SPRING, prog, sp } from "../theme";

/** Fetch a JSON file from public/ (holds the render until it arrives). Returns null if missing. */
export function usePublicJson<T>(name: string): T | null {
  const exists = getStaticFiles().some((f) => f.name === name);
  const [data, setData] = useState<T | null>(null);
  const [handle] = useState(() => (exists ? delayRender(name) : null));
  useEffect(() => {
    if (!exists) return;
    fetch(staticFile(name))
      .then((r) => r.json())
      .then((j) => {
        setData(j);
        if (handle !== null) continueRender(handle);
      })
      .catch(() => handle !== null && continueRender(handle));
  }, [exists, handle, name]);
  return data;
}

/** macOS-style window chrome around recreated UI. */
export const Window: React.FC<{
  title?: string;
  width: number;
  height: number;
  children?: React.ReactNode;
  style?: React.CSSProperties;
  bar?: number;
  tint?: string;
}> = ({ title, width, height, children, style, bar = 40, tint = C.window }) => (
  <div
    style={{
      width,
      height,
      borderRadius: 16,
      overflow: "hidden",
      background: tint,
      boxShadow: "0 40px 120px rgba(0,0,0,0.6), 0 0 0 1px rgba(255,255,255,0.07) inset, 0 0 0 1px rgba(0,0,0,0.6)",
      position: "relative",
      ...style,
    }}
  >
    <div
      style={{
        height: bar,
        display: "flex",
        alignItems: "center",
        padding: "0 16px",
        gap: 9,
        background: "linear-gradient(#202127, #1a1b20)",
        borderBottom: "1px solid rgba(255,255,255,0.06)",
      }}
    >
      {["#ff5f57", "#febc2e", "#28c840"].map((c) => (
        <div key={c} style={{ width: 13, height: 13, borderRadius: 7, background: c, opacity: 0.9 }} />
      ))}
      <div
        style={{
          flex: 1,
          textAlign: "center",
          marginRight: 60,
          fontFamily: FONT.body,
          fontWeight: 500,
          fontSize: 15,
          color: "rgba(227,228,232,0.7)",
        }}
      >
        {title}
      </div>
    </div>
    <div style={{ position: "absolute", top: bar, left: 0, right: 0, bottom: 0 }}>{children}</div>
  </div>
);

/** Typewriter: how many characters of `text` are visible at this frame. */
export const typed = (frame: number, at: number, text: string, cps = 38, fps = 30) =>
  text.slice(0, Math.max(0, Math.floor(((frame - at) / fps) * cps)));

export const Caret: React.FC<{ color?: string; h?: number }> = ({ color = C.sky, h = 26 }) => {
  const frame = useCurrentFrame();
  return <span style={{ display: "inline-block", width: 2.5, height: h, background: color, marginLeft: 2, verticalAlign: "middle", opacity: Math.floor(frame / 15) % 2 ? 0.2 : 1 }} />;
};

/* ---------- syntax colouring ---------- */
const WANDER_KW = new Set([
  "behavior", "intent", "param", "in", "on", "end", "with", "by", "state", "go", "to", "if", "else", "test", "expect", "wait", "and", "or", "not", "at", "toward",
]);
const WANDER_FN = new Set(["rotate", "emit", "destroy", "move", "self", "dt", "spin", "burst", "log"]);
const CPP_KW = new Set(["static", "void", "const", "float", "double", "int", "auto", "return", "if", "struct", "extern", "namespace", "using", "for", "constexpr", "inline"]);

export const codeColors = {
  kw: "#c792ff",
  fn: "#7fb4ff",
  str: "#ffc58a",
  num: "#f78c8c",
  comment: "rgba(200,205,230,0.42)",
  text: "#e3e6f3",
  punct: "rgba(220,225,245,0.6)",
};

export const tokenize = (line: string, lang: "wander" | "cpp" | "json"): { t: string; c: string }[] => {
  const out: { t: string; c: string }[] = [];
  const re = /(\/\/.*$|#.*$|"[^"]*"?|\d+(?:\.\d+)?f?|[A-Za-z_][A-Za-z0-9_:]*|\s+|.)/g;
  let m: RegExpExecArray | null;
  while ((m = re.exec(line))) {
    const t = m[0];
    let c = codeColors.text;
    if (t.startsWith("//") || (lang === "wander" && t.startsWith("#"))) c = codeColors.comment;
    else if (t.startsWith('"')) c = lang === "json" ? codeColors.str : codeColors.str;
    else if (/^\d/.test(t)) c = codeColors.num;
    else if (lang === "wander" && WANDER_KW.has(t)) c = codeColors.kw;
    else if (lang === "wander" && WANDER_FN.has(t)) c = codeColors.fn;
    else if (lang === "cpp" && CPP_KW.has(t)) c = codeColors.kw;
    else if (lang === "cpp" && /^(sky|sk_|std)/.test(t)) c = codeColors.fn;
    else if (/^[{}()[\],:;.=+*<>-]$/.test(t)) c = codeColors.punct;
    out.push({ t, c });
  }
  return out;
};

export const CodeLine: React.FC<{ line: string; lang: "wander" | "cpp" | "json"; chars?: number }> = ({ line, lang, chars }) => {
  const toks = tokenize(line, lang);
  let left = chars ?? Infinity;
  return (
    <>
      {toks.map((k, i) => {
        if (left <= 0) return null;
        const t = k.t.slice(0, left);
        left -= k.t.length;
        return (
          <span key={i} style={{ color: k.c, whiteSpace: "pre" }}>
            {t}
          </span>
        );
      })}
    </>
  );
};

/** A tool call card: "▸ tool_name" + JSON args that type out, then a result line. */
export const ToolCall: React.FC<{
  tool: string;
  args: string;
  at: number;
  result?: string;
  actor?: string;
  width?: number;
  scale?: number;
  style?: React.CSSProperties;
}> = ({ tool, args, at, result, actor = "agent", width = 640, scale = 1, style }) => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();
  const s = sp(frame, fps, at, SPRING.snappy);
  const argText = typed(frame, at + 10, args, 60);
  const done = at + 10 + (args.length / 60) * fps;
  const r = prog(frame, done + 6, done + 22);
  return (
    <div
      style={{
        width,
        transform: `translateY(${(1 - s) * 30}px) scale(${scale * (0.96 + 0.04 * s)})`,
        transformOrigin: "top left",
        opacity: interpolate(s, [0, 0.5], [0, 1], { extrapolateRight: "clamp" }),
        background: "rgba(20,22,34,0.86)",
        backdropFilter: "blur(18px)",
        border: "1px solid rgba(154,134,255,0.28)",
        borderRadius: 14,
        padding: "16px 20px",
        fontFamily: FONT.mono,
        fontSize: 21,
        color: C.uiText,
        boxShadow: "0 24px 70px rgba(0,0,0,0.45)",
        ...style,
      }}
    >
      <div style={{ display: "flex", alignItems: "center", gap: 10, marginBottom: 10 }}>
        <span style={{ fontFamily: FONT.body, fontSize: 14, fontWeight: 600, letterSpacing: 1.2, color: C.violet, textTransform: "uppercase" }}>{actor}</span>
        <span style={{ color: C.faint }}>·</span>
        <span style={{ color: "#7fb4ff", fontWeight: 700 }}>{tool}</span>
      </div>
      <div style={{ lineHeight: 1.45, minHeight: 30, whiteSpace: "pre-wrap" }}>
        <CodeLine line={argText} lang="json" />
        {frame < done + 4 && <Caret h={22} />}
      </div>
      {result && (
        <div style={{ marginTop: 10, opacity: r, transform: `translateY(${(1 - r) * 6}px)`, color: C.green, fontSize: 18 }}>
          ✓ <span style={{ color: "rgba(227,228,232,0.75)" }}>{result}</span>
        </div>
      )}
    </div>
  );
};

/** Mouse pointer. */
export const Cursor: React.FC<{ x: number; y: number; scale?: number; opacity?: number; click?: number }> = ({ x, y, scale = 1, opacity = 1, click = 0 }) => (
  <div style={{ position: "absolute", left: x, top: y, opacity, pointerEvents: "none" }}>
    {click > 0 && click < 1 && (
      <div
        style={{
          position: "absolute",
          left: -30 * click,
          top: -30 * click,
          width: 60 * click,
          height: 60 * click,
          borderRadius: "50%",
          border: `2px solid rgba(255,255,255,${0.7 * (1 - click)})`,
        }}
      />
    )}
    <svg width={30 * scale} height={42 * scale} viewBox="0 0 30 42" style={{ filter: "drop-shadow(0 3px 6px rgba(0,0,0,0.5))" }}>
      <path d="M2 2 L2 33 L10 25.5 L15.5 38.5 L21 36 L15.6 23.4 L26 23.4 Z" fill="#fff" stroke="#000" strokeWidth={2} strokeLinejoin="round" />
    </svg>
  </div>
);

/** A Cloudling: the crew's cloud avatar (bare, colourful variants of the logo character). */
export const Cloudling: React.FC<{
  size: number;
  color: string;
  face?: "happy" | "focused" | "curious" | "wink" | "determined" | "dreamy";
  blink?: boolean;
  glasses?: boolean;
  style?: React.CSSProperties;
}> = ({ size, color, face = "happy", blink, glasses, style }) => {
  const frame = useCurrentFrame();
  const closed = blink && frame % 97 < 4;
  const id = `cl${color.replace("#", "")}`;
  const eyeY = 58;
  return (
    <svg width={size} height={size * 0.78} viewBox="0 0 120 94" style={{ overflow: "visible", ...style }}>
      <defs>
        <linearGradient id={id} x1="0" y1="0" x2="0" y2="1">
          <stop offset="0" stopColor="#ffffff" stopOpacity={0.95} />
          <stop offset="0.35" stopColor={color} stopOpacity={0.55} />
          <stop offset="1" stopColor={color} />
        </linearGradient>
      </defs>
      <path
        d="M24 88 C9 88 2 78 2 66 C2 53 12 45 24 46 C24 28 38 16 55 16 C70 16 81 25 85 37 C88 36 91 35 95 35 C109 35 118 47 118 61 C118 77 107 88 92 88 Z"
        fill={`url(#${id})`}
      />
      <path d="M24 88 C9 88 2 78 2 66 C2 53 12 45 24 46" fill="none" stroke="rgba(255,255,255,0.5)" strokeWidth={1.5} />
      {glasses ? (
        <g>
          <path d="M32 54 H56 Q56 70 44 70 Q32 70 32 54 Z M64 54 H88 Q88 70 76 70 Q64 70 64 54 Z" fill="#2a1d3a" stroke="#e2b062" strokeWidth={2} />
          <path d="M56 56 H64" stroke="#e2b062" strokeWidth={2} />
        </g>
      ) : closed ? (
        <g stroke="#1b1d2b" strokeWidth={3} strokeLinecap="round">
          <path d={`M40 ${eyeY} h8`} />
          <path d={`M72 ${eyeY} h8`} />
        </g>
      ) : (
        <g fill="#1b1d2b">
          {face === "wink" ? <path d={`M39 ${eyeY} q5 -4 10 0`} stroke="#1b1d2b" strokeWidth={3} fill="none" strokeLinecap="round" /> : <ellipse cx={44} cy={eyeY} rx={4.2} ry={face === "focused" ? 3.2 : 5} />}
          <ellipse cx={76} cy={eyeY} rx={4.2} ry={face === "focused" ? 3.2 : 5} />
          {face === "dreamy" && <path d={`M38 ${eyeY - 9} l10 2 M82 ${eyeY - 9} l-10 2`} stroke="#1b1d2b" strokeWidth={2} />}
          {face === "determined" && <path d={`M37 ${eyeY - 10} l12 4 M83 ${eyeY - 10} l-12 4`} stroke="#1b1d2b" strokeWidth={2.6} strokeLinecap="round" />}
        </g>
      )}
      <circle cx={34} cy={70} r={5} fill="#ed6b7a" opacity={0.45} />
      <circle cx={86} cy={70} r={5} fill="#ed6b7a" opacity={0.45} />
      {face === "curious" ? (
        <ellipse cx={60} cy={73} rx={4} ry={4.5} fill="#1b1d2b" />
      ) : (
        <path d="M52 70 Q60 78 68 70" fill="none" stroke="#1b1d2b" strokeWidth={3} strokeLinecap="round" />
      )}
    </svg>
  );
};

/** Pill label. */
export const Chip: React.FC<{ children: React.ReactNode; color?: string; style?: React.CSSProperties; mono?: boolean }> = ({
  children,
  color = C.sky,
  style,
  mono,
}) => (
  <div
    style={{
      display: "inline-flex",
      alignItems: "center",
      gap: 8,
      padding: "7px 14px",
      borderRadius: 999,
      background: `${color}22`,
      border: `1px solid ${color}55`,
      color,
      fontFamily: mono ? FONT.mono : FONT.body,
      fontWeight: 600,
      fontSize: 18,
      letterSpacing: mono ? 0 : 0.2,
      whiteSpace: "nowrap",
      ...style,
    }}
  >
    {children}
  </div>
);

/** Fade + rise helper for UI elements. */
export const useRise = (at: number, cfg = SPRING.snappy) => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();
  const s = sp(frame, fps, at, cfg);
  return { s, style: { opacity: interpolate(s, [0, 0.6], [0, 1], { extrapolateRight: "clamp" }), transform: `translateY(${(1 - s) * 24}px)` } as React.CSSProperties };
};

export const fadeOut = (frame: number, at: number, d = 14) => 1 - prog(frame, at, at + d, EASE.in);
