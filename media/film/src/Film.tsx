import React from "react";
import { AbsoluteFill, Audio, getStaticFiles, Sequence, staticFile } from "remotion";
import { Grain, Vignette } from "./components/Overlays";
import { S1Open } from "./sections/S1Open";
import { FILM_FRAMES, section } from "./timeline";

export { FILM_FRAMES };

const SECTIONS: { id: string; C: React.FC }[] = [{ id: "open", C: S1Open }];

export const Film: React.FC = () => {
  const hasScore = getStaticFiles().some((f) => f.name === "audio/score.wav");
  return (
    <AbsoluteFill style={{ background: "#000" }}>
      {SECTIONS.map(({ id, C }) => {
        const s = section(id);
        return (
          <Sequence key={id} from={s.from} durationInFrames={s.duration} name={s.title}>
            <C />
          </Sequence>
        );
      })}
      <Vignette strength={0.42} />
      <Grain opacity={0.06} />
      {hasScore && <Audio src={staticFile("audio/score.wav")} />}
    </AbsoluteFill>
  );
};
