// Render many review stills from one bundle (much faster than `remotion still` per frame).
//   node scripts/keyframes.mjs OUT_DIR 120 240 360          explicit frames
//   node scripts/keyframes.mjs OUT_DIR --every 30 [--from 0 --to 6191] [--scale 0.5]
import path from "node:path";
import fs from "node:fs";
import url from "node:url";
import { bundle } from "@remotion/bundler";
import { openBrowser, renderStill, selectComposition } from "@remotion/renderer";

const here = path.dirname(url.fileURLToPath(import.meta.url));
const root = path.join(here, "..");
const args = process.argv.slice(2);
const out = path.resolve(args.shift());
const opt = (n, d) => (args.includes(n) ? Number(args[args.indexOf(n) + 1]) : d);
fs.mkdirSync(out, { recursive: true });

const serveUrl = await bundle({ entryPoint: path.join(root, "src", "index.ts"), publicDir: path.join(root, "public") });
const composition = await selectComposition({ serveUrl, id: "Film" });
let frames = args.filter((a, i) => /^\d+$/.test(a) && !["--every", "--from", "--to", "--scale"].includes(args[i - 1])).map(Number);
if (args.includes("--every")) {
  const every = opt("--every", 30);
  frames = [];
  for (let f = opt("--from", 0); f <= Math.min(opt("--to", composition.durationInFrames - 1), composition.durationInFrames - 1); f += every) frames.push(f);
}
const scale = opt("--scale", 1);
const puppeteerInstance = await openBrowser("chrome");
for (const f of frames) {
  const file = path.join(out, `f${String(f).padStart(5, "0")}.jpg`);
  await renderStill({ composition, serveUrl, output: file, frame: f, imageFormat: "jpeg", jpegQuality: 88, scale, puppeteerInstance });
  process.stdout.write(`${f} `);
}
process.stdout.write("\n");
await puppeteerInstance.close({ silent: true });
