"""Tool results as returned by the engine (an MCP ``CallToolResult``)."""

from __future__ import annotations

import base64
import re
from typing import TYPE_CHECKING, Any, Literal

from pydantic import BaseModel, ConfigDict, Field

from ..errors import ToolError

if TYPE_CHECKING:  # pragma: no cover
    from PIL.Image import Image as PILImage

_ERROR_RE = re.compile(r"^error \[([a-z_]+)\]: (.*?)(?:\nhint: (.*))?$", re.S)


class ContentBlock(BaseModel):
    """One block of tool output: text, or an image (base64 PNG)."""

    model_config = ConfigDict(extra="allow")

    type: Literal["text", "image"] | str = "text"
    text: str | None = None
    data: str | None = None
    mimeType: str | None = None

    def image_bytes(self) -> bytes:
        return base64.b64decode(self.data or "")


class ToolResult(BaseModel):
    """What a tool returned.

    ``text`` is what a model reads, ``data`` the machine-readable payload (``structuredContent``),
    ``images`` the PNG images (captures, previews) as bytes.
    """

    model_config = ConfigDict(extra="allow", populate_by_name=True)

    tool: str = ""
    content: list[ContentBlock] = Field(default_factory=list)
    structured: dict[str, Any] | None = Field(default=None, alias="structuredContent")
    is_error: bool = Field(default=False, alias="isError")

    @classmethod
    def from_mcp(cls, tool: str, payload: dict[str, Any]) -> ToolResult:
        return cls.model_validate({**payload, "tool": tool})

    @classmethod
    def ok(
        cls, text: str = "", data: dict[str, Any] | None = None, images: list[bytes] | None = None, tool: str = ""
    ) -> ToolResult:
        blocks = [ContentBlock(type="text", text=text or (_json(data) if data is not None else ""))]
        for img in images or []:
            blocks.append(ContentBlock(type="image", data=base64.b64encode(img).decode(), mimeType="image/png"))
        return cls(tool=tool, content=blocks, structuredContent=data, isError=False)

    @classmethod
    def error(cls, code: str, message: str, hint: str = "", tool: str = "") -> ToolResult:
        text = f"error [{code}]: {message}" + (f"\nhint: {hint}" if hint else "")
        data: dict[str, Any] = {"error": code, "message": message}
        if hint:
            data["hint"] = hint
        return cls(tool=tool, content=[ContentBlock(type="text", text=text)], structuredContent=data, isError=True)

    # ------------------------------------------------------------------ accessors
    @property
    def text(self) -> str:
        return "\n".join(b.text or "" for b in self.content if b.type == "text")

    @property
    def data(self) -> dict[str, Any]:
        """The structured payload ({} when the tool returned only text)."""
        return dict(self.structured or {})

    def __getitem__(self, key: str) -> Any:
        return self.data[key]

    def get(self, key: str, default: Any = None) -> Any:
        return self.data.get(key, default)

    @property
    def images(self) -> list[bytes]:
        return [b.image_bytes() for b in self.content if b.type == "image" and b.data]

    def pil_images(self) -> list[PILImage]:
        """Images as PIL images (needs the ``images`` extra: pillow)."""
        import io

        from PIL import Image

        return [Image.open(io.BytesIO(raw)) for raw in self.images]

    def save_image(self, path: str, index: int = 0) -> str:
        with open(path, "wb") as f:
            f.write(self.images[index])
        return path

    # ------------------------------------------------------------------ errors
    @property
    def error_code(self) -> str:
        if not self.is_error:
            return ""
        if self.structured and isinstance(self.structured.get("error"), str):
            return str(self.structured["error"])
        m = _ERROR_RE.match(self.text)
        return m.group(1) if m else "tool_error"

    @property
    def error_hint(self) -> str:
        if self.structured and isinstance(self.structured.get("hint"), str):
            return str(self.structured["hint"])
        m = _ERROR_RE.match(self.text)
        return (m.group(3) or "") if m else ""

    @property
    def error_message(self) -> str:
        if self.structured and isinstance(self.structured.get("message"), str):
            return str(self.structured["message"])
        m = _ERROR_RE.match(self.text)
        return m.group(2) if m else self.text

    def raise_for_error(self) -> ToolResult:
        if self.is_error:
            raise ToolError(self.tool, self.error_code, self.error_message, self.error_hint)
        return self

    def to_mcp(self) -> dict[str, Any]:
        return self.model_dump(by_alias=True, exclude_none=True, exclude={"tool"})

    def __str__(self) -> str:
        return self.text


def _json(data: Any) -> str:
    import json

    return json.dumps(data, ensure_ascii=False)
