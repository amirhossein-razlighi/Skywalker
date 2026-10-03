import { Composition } from "remotion";
import { Film, FILM_FRAMES } from "./Film";

export const Root: React.FC = () => (
  <Composition id="Film" component={Film} durationInFrames={FILM_FRAMES} fps={30} width={1920} height={1080} />
);
