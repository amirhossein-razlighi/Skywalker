"""Optional: any model LiteLLM supports, via its OpenAI-shaped ``acompletion`` (``[litellm]`` extra)."""

from __future__ import annotations

from typing import Any

from ..errors import ProviderError
from .openai_compat import OpenAICompatibleProvider
from .types import LLMRequest, LLMResponse


class LiteLLMProvider(OpenAICompatibleProvider):
    """``model`` uses LiteLLM's ``provider/model`` ids (e.g. ``gemini/...``, ``bedrock/...``)."""

    def __init__(
        self, *, default_model: str = "", vision: bool = True, max_tokens_cap: int = 8192, **litellm_kwargs: Any
    ) -> None:
        try:
            import litellm
        except ImportError as e:  # pragma: no cover - depends on the extra
            raise ProviderError("the LiteLLM provider needs `pip install skywalker-agents[litellm]`") from e
        self._litellm = litellm
        self._kwargs = litellm_kwargs
        super().__init__(
            "litellm", client=object(), default_model=default_model, vision=vision, max_tokens_cap=max_tokens_cap
        )

    async def complete(self, request: LLMRequest) -> LLMResponse:
        params = self.build_params(request)
        params["max_tokens"] = params.pop("max_completion_tokens", params.get("max_tokens"))
        try:
            resp = await self._litellm.acompletion(**params, **self._kwargs)
        except Exception as e:
            raise ProviderError(f"litellm: {e}") from e
        return self.parse(resp, params["model"])
