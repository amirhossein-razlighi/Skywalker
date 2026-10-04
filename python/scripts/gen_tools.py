"""Regenerates src/skywalker_agents/engine/_generated.py from a skywalker binary.

Usage: python scripts/gen_tools.py [path/to/skywalker]   (same as `sky-agents gen-tools`)
"""

from __future__ import annotations

import sys
from pathlib import Path

from skywalker_agents.engine import codegen
from skywalker_agents.engine.process import find_binary


def main() -> int:
    binary = find_binary(sys.argv[1] if len(sys.argv) > 1 else None)
    tools, version = codegen.tools_from_binary(binary)
    out = Path(__file__).resolve().parents[1] / "src" / "skywalker_agents" / "engine" / "_generated.py"
    out.write_text(codegen.generate(tools, version))
    print(f"wrote {out} ({len(tools)} tools, engine {version})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
