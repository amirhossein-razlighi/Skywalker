// Prints src/footage.ts (the footage manifest) as JSON, so shell/Python tools can use the same render list.
//   node scripts/manifest.mjs > manifest.json
import fs from "node:fs";
import path from "node:path";
import url from "node:url";
import ts from "typescript";

const here = path.dirname(url.fileURLToPath(import.meta.url));
const src = fs.readFileSync(path.join(here, "..", "src", "footage.ts"), "utf8");
const js = ts.transpileModule(src, { compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 } }).outputText;
const mod = { exports: {} };
new Function("module", "exports", "require", js)(mod, mod.exports, () => ({}));
process.stdout.write(JSON.stringify(mod.exports.FOOTAGE, null, 2) + "\n");
