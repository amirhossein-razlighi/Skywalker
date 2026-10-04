import { AbsoluteFill, interpolate, spring, useCurrentFrame, useVideoConfig } from "remotion";
import { loadFont } from "@remotion/google-fonts/Inter";

const { fontFamily } = loadFont();
export const FILM_FRAMES = 60;

export const Film: React.FC = () => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();
  const s = spring({ frame, fps, config: { damping: 200 } });
  return (
    <AbsoluteFill style={{ background: "#000", alignItems: "center", justifyContent: "center" }}>
      <div style={{ fontFamily, fontWeight: 600, fontSize: 120, color: "white", letterSpacing: -4,
                    opacity: s, transform: `translateY(${interpolate(s, [0, 1], [40, 0])}px)` }}>
        Skywalker
      </div>
    </AbsoluteFill>
  );
};
