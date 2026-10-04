"""Estimated USD per million tokens, for cost accounting and budgets.

Register prices for other models with :func:`set_price`. Unknown models cost 0 and are reported
as unpriced.
"""

from __future__ import annotations

from dataclasses import dataclass

from .types import Usage


@dataclass(frozen=True)
class Price:
    input: float
    output: float
    cache_read: float
    cache_write: float


# First-party Claude API list prices (cache writes at the 5-minute TTL rate, 1.25x input).
_PRICES: dict[str, Price] = {
    "claude-fable-5-1": Price(10.0, 50.0, 0.25, 12.5),
    "claude-fable-5": Price(10.0, 50.0, 1.0, 12.5),
    "claude-opus-5-5": Price(4.0, 20.0, 0.20, 5.0),
    "claude-opus-5": Price(5.0, 25.0, 0.50, 6.25),
    "claude-opus-4-8": Price(5.0, 25.0, 0.50, 6.25),
    "claude-opus-4-7": Price(5.0, 25.0, 0.50, 6.25),
    "claude-opus-4-6": Price(5.0, 25.0, 0.50, 6.25),
    "claude-sonnet-5-5": Price(2.0, 10.0, 0.20, 2.5),
    "claude-sonnet-5": Price(2.0, 10.0, 0.20, 2.5),
    "claude-sonnet-4-6": Price(3.0, 15.0, 0.30, 3.75),
    "claude-haiku-4-5": Price(1.0, 5.0, 0.10, 1.25),
}


def set_price(
    model: str, input: float, output: float, cache_read: float | None = None, cache_write: float | None = None
) -> None:
    """Registers (or overrides) a model's price in USD per million tokens."""
    _PRICES[model] = Price(
        input,
        output,
        cache_read if cache_read is not None else input,
        cache_write if cache_write is not None else input,
    )


def price_for(model: str) -> Price | None:
    if model in _PRICES:
        return _PRICES[model]
    # Provider-prefixed ids (anthropic/claude-..., anthropic.claude-...) and dated snapshots.
    for key in sorted(_PRICES, key=len, reverse=True):  # longest first: claude-opus-5-5 before claude-opus-5
        if key in model:
            return _PRICES[key]
    return None


def cost_usd(model: str, usage: Usage) -> float:
    p = price_for(model)
    if p is None:
        return 0.0
    return (
        usage.input_tokens * p.input
        + usage.output_tokens * p.output
        + usage.cache_read_tokens * p.cache_read
        + usage.cache_write_tokens * p.cache_write
    ) / 1e6
