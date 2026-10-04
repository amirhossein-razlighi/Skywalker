import React from "react";
import { AbsoluteFill, Audio, getStaticFiles, Sequence, staticFile } from "remotion";
import { Grain, Vignette } from "./components/Overlays";
import { S1Open } from "./sections/S1Open";
import { S2Problem } from "./sections/S2Problem";
import { S3Idea } from "./sections/S3Idea";
import { S4Studio } from "./sections/S4Studio";
import { S5Wander } from "./sections/S5Wander";
import { S6Render } from "./sections/S6Render";
import { S7Variety } from "./sections/S7Variety";
import { S8Tools } from "./sections/S8Tools";
import { S9Finale } from "./sections/S9Finale";
import { FILM_FRAMES, section } from "./timeline";

export { FILM_FRAMES };

const SECTIONS: { id: string; C: React.FC }[] = [
  { id: "open", C: S1Open },
  { id: "problem", C: S2Problem },
  { id: "idea", C: S3Idea },
  { id: "studio", C: S4Studio },
  { id: "wander", C: S5Wander },
  { id: "render", C: S6Render },
  { id: "variety", C: S7Variety },
  { id: "tools", C: S8Tools },
  { id: "finale", C: S9Finale },
];


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

