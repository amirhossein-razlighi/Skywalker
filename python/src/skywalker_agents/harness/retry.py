"""Retries with exponential backoff, timeouts and bounded concurrency."""

from __future__ import annotations

import asyncio
import random
from collections.abc import Awaitable, Callable, Iterable, Sequence
from typing import TypeVar

from ..errors import EngineConnectionError, ProviderError

T = TypeVar("T")
RETRYABLE: tuple[type[BaseException], ...] = (
    ProviderError,
    EngineConnectionError,
    asyncio.TimeoutError,
    ConnectionError,
)


async def retry_async(
    fn: Callable[[], Awaitable[T]],
    *,
    attempts: int = 3,
    base_delay: float = 0.5,
    max_delay: float = 10.0,
    retry_on: Sequence[type[BaseException]] = RETRYABLE,
    jitter: bool = True,
    on_retry: Callable[[int, BaseException], None] | None = None,
) -> T:
    """Calls ``fn`` until it succeeds or ``attempts`` run out (exponential backoff with jitter)."""
    last: BaseException | None = None
    for attempt in range(1, max(1, attempts) + 1):
        try:
            return await fn()
        except tuple(retry_on) as e:
            last = e
            if attempt >= attempts or getattr(e, "retryable", True) is False:
                break
            if on_retry is not None:
                on_retry(attempt, e)
            delay = min(max_delay, base_delay * 2 ** (attempt - 1))
            if jitter:
                delay *= 0.5 + random.random()
            await asyncio.sleep(delay)
    assert last is not None
    raise last


async def with_timeout(aw: Awaitable[T], seconds: float | None) -> T:
    """Awaits with a timeout (None = no limit); the task is cancelled on timeout."""
    if seconds is None:
        return await aw
    return await asyncio.wait_for(aw, seconds)


async def gather_limited(
    factories: Iterable[Callable[[], Awaitable[T]]], limit: int = 4, *, return_exceptions: bool = False
) -> list[T | BaseException]:
    """Runs coroutine factories with at most ``limit`` at a time; results keep input order."""
    sem = asyncio.Semaphore(max(1, limit))

    async def run(f: Callable[[], Awaitable[T]]) -> T:
        async with sem:
            return await f()

    results = await asyncio.gather(*(run(f) for f in factories), return_exceptions=return_exceptions)
    return list(results)
