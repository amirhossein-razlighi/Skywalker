#!/usr/bin/env python3
"""Films every showcase game being built live in the editor by the crew.

  record_all.py WORK_DIR [game ...]   ->  WORK_DIR/rec/<game>/{frames/, events.jsonl, edits.jsonl}
Needs the release editor build (build/release/bin/Skywalker.app).
"""
import os, socket, subprocess, sys, time
DEMO = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(DEMO))
APP = os.path.join(ROOT, "build/release/bin/Skywalker.app/Contents/MacOS/Skywalker")
W = sys.argv[1]
games = sys.argv[2:] or subprocess.check_output([sys.executable, "showcase.py", "list"], cwd=DEMO, text=True).split()
SOCK = os.path.expanduser("~/.skywalker/editor.sock")

def ready():
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(2)
        s.connect(SOCK)
        s.sendall(b'{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","clientInfo":{"name":"probe"}}}\n')
        ok = b"result" in s.recv(65536)
        s.close()
        return ok
    except OSError:
        return False

for g in games:
    out = os.path.join(W, "rec", g)
    subprocess.run(["rm", "-rf", out])
    os.makedirs(out)
    subprocess.run(["pkill", "-f", "Skywalker.app/Contents/MacOS/Skywalker"])
    time.sleep(1.5)
    scene = os.path.join(ROOT, "examples", g, "scenes", "main.sky.json")
    if os.path.exists(scene):
        os.remove(scene)
    env = dict(os.environ, SKY_PROJECT=os.path.join(ROOT, "examples", g))
    editor = subprocess.Popen([APP], env=env, stdout=open(os.path.join(out, "editor.log"), "w"), stderr=subprocess.STDOUT)
    t0 = time.time()
    while not ready():
        if time.time() - t0 > 40:
            raise SystemExit(f"{g}: editor did not come up")
        time.sleep(0.5)
    time.sleep(2)
    rec = subprocess.Popen([sys.executable, os.path.join(DEMO, "record_session.py"), out, "--interval", "0.25", "--size", "1280x720"],
                           stdout=open(os.path.join(out, "rec.log"), "w"), stderr=subprocess.STDOUT)
    time.sleep(1)
    tb = time.time()
    subprocess.run([sys.executable, "showcase.py", "build", g, "--attach", "--pace", "1.0", "--log", os.path.join(out, "edits.jsonl")],
                   cwd=DEMO, check=True)
    time.sleep(2.5)
    open(os.path.join(out, "STOP"), "w").close()
    rec.wait(timeout=30)
    editor.terminate()
    try:
        editor.wait(timeout=10)
    except subprocess.TimeoutExpired:
        editor.kill()
    n = len(os.listdir(os.path.join(out, "frames")))
    print(f"{g}: built in {time.time() - tb:.1f}s, {n} frames", flush=True)
print("RECORDED")
