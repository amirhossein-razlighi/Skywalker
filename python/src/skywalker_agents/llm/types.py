"""Provider-neutral conversation types.

Histories are append-only: an assistant turn keeps the provider's native content (``raw``) so it is
echoed back unchanged on the next request (thinking blocks, signatures, server-tool blocks).
"""

from __future__ import annotations

import base64
from typing import Annotated, Any, Literal, Protocol, runtime_checkable

from pydantic import BaseModel, Field

StopReason = Literal["end_turn", "tool_use", "max_tokens", "refusal", "pause_turn", "stop_sequence", "error"]


class TextBlock(BaseModel):
    type: Literal["text"] = "text"
    text: str


class ImageBlock(BaseModel):
    type: Literal["image"] = "image"
    data: str  # base64
    media_type: str = "image/png"

    @classmethod
    def from_bytes(cls, raw: bytes, media_type: str = "image/png") -> ImageBlock:
        return cls(data=base64.b64encode(raw).decode(), media_type=media_type)


class ToolCall(BaseModel):
    type: Literal["tool_call"] = "tool_call"
    id: str
    name: str
    input: dict[str, Any] = Field(default_factory=dict)
    invalid_json: bool = False  # the model produced arguments that did not parse
    raw_input: str = ""


class ToolResultBlock(BaseModel):
    type: Literal["tool_result"] = "tool_result"
    tool_call_id: str
    name: str = ""
    content: list[Annotated[TextBlock | ImageBlock, Field(discriminator="type")]] = Field(default_factory=list)
    is_error: bool = False

    @property
    def text(self) -> str:
        return "\n".join(b.text for b in self.content if isinstance(b, TextBlock))


Block = Annotated[TextBlock | ImageBlock | ToolCall | ToolResultBlock, Field(discriminator="type")]


class ChatMessage(BaseModel):
    role: Literal["user", "assistant", "system"]
    content: list[Block] = Field(default_factory=list)
    provider: str = ""  # which provider produced `raw`
    raw: list[dict[str, Any]] | None = None  # native assistant content, echoed unchanged

    @classmethod
    def user(cls, text: str, images: list[bytes] | None = None) -> ChatMessage:
        blocks: list[Any] = [ImageBlock.from_bytes(i) for i in images or []]
        blocks.append(TextBlock(text=text))
        return cls(role="user", content=blocks)

    @property
    def text(self) -> str:
        return "\n".join(b.text for b in self.content if isinstance(b, TextBlock))

    @property
    def tool_calls(self) -> list[ToolCall]:
        return [b for b in self.content if isinstance(b, ToolCall)]


class Usage(BaseModel):
    input_tokens: int = 0
    output_tokens: int = 0
    cache_read_tokens: int = 0
    cache_write_tokens: int = 0
    requests: int = 0

    def __add__(self, other: Usage) -> Usage:
        return Usage(
            input_tokens=self.input_tokens + other.input_tokens,
            output_tokens=self.output_tokens + other.output_tokens,
            cache_read_tokens=self.cache_read_tokens + other.cache_read_tokens,
            cache_write_tokens=self.cache_write_tokens + other.cache_write_tokens,
            requests=self.requests + other.requests,
        )

    @property
    def total_tokens(self) -> int:
        return self.input_tokens + self.output_tokens + self.cache_read_tokens + self.cache_write_tokens


class ToolSpecParam(BaseModel):
    name: str
    description: str = ""
    input_schema: dict[str, Any] = Field(default_factory=lambda: {"type": "object"})


class LLMRequest(BaseModel):
    model: str
    system: str = ""
    messages: list[ChatMessage]
    tools: list[ToolSpecParam] = Field(default_factory=list)
    max_tokens: int = 64000
    effort: Literal["low", "medium", "high", "xhigh", "max"] | None = "high"
    metadata: dict[str, Any] = Field(default_factory=dict)  # agent id, run id (not sent to providers)


class LLMResponse(BaseModel):
    content: list[Block] = Field(default_factory=list)
    stop_reason: StopReason = "end_turn"
    usage: Usage = Field(default_factory=Usage)
    model: str = ""
    provider: str = ""
    raw: list[dict[str, Any]] | None = None
    refusal_category: str | None = None

    @property
    def text(self) -> str:
        return "\n".join(b.text for b in self.content if isinstance(b, TextBlock))

    @property
    def tool_calls(self) -> list[ToolCall]:
        return [b for b in self.content if isinstance(b, ToolCall)]

    def as_message(self) -> ChatMessage:
        return ChatMessage(role="assistant", content=list(self.content), provider=self.provider, raw=self.raw)


@runtime_checkable
class Provider(Protocol):
    """A chat model behind one call: request in, response out (retries inside)."""

    name: str
    default_model: str
    supports_vision: bool

    async def complete(self, request: LLMRequest) -> LLMResponse: ...
