import data from "./timeline.json";

export const FPS = data.fps;
export const BEAT = (60 / data.bpm) * FPS; // 18 frames
export const BAR = BEAT * data.beatsPerBar; // 72 frames

export type SectionId = (typeof data.sections)[number]["id"];

export const bars = (n: number) => Math.round(n * BAR);
export const beats = (n: number) => Math.round(n * BEAT);

export const SECTIONS = data.sections.map((s) => ({
  ...s,
  from: bars(s.startBar),
  duration: bars(s.bars),
}));

export const section = (id: string) => {
  const s = SECTIONS.find((x) => x.id === id);
  if (!s) throw new Error(`unknown section ${id}`);
  return s;
};

export const FILM_FRAMES = SECTIONS.reduce((m, s) => Math.max(m, s.from + s.duration), 0);
