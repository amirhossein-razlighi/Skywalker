#!/usr/bin/env python3
"""Pinned asset manifests: reproducible, license-checked downloads for projects and asset kits.

A manifest (`assets.json`, next to a project or kit) lists every third-party file the project
needs, each pinned by URL and sha256 with its license, author and source page. Heavy files stay out
of git: `fetch` downloads them into the manifest's `root` folder (gitignored `downloads/` by
default), in parallel, resuming partial downloads, verifying every hash, unpacking archives.

    python3 media/demo/assetkit.py fetch  path/to/assets.json [--only ID,ID] [--jobs 6]
    python3 media/demo/assetkit.py fetch  path/to/project       # its assets.json and those of the kits it mounts
    python3 media/demo/assetkit.py pin    path/to/assets.json      # resolve + hash entries added without sha256
    python3 media/demo/assetkit.py check  path/to/assets.json      # licenses in the allowlist, every entry pinned
    python3 media/demo/assetkit.py credits path/to/assets.json [-o CREDITS.md]
    python3 media/demo/assetkit.py add-polyhaven path/to/assets.json model wooden_crate_01 --res 2k
    python3 media/demo/assetkit.py add-ambientcg path/to/assets.json Bricks090 --res 2K

Manifest format (JSON, `format: skywalker.assets`):

    {"format": "skywalker.assets", "version": 1, "root": "downloads",
     "assets": [
       {"id": "ual1", "title": "Universal Animation Library (Standard)",
        "license": "CC0-1.0", "author": "Quaternius", "source_page": "https://quaternius.com/...",
        "source": {"kind": "itch", "page": "https://quaternius.itch.io/universal-animation-library",
                   "file": "Universal Animation Library[Standard].zip"},
        "sha256": "...", "size": 15904933, "path": "quaternius/ual1",
        "extract": {"include": ["*_Standard*.glb", "*/License.txt"], "exclude": ["*.fbx"], "strip": 1}},
       {"id": "polyhaven/wooden_crate_01", "license": "CC0-1.0", "author": "...", "source_page": "https://polyhaven.com/a/wooden_crate_01",
        "polyhaven": {"type": "model", "id": "wooden_crate_01", "res": "2k"},
        "files": [{"url": "https://dl.polyhaven.org/...", "path": "polyhaven/wooden_crate_01/wooden_crate_01.gltf", "sha256": "...", "size": 5612}, ...]}
     ]}

Entry kinds:
  * single file or archive: `url` (or `source` for sites that hand out signed links: itch.io free
    downloads), `sha256`, `size`, `path` (a file, or the folder an archive unpacks into) and optional
    `extract` {include: [globs], exclude: [globs], strip: N leading folders};
  * multi-file: `files` [{url, path, sha256, size}] (Poly Haven models and texture sets);
  * `polyhaven` / `ambientcg` blocks record how the entry was resolved, so `pin --refresh` can redo it.

Licenses must be in the allowlist (CC0, CC-BY, MIT, Apache, OFL, public domain); NC / ND / SA and
unknown licenses are refused. `credits` writes the attribution file every project ships.
Only standard library modules are used (Pillow is optional, for packing ORM maps).
"""
import argparse
import concurrent.futures
import fnmatch
import hashlib
import http.cookiejar
import json
import os
import re
import shutil
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

UA = "Skywalker-Engine-AssetKit/0.1 (+https://github.com/amirhossein-razlighi)"
FORMAT = "skywalker.assets"
ALLOWED_LICENSES = {
    "CC0-1.0": "Creative Commons Zero (public domain dedication)",
    "CC-BY-4.0": "Creative Commons Attribution 4.0",
    "CC-BY-3.0": "Creative Commons Attribution 3.0",
    "MIT": "MIT",
    "Apache-2.0": "Apache 2.0",
    "OFL-1.1": "SIL Open Font License 1.1",
    "Public-Domain": "Public domain",
}
REFUSED = re.compile(r"\b(NC|ND|SA)\b|non-?commercial|no-?deriv|share-?alike|all rights reserved|unknown", re.I)
_print_lock = threading.Lock()


def log(*a):
    with _print_lock:
        print(*a, flush=True)


# ---------------------------------------------------------------------------------------------
# Manifest
# ---------------------------------------------------------------------------------------------
class Manifest:
    def __init__(self, path, data=None):
        self.path = os.path.abspath(path)
        self.dir = os.path.dirname(self.path)
        self.data = data if data is not None else {"format": FORMAT, "version": 1, "root": "downloads", "assets": []}

    @classmethod
    def load(cls, path):
        with open(path) as f:
            data = json.load(f)
        if data.get("format") != FORMAT:
            raise ValueError(f"{path}: not a {FORMAT} manifest (format: {data.get('format')!r})")
        return cls(path, data)

    def save(self):
        with open(self.path, "w") as f:
            json.dump(self.data, f, indent=2)
            f.write("\n")

    @property
    def assets(self):
        return self.data.setdefault("assets", [])

    @property
    def root(self):
        return os.path.normpath(os.path.join(self.dir, self.data.get("root", "downloads")))

    def get(self, asset_id):
        for a in self.assets:
            if a["id"] == asset_id:
                return a
        return None

    def put(self, entry):
        """Adds or replaces an entry (by id)."""
        for i, a in enumerate(self.assets):
            if a["id"] == entry["id"]:
                self.assets[i] = entry
                return entry
        self.assets.append(entry)
        return entry

    def local(self, rel):
        p = os.path.normpath(os.path.join(self.root, rel))
        if not (p == self.root or p.startswith(self.root + os.sep)):
            raise ValueError(f"path escapes the download root: {rel}")
        return p


def license_problem(entry):
    lic = entry.get("license", "")
    if not lic:
        return "no license"
    if REFUSED.search(lic):
        return f"license {lic!r} is not allowed (NC/ND/SA/unknown)"
    if lic not in ALLOWED_LICENSES:
        return f"license {lic!r} is not in the allowlist ({', '.join(ALLOWED_LICENSES)})"
    if lic.startswith("CC-BY") and not entry.get("author"):
        return "CC-BY needs an author for attribution"
    return None


def check(manifest, require_pinned=True):
    """Returns a list of problems (empty = fine)."""
    problems = []
    seen = set()
    for a in manifest.assets:
        aid = a.get("id", "?")
        if aid in seen:
            problems.append(f"{aid}: duplicate id")
        seen.add(aid)
        if p := license_problem(a):
            problems.append(f"{aid}: {p}")
        if not a.get("source_page"):
            problems.append(f"{aid}: no source_page (where the license was verified)")
        if "files" in a:
            for f in a["files"]:
                if require_pinned and not f.get("sha256"):
                    problems.append(f"{aid}: {f.get('path')} not pinned (run pin)")
                if not f.get("url") or not f.get("path"):
                    problems.append(f"{aid}: file entry needs url and path")
        else:
            if not a.get("url") and not a.get("source"):
                problems.append(f"{aid}: needs url, source or files")
            if not a.get("path"):
                problems.append(f"{aid}: needs path")
            if require_pinned and not a.get("sha256"):
                problems.append(f"{aid}: not pinned (run pin)")
    return problems


# ---------------------------------------------------------------------------------------------
# Downloading
# ---------------------------------------------------------------------------------------------
def _request(url, headers=None, data=None, opener=None):
    req = urllib.request.Request(url, data=data, headers={"User-Agent": UA, **(headers or {})})
    return (opener or urllib.request.build_opener()).open(req, timeout=120)


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def resolve_itch(page, filename):
    """A short-lived download link for a free (pay-what-you-want, $0 minimum) itch.io upload: the same
    steps as the page's "No thanks, just take me to the downloads" button. No login, no payment."""
    jar = http.cookiejar.CookieJar()
    opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
    ua = {"User-Agent": "Mozilla/5.0 (Skywalker AssetKit)"}
    html = opener.open(urllib.request.Request(page, headers=ua), timeout=60).read().decode("utf-8", "replace")
    m = re.search(r'name="csrf_token" value="([^"]+)"', html)
    if not m:
        raise RuntimeError(f"{page}: no download form (is it a free download?)")
    token = m.group(1)
    price = re.search(r'"min_price":(\d+)', html)
    if price and int(price.group(1)) > 0:
        raise RuntimeError(f"{page}: not a free download (minimum price {int(price.group(1)) / 100:.2f})")
    body = urllib.parse.urlencode({"csrf_token": token}).encode()
    r = json.load(opener.open(urllib.request.Request(page + "/download_url", data=body, headers=ua), timeout=60))
    dl_page = opener.open(urllib.request.Request(r["url"], headers=ua), timeout=60).read().decode("utf-8", "replace")
    uploads = re.findall(r'data-upload_id="(\d+)".*?class="name"[^>]*>([^<]+)<', dl_page, re.S)
    names = [n for _, n in uploads]
    for uid, name in uploads:
        if name.strip() == filename:
            r = json.load(opener.open(urllib.request.Request(
                f"{page}/file/{uid}?source=game_download&after_download_lightbox=1&as_props=1", data=body, headers=ua), timeout=60))
            if "url" not in r:
                raise RuntimeError(f"{page}: {r}")
            return r["url"]
    raise RuntimeError(f"{page}: no upload named {filename!r} (available: {names})")


def download(url, dest, expected_sha=None, expected_size=None, retries=3):
    """Downloads url to dest (resuming dest.part), verifies the hash. Returns the sha256."""
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    part = dest + ".part"
    for attempt in range(retries):
        try:
            have = os.path.getsize(part) if os.path.exists(part) else 0
            headers = {"Range": f"bytes={have}-"} if have else {}
            try:
                resp = _request(url, headers)
            except urllib.error.HTTPError as e:
                if e.code == 416:  # already complete
                    resp, have = None, have
                else:
                    raise
            if resp is not None:
                if have and resp.status != 206:  # server ignored the range: start over
                    have = 0
                with open(part, "ab" if have else "wb") as f:
                    shutil.copyfileobj(resp, f, 1 << 20)
            digest = sha256_of(part)
            if expected_size and os.path.getsize(part) != expected_size and not expected_sha:
                raise RuntimeError(f"size {os.path.getsize(part)} != {expected_size}")
            if expected_sha and digest != expected_sha:
                os.remove(part)
                raise RuntimeError(f"sha256 mismatch for {os.path.basename(dest)}: got {digest}, pinned {expected_sha}")
            os.replace(part, dest)
            return digest
        except Exception as e:  # noqa: BLE001 - network errors of every kind are retried
            if attempt == retries - 1:
                raise
            log(f"  retry {attempt + 1}/{retries - 1} {os.path.basename(dest)}: {e}")
            time.sleep(2 * (attempt + 1))
    return None


def extract(archive, dest, include=None, exclude=None, strip=0):
    """Unpacks a zip into dest with traversal guards, glob filters and leading-folder stripping."""
    os.makedirs(dest, exist_ok=True)
    root = os.path.realpath(dest)
    count = 0
    with zipfile.ZipFile(archive) as z:
        total = sum(i.file_size for i in z.infolist())
        if total > 8 << 30:
            raise RuntimeError(f"{archive}: {total >> 20} MB unpacked is too large")
        for info in z.infolist():
            name = info.filename.replace("\\", "/")
            if info.is_dir():
                continue
            if include and not any(fnmatch.fnmatch(name, g) for g in include):
                continue
            if exclude and any(fnmatch.fnmatch(name, g) for g in exclude):
                continue
            parts = [p for p in name.split("/") if p not in ("", ".")]
            if ".." in parts or len(parts) <= strip:
                continue
            target = os.path.realpath(os.path.join(dest, *parts[strip:]))
            if not target.startswith(root + os.sep):
                continue
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with z.open(info) as src, open(target, "wb") as out:
                shutil.copyfileobj(src, out, 1 << 20)
            count += 1
    return count


def _stamp_path(manifest, entry):
    return manifest.local(os.path.join(".stamps", entry["id"].replace("/", "__") + ".json"))


def _is_fetched(manifest, entry):
    stamp = _stamp_path(manifest, entry)
    if not os.path.exists(stamp):
        return False
    try:
        with open(stamp) as f:
            s = json.load(f)
    except (OSError, ValueError):
        return False
    want = entry.get("sha256") or [f.get("sha256") for f in entry.get("files", [])]
    if s.get("sha256") != want or s.get("extract") != entry.get("extract"):
        return False
    paths = [entry["path"]] if "path" in entry else [f["path"] for f in entry.get("files", [])]
    return all(os.path.exists(manifest.local(p)) for p in paths)


def _mark_fetched(manifest, entry):
    stamp = _stamp_path(manifest, entry)
    os.makedirs(os.path.dirname(stamp), exist_ok=True)
    with open(stamp, "w") as f:
        json.dump({"sha256": entry.get("sha256") or [x.get("sha256") for x in entry.get("files", [])],
                   "extract": entry.get("extract"), "time": time.strftime("%Y-%m-%d")}, f)


def _is_archive(entry):
    return bool(entry.get("extract")) or str(entry.get("url", entry.get("source", {}).get("file", ""))).lower().endswith(".zip")


def fetch_entry(manifest, entry, pin=False):
    """Downloads one entry (all its files), verifying hashes. pin=True records missing hashes."""
    if p := license_problem(entry):
        raise RuntimeError(f"{entry['id']}: {p}")
    if not pin and _is_fetched(manifest, entry):
        return "cached"
    if "files" in entry:
        for f in entry["files"]:
            dest = manifest.local(f["path"])
            if os.path.exists(dest) and f.get("sha256") and sha256_of(dest) == f["sha256"]:
                continue
            digest = download(f["url"], dest, None if pin else f.get("sha256"), f.get("size"))
            if pin or not f.get("sha256"):
                f["sha256"], f["size"] = digest, os.path.getsize(dest)
        _mark_fetched(manifest, entry)
        return "downloaded"
    url = entry.get("url")
    if not url and entry.get("source", {}).get("kind") == "itch":
        url = resolve_itch(entry["source"]["page"], entry["source"]["file"])
    if not url:
        raise RuntimeError(f"{entry['id']}: no url")
    if _is_archive(entry):
        with tempfile.TemporaryDirectory(dir=manifest.root if os.path.isdir(manifest.root) else None) as tmp:
            arch = os.path.join(tmp, "archive.zip")
            digest = download(url, arch, None if pin else entry.get("sha256"), entry.get("size"))
            if pin or not entry.get("sha256"):
                entry["sha256"], entry["size"] = digest, os.path.getsize(arch)
            ex = entry.get("extract") or {}
            n = extract(arch, manifest.local(entry["path"]), ex.get("include"), ex.get("exclude"), ex.get("strip", 0))
            if n == 0:
                raise RuntimeError(f"{entry['id']}: the archive's include filters matched nothing")
    else:
        dest = manifest.local(entry["path"])
        digest = download(url, dest, None if pin else entry.get("sha256"), entry.get("size"))
        if pin or not entry.get("sha256"):
            entry["sha256"], entry["size"] = digest, os.path.getsize(dest)
    _mark_fetched(manifest, entry)
    return "downloaded"


def fetch(manifest, only=None, jobs=6, pin=False):
    """Fetches (or pins) every entry, in parallel. Returns {id: status}; raises on the first error after all ran."""
    if not manifest.assets:
        return {}
    os.makedirs(manifest.root, exist_ok=True)
    entries = [a for a in manifest.assets if not only or a["id"] in only or any(fnmatch.fnmatch(a["id"], o) for o in only)]
    if only and not entries:
        raise SystemExit(f"no entries match {only}")
    results, errors = {}, []

    def run(e):
        t0 = time.time()
        try:
            status = fetch_entry(manifest, e, pin=pin)
            results[e["id"]] = status
            if status != "cached":
                log(f"  {status:10s} {e['id']} ({time.time() - t0:.1f}s)")
        except Exception as ex:  # noqa: BLE001
            errors.append(f"{e['id']}: {ex}")
            log(f"  FAILED     {e['id']}: {ex}")

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        list(pool.map(run, entries))
    if pin:
        manifest.save()
    if errors:
        raise RuntimeError("some downloads failed:\n  " + "\n  ".join(errors))
    return results


# ---------------------------------------------------------------------------------------------
# Libraries: Poly Haven, ambientCG
# ---------------------------------------------------------------------------------------------
_api_cache = {}


def _json(url):
    if url not in _api_cache:
        with _request(url) as r:
            _api_cache[url] = json.load(r)
    return _api_cache[url]


def polyhaven_info(asset_id):
    return _json(f"https://api.polyhaven.com/info/{asset_id}")


def _polyhaven_credit(asset_id):
    info = polyhaven_info(asset_id)
    return {"license": "CC0-1.0", "author": ", ".join(info.get("authors", {}).keys()) or "Poly Haven",
            "source_page": f"https://polyhaven.com/a/{asset_id}", "title": info.get("name", asset_id)}


def polyhaven_model(asset_id, res="2k", folder=None):
    """Entry for a Poly Haven glTF model with `res` textures (1k, 2k, 4k, 8k). The engine builds LODs
    for heavy meshes on import; pick the texture resolution here."""
    files = _json(f"https://api.polyhaven.com/files/{asset_id}")
    if "gltf" not in files:
        raise ValueError(f"{asset_id}: no glTF download (is it a model?)")
    if res not in files["gltf"]:
        raise ValueError(f"{asset_id}: resolutions {sorted(files['gltf'])}")
    gl = files["gltf"][res]["gltf"]
    folder = folder or f"polyhaven/{asset_id}"
    out = [{"url": gl["url"], "path": f"{folder}/{gl['url'].rsplit('/', 1)[1]}", "size": gl.get("size")}]
    for rel, inc in sorted(gl.get("include", {}).items()):
        out.append({"url": inc["url"], "path": f"{folder}/{rel}", "size": inc.get("size")})
    # Cut-out foliage: the glTF's JPEG base color has no alpha channel; the separate alpha map
    # comes along so a build can merge it (see merge_alpha).
    alpha = files.get("Alpha", {}).get(res, {}).get("jpg") or files.get("Alpha", {}).get(res, {}).get("png")
    if alpha:
        out.append({"url": alpha["url"], "path": f"{folder}/textures/{alpha['url'].rsplit('/', 1)[1]}", "size": alpha.get("size")})
    return {"id": f"polyhaven/{asset_id}", **_polyhaven_credit(asset_id),
            "polyhaven": {"type": "model", "id": asset_id, "res": res}, "files": out}


def polyhaven_textures(asset_id, res="2k", maps=("Diffuse", "nor_gl", "arm"), fmt="jpg", folder=None):
    """Entry for a Poly Haven texture set (albedo, OpenGL normal, packed AO/rough/metal by default)."""
    files = _json(f"https://api.polyhaven.com/files/{asset_id}")
    folder = folder or f"polyhaven/{asset_id}"
    out = []
    for m in maps:
        if m == "Diffuse" and m not in files:  # color variants instead of one albedo: take the first
            m = next((k for k in sorted(files) if k.startswith("col_")), m)
        if m not in files:
            raise ValueError(f"{asset_id}: no {m} map (has {sorted(files)})")
        f = files[m][res][fmt]
        out.append({"url": f["url"], "path": f"{folder}/{f['url'].rsplit('/', 1)[1]}", "size": f.get("size")})
    return {"id": f"polyhaven/{asset_id}", **_polyhaven_credit(asset_id),
            "polyhaven": {"type": "textures", "id": asset_id, "res": res, "maps": list(maps), "fmt": fmt}, "files": out}


def polyhaven_hdri(asset_id, res="4k", folder="polyhaven/hdri"):
    files = _json(f"https://api.polyhaven.com/files/{asset_id}")
    f = files["hdri"][res]["hdr"]
    return {"id": f"polyhaven/hdri/{asset_id}", **_polyhaven_credit(asset_id),
            "polyhaven": {"type": "hdri", "id": asset_id, "res": res},
            "files": [{"url": f["url"], "path": f"{folder}/{f['url'].rsplit('/', 1)[1]}", "size": f.get("size")}]}


def ambientcg(asset_id, res="2K", fmt="JPG", maps=("Color", "NormalGL", "Roughness", "AmbientOcclusion", "Metalness"), folder=None):
    """Entry for an ambientCG material zip (only the listed maps are unpacked)."""
    j = _json(f"https://ambientcg.com/api/v2/full_json?id={urllib.parse.quote(asset_id)}&include=downloadData")
    if not j.get("foundAssets"):
        raise ValueError(f"ambientCG: no asset {asset_id}")
    a = j["foundAssets"][0]
    want = f"{res}-{fmt}"
    for d in a["downloadFolders"]["default"]["downloadFiletypeCategories"]["zip"]["downloads"]:
        if d["attribute"] == want:
            return {"id": f"ambientcg/{a['assetId']}", "title": a.get("displayName", a["assetId"]), "license": "CC0-1.0",
                    "author": "ambientCG (Lennart Demes)", "source_page": f"https://ambientcg.com/view?id={a['assetId']}",
                    "ambientcg": {"id": a["assetId"], "res": res, "fmt": fmt}, "url": d["downloadLink"], "size": d["size"],
                    "path": folder or f"ambientcg/{a['assetId']}",
                    "extract": {"include": [f"*_{m}.*" for m in maps]}}
    raise ValueError(f"ambientCG {asset_id}: no {want} download")


def merge_alpha(color, alpha, out):
    """Writes a PNG with `color`'s RGB and `alpha`'s luminance as alpha (cut-out foliage; needs Pillow)."""
    from PIL import Image  # optional dependency
    c = Image.open(color).convert("RGB")
    a = Image.open(alpha).convert("L").resize(c.size)
    c.putalpha(a)
    c.save(out)
    return out


def pack_orm(ao, rough, metal, out):
    """Packs separate occlusion / roughness / metalness maps into one glTF-style ORM image (needs Pillow).
    Any input may be None (AO 1, roughness 1, metal 0)."""
    from PIL import Image  # optional dependency
    src = next(p for p in (rough, ao, metal) if p)
    size = Image.open(src).size

    def chan(p, default):
        if not p:
            return Image.new("L", size, default)
        return Image.open(p).convert("L").resize(size)

    Image.merge("RGB", (chan(ao, 255), chan(rough, 255), chan(metal, 0))).save(out, quality=92)
    return out


# ---------------------------------------------------------------------------------------------
# Credits
# ---------------------------------------------------------------------------------------------
def credits_markdown(manifest, title=None, extra=None):
    """CREDITS.md text: every entry with title, author, license and source, grouped by license."""
    lines = [f"# {title or 'Credits'}", "",
             "Third-party assets used by this project. Files are fetched from the pinned manifest "
             f"(`{os.path.basename(manifest.path)}`) and verified by sha256; they are not stored in git.", ""]
    groups = {}
    for a in sorted(manifest.assets, key=lambda a: (a.get("license", ""), a.get("author", ""), a["id"])):
        groups.setdefault(a.get("license", "?"), []).append(a)
    for lic, items in groups.items():
        lines += [f"## {ALLOWED_LICENSES.get(lic, lic)} ({lic})", ""]
        for a in items:
            name = a.get("title") or a["id"]
            lines.append(f"- **{name}** by {a.get('author', 'unknown')} - [{a.get('source_page', '')}]({a.get('source_page', '')})")
        lines.append("")
    for line in extra or []:
        lines.append(line)
    return "\n".join(lines).rstrip() + "\n"


def write_credits(manifest, out=None, title=None, extra=None):
    out = out or os.path.join(manifest.dir, "CREDITS.md")
    with open(out, "w") as f:
        f.write(credits_markdown(manifest, title, extra))
    return out


# ---------------------------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch", help="download and verify")
    f.add_argument("manifest")
    f.add_argument("--only", default="", help="comma-separated ids or globs")
    f.add_argument("--jobs", type=int, default=6)
    p = sub.add_parser("pin", help="hash entries that have no sha256 yet (downloads them)")
    p.add_argument("manifest")
    p.add_argument("--only", default="")
    p.add_argument("--jobs", type=int, default=6)
    c = sub.add_parser("check", help="licenses and pins")
    c.add_argument("manifest")
    c.add_argument("--allow-unpinned", action="store_true")
    cr = sub.add_parser("credits", help="write CREDITS.md")
    cr.add_argument("manifest")
    cr.add_argument("-o", "--out")
    cr.add_argument("--title")
    ph = sub.add_parser("add-polyhaven", help="add a Poly Haven model, texture set or HDRI")
    ph.add_argument("manifest")
    ph.add_argument("type", choices=["model", "textures", "hdri"])
    ph.add_argument("ids", nargs="+")
    ph.add_argument("--res", default=None)
    ac = sub.add_parser("add-ambientcg", help="add an ambientCG material")
    ac.add_argument("manifest")
    ac.add_argument("ids", nargs="+")
    ac.add_argument("--res", default="2K")
    a = ap.parse_args(argv)

    if a.cmd in ("add-polyhaven", "add-ambientcg"):
        m = Manifest.load(a.manifest) if os.path.exists(a.manifest) else Manifest(a.manifest)
        for i in a.ids:
            if a.cmd == "add-ambientcg":
                e = ambientcg(i, a.res)
            elif a.type == "model":
                e = polyhaven_model(i, a.res or "2k")
            elif a.type == "textures":
                e = polyhaven_textures(i, a.res or "2k")
            else:
                e = polyhaven_hdri(i, a.res or "4k")
            m.put(e)
            log(f"added {e['id']}")
        m.save()
        log("run `pin` to download and record the hashes")
        return 0
    project_arg = a.manifest
    a.manifest = resolve_manifest_path(a.manifest)
    if not os.path.exists(a.manifest) and a.cmd == "fetch" and mounted_manifests(project_arg):
        m = Manifest(a.manifest)  # a project without its own downloads that only mounts a kit
    else:
        m = Manifest.load(a.manifest)
    a.manifest = project_arg
    if a.cmd == "check":
        problems = check(m, require_pinned=not a.allow_unpinned)
        for pr in problems:
            log("  " + pr)
        log(f"{len(m.assets)} entries, {len(problems)} problem(s)")
        return 1 if problems else 0
    if a.cmd == "credits":
        log(write_credits(m, a.out, a.title))
        return 0
    only = [s for s in getattr(a, "only", "").split(",") if s]
    if a.cmd == "pin":
        unpinned = [e["id"] for e in m.assets if not e.get("sha256") and not all(x.get("sha256") for x in e.get("files", [{}]))]
        fetch(m, only or unpinned or ["__none__"], a.jobs, pin=True) if (only or unpinned) else log("everything is pinned")
        return 0
    status = 0
    for mm in [m] + mounted_manifests(a.manifest):
        problems = [p for p in check(mm) if "not pinned" in p or "license" in p]
        if problems:
            log(f"refusing to fetch {mm.path}:\n  " + "\n  ".join(problems))
            status = 1
            continue
        t0 = time.time()
        res = fetch(mm, only if mm is m else None, a.jobs)
        log(f"{len(res)} entries ready in {mm.root} ({sum(1 for v in res.values() if v == 'cached')} cached, {time.time() - t0:.0f}s)")
        builder = os.path.join(mm.dir, "tools", "build_kit.py")
        if mm is not m and os.path.exists(builder) and not os.path.isdir(os.path.join(mm.dir, "generated")):
            log(f"  the kit has generated content to build: python3 {builder}")
    return status


def resolve_manifest_path(path):
    """A manifest file, or a project / kit folder holding assets.json."""
    if os.path.isdir(path):
        return os.path.join(path, "assets.json")
    return path


def mounted_manifests(path):
    """Manifests of the folders a project mounts in game.json ("mounts": {"kit": "../_kit"})."""
    folder = path if os.path.isdir(path) else os.path.dirname(os.path.abspath(path))
    try:
        with open(os.path.join(folder, "game.json")) as f:
            mounts = json.load(f).get("mounts", {})
    except (OSError, ValueError):
        return []
    out = []
    for _name, rel in mounts.items():
        root = os.path.normpath(os.path.join(folder, os.path.expanduser(rel)))
        mp = os.path.join(root, "assets.json")
        if os.path.exists(mp):
            out.append(Manifest.load(mp))
    return out


if __name__ == "__main__":
    sys.exit(main())
