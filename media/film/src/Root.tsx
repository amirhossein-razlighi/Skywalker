import React from "react";
import { Composition } from "remotion";
import { Film, FILM_FRAMES } from "./Film";
import { FPS } from "./timeline";

export const Root: React.FC = () => (
  <Composition id="Film" component={Film} durationInFrames={FILM_FRAMES} fps={FPS} width={1920} height={1080} />
);
