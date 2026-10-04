"""Providers without network: request building and response parsing against stand-in SDK objects."""

from __future__ import annotations

import json
from types import SimpleNamespace
from typing import Any

import pytest

from skywalker_agents.errors import ProviderError
from skywalker_agents.llm import (
    ChatMessage,
    ImageBlock,
    LLMRequest,
    ScriptedProvider,
    TextBlock,
    ToolCall,
    ToolResultBlock,
    ToolSpecParam,
    Usage,
    cost_usd,
    get_provider,
    price_for,
)
from skywalker_agents.llm.anthropic import AnthropicProvider
from skywalker_agents.llm.openai_compat import OpenAICompatibleProvider


class _Block(SimpleNamespace):
    def model_dump(self, **_: Any) -> dict[str, Any]:
        return {k: v for k, v in vars(self).items() if v is not None}


def _message(content: list[_Block], stop: str = "end_turn", **extra: Any) -> SimpleNamespace:
    usage = SimpleNamespace(
        input_tokens=10, output_tokens=5, cache_read_input_tokens=100, cache_creation_input_tokens=7
    )
    return SimpleNamespace(content=content, stop_reason=stop, usage=usage, model="claude-opus-5-5", **extra)


class _Stream:
    def __init__(self, msg: Any, fail: list[Exception]) -> None:
        self.msg = msg
        self.fail = fail

    async def __aenter__(self) -> _Stream:
        return self

    async def __aexit__(self, *exc: object) -> None:
        return None

    async def get_final_message(self) -> Any:
        if self.fail:
            raise self.fail.pop(0)
        return self.msg


class _FakeAnthropic:
    base_url = "https://api.anthropic.com"

    def __init__(self, msg: Any, fail: list[Exception] | None = None) -> None:
        self.calls: list[dict[str, Any]] = []
        outer = self
        fails = fail or []

        class Messages:
            def stream(self, **params: Any) -> _Stream:
                outer.calls.append(params)
                return _Stream(msg, fails)

        self.beta = SimpleNamespace(messages=Messages())


def _request(model: str = "claude-opus-5-5") -> LLMRequest:
    raw_assistant = [
        {"type": "thinking", "thinking": "", "signature": "sig"},
        {"type": "tool_use", "id": "tu1", "name": "scene_overview", "input": {}},
    ]
    return LLMRequest(
        model=model,
        system="You are Mira.",
        tools=[ToolSpecParam(name="scene_overview")],
        messages=[
            ChatMessage.user("Look at the scene"),
            ChatMessage(
                role="assistant",
                provider="anthropic",
                raw=raw_assistant,
                content=[ToolCall(id="tu1", name="scene_overview")],
            ),
            ChatMessage(
                role="user",
                content=[
                    ToolResultBlock(tool_call_id="tu1", content=[TextBlock(text="#1 Cube"), ImageBlock(data="AAAA")]),
                    TextBlock(text="go on"),
                ],
            ),
        ],
    )


def test_anthropic_request_follows_current_guidance() -> None:
    p = AnthropicProvider(client=_FakeAnthropic(None))
    params = p.build_params(_request())
    assert params["model"] == "claude-opus-5-5"
    assert params["max_tokens"] == 64000
    assert params["thinking"] == {"type": "adaptive"}
    assert params["output_config"] == {"effort": "high"}
    assert params["system"][0]["cache_control"] == {"type": "ephemeral"}
    assert params["cache_control"] == {"type": "ephemeral"}
    assert params["fallbacks"] == "default" and params["betas"] == ["server-side-fallback-2026-07-01"]
    assert params["tools"][0]["eager_input_streaming"] is True
    # Append-only: the assistant turn is echoed unchanged (thinking block and signature).
    assert params["messages"][1]["content"][0] == {"type": "thinking", "thinking": "", "signature": "sig"}
    # Tool results come first in their user message, images as base64 blocks.
    user = params["messages"][2]["content"]
    assert user[0]["type"] == "tool_result" and user[0]["content"][1]["type"] == "image"
    assert user[1] == {"type": "text", "text": "go on"}


def test_anthropic_older_models_and_proxies() -> None:
    haiku = AnthropicProvider(client=_FakeAnthropic(None)).build_params(_request("claude-haiku-4-5"))
    assert "thinking" not in haiku and "fallbacks" not in haiku
    proxied = _FakeAnthropic(None)
    proxied.base_url = "https://proxy.example.com"
    assert "fallbacks" not in AnthropicProvider(client=proxied).build_params(_request())


async def test_anthropic_parse_tool_use_refusal_and_json_retry() -> None:
    msg = _message(
        [
            _Block(type="thinking", thinking="", signature="s"),
            _Block(type="text", text="Looking."),
            _Block(type="tool_use", id="t1", name="scene_overview", input={"a": 1}),
        ],
        stop="tool_use",
    )
    client = _FakeAnthropic(msg, fail=[ValueError("bad partial json")])
    resp = await AnthropicProvider(client=client).complete(_request())
    assert len(client.calls) == 2  # unparseable eager tool input: re-issued once
    assert resp.stop_reason == "tool_use"
    assert resp.tool_calls[0].input == {"a": 1}
    assert resp.raw is not None and resp.raw[0]["type"] == "thinking"
    assert resp.usage.cache_read_tokens == 100 and resp.usage.cache_write_tokens == 7
    refused = AnthropicProvider.parse(
        _message([_Block(type="text", text="partial")], stop="refusal", stop_details=SimpleNamespace(category="cyber")),
        "claude-opus-5-5",
    )
    assert refused.content == [] and refused.raw is None and refused.refusal_category == "cyber"


async def test_anthropic_bad_request_is_not_retryable() -> None:
    import anthropic
    import httpx2 as httpx

    req = httpx.Request("POST", "https://api.anthropic.com/v1/messages")
    err = anthropic.BadRequestError("bad", response=httpx.Response(400, request=req), body=None)
    with pytest.raises(ProviderError) as e:
        await AnthropicProvider(client=_FakeAnthropic(None, fail=[err])).complete(_request())
    assert e.value.retryable is False


def test_openai_compatible_request_and_parse() -> None:
    p = OpenAICompatibleProvider(
        "ollama", client=SimpleNamespace(base_url="http://localhost:11434/v1"), default_model="qwen3"
    )
    req = _request("")
    params = p.build_params(req)
    assert params["model"] == "qwen3" and params["max_tokens"] == 8192
    roles = [m["role"] for m in params["messages"]]
    assert roles == ["system", "user", "assistant", "tool", "user"]
    assert params["messages"][2]["tool_calls"][0]["function"]["name"] == "scene_overview"
    # The screenshot from the tool result follows as an image user message.
    assert any(part.get("type") == "image_url" for part in params["messages"][4]["content"])
    msg = SimpleNamespace(
        content=None,
        tool_calls=[
            SimpleNamespace(id="c1", function=SimpleNamespace(name="entity_create", arguments='{"name": "A"')),
            SimpleNamespace(id="c2", function=SimpleNamespace(name="scene_overview", arguments="{}")),
        ],
        model_dump=lambda **_: {"role": "assistant", "content": None, "tool_calls": []},
    )
    resp = p.parse(
        SimpleNamespace(
            choices=[SimpleNamespace(message=msg, finish_reason="tool_calls")],
            usage=SimpleNamespace(
                prompt_tokens=50, completion_tokens=9, prompt_tokens_details=SimpleNamespace(cached_tokens=20)
            ),
            model="qwen3",
        ),
        "qwen3",
    )
    assert resp.stop_reason == "tool_use"
    assert resp.tool_calls[0].invalid_json and resp.tool_calls[0].raw_input == '{"name": "A"'
    assert resp.usage.input_tokens == 30 and resp.usage.cache_read_tokens == 20
    with pytest.raises(ProviderError):
        OpenAICompatibleProvider("ollama", client=SimpleNamespace(), default_model="").build_params(_request(""))


async def test_scripted_provider_per_agent_and_registry() -> None:
    p = ScriptedProvider([{"text": "shared"}], agents={"mira": [{"tool_calls": [{"name": "x", "input": {"a": 1}}]}]})
    r1 = await p.complete(LLMRequest(model="m", messages=[ChatMessage.user("hi")], metadata={"agent": "mira"}))
    assert r1.stop_reason == "tool_use" and r1.tool_calls[0].name == "x"
    r2 = await p.complete(LLMRequest(model="m", messages=[ChatMessage.user("hi")], metadata={"agent": "zed"}))
    assert r2.text == "shared"
    assert isinstance(get_provider("mock"), ScriptedProvider)
    assert get_provider("ollama", client=SimpleNamespace(), default_model="x").name == "ollama"


def test_pricing() -> None:
    u = Usage(input_tokens=1_000_000, output_tokens=1_000_000, cache_read_tokens=1_000_000)
    assert cost_usd("claude-opus-5-5", u) == pytest.approx(4 + 20 + 0.2)
    assert price_for("anthropic.claude-sonnet-5-5") is not None
    assert price_for("claude-opus-5") != price_for("claude-opus-5-5")
    assert cost_usd("my-local-model", u) == 0.0
    assert json.dumps(u.model_dump())
