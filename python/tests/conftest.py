from __future__ import annotations

import os
import shutil
from collections.abc import AsyncIterator, Iterator
from pathlib import Path

import pytest

from skywalker_agents import AsyncEngine, FakeEngine

REPO = Path(__file__).resolve().parents[2]


@pytest.fixture(autouse=True)
def _isolated_home(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """Global memory and run dirs never touch the real ~/.skywalker."""
    monkeypatch.setenv("SKY_AGENTS_HOME", str(tmp_path / "home"))


@pytest.fixture
def fake() -> FakeEngine:
    return FakeEngine()


@pytest.fixture
async def engine(fake: FakeEngine) -> AsyncIterator[AsyncEngine]:
    eng = AsyncEngine.fake(fake)
    yield eng
    await eng.close()


@pytest.fixture
def project(tmp_path: Path) -> Path:
    p = tmp_path / "game"
    (p / "agents").mkdir(parents=True)
    return p


def skywalker_binary() -> str | None:
    for cand in (os.environ.get("SKYWALKER_BIN"), str(REPO / "build" / "release" / "bin" / "skywalker"),
                 str(REPO / "build" / "headless" / "bin" / "skywalker"), shutil.which("skywalker")):
        if cand and os.path.isfile(cand) and os.access(cand, os.X_OK):
            return cand
    return None


@pytest.fixture
def binary() -> str:
    b = skywalker_binary()
    if b is None:
        pytest.skip("no skywalker binary (build it or set SKYWALKER_BIN)")
    return b


@pytest.fixture
def sky_dash(tmp_path: Path) -> Iterator[Path]:
    """A throwaway copy of examples/sky_dash (agents write studio files into the project)."""
    src = REPO / "examples" / "sky_dash"
    dst = tmp_path / "sky_dash"
    shutil.copytree(src, dst, ignore=shutil.ignore_patterns("* 2.*", "studio"))
    yield dst
