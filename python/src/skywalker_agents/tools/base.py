"""Tools: what an agent can call. Engine tools, Python functions and MCP servers look the same.

Write a Python tool with :func:`tool`::

    @tool
    async def count_hazards(radius: float = 10.0, *, ctx: ToolContext) -> dict:
        '''Count hazards near the player.

        Args:
            radius: Search radius in meters.
        '''
        scene = await ctx.session.call("scene_overview")
        ...

The schema comes from the signature (pydantic), descriptions from the docstring. A ``ctx``
parameter annotated :class:`ToolContext` is injected and hidden from the model.
"""

from __future__ import annotations

import asyncio
import base64
import inspect
import json
import re
from collections.abc import Awaitable, Callable, Iterable, Iterator
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any, get_type_hints, overload

from pydantic import BaseModel, ValidationError, create_model

from ..engine.results import ToolResult

if TYPE_CHECKING:  # pragma: no cover
    from ..engine.client import AgentSession, AsyncEngine
    from ..memory.memory import Memory


@dataclass
class ToolContext:
    """What a running tool knows about its caller."""

    agent_id: str = ""
    actor: str = ""
    call_id: str = ""
    engine: AsyncEngine | None = None
    session: AgentSession | None = None
    memory: Memory | None = None
    metadata: dict[str, Any] = field(default_factory=dict)


ToolFn = Callable[..., Any]


class Tool:
    """A callable capability with a JSON Schema, described for language models."""

    def __init__(
        self,
        name: str,
        description: str,
        input_schema: dict[str, Any],
        handler: Callable[[dict[str, Any], ToolContext], Awaitable[ToolResult]],
        *,
        mutates: bool = False,
        category: str = "python",
        requires_approval: bool | None = None,
        source: str = "python",
        strict: bool = False,
    ) -> None:
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", name):
            raise ValueError(f"tool name '{name}' must match [A-Za-z0-9_-]{{1,64}}")
        self.name = name
        self.description = description
        self.input_schema = input_schema
        self._handler = handler
        self.mutates = mutates
        self.category = category
        self.requires_approval = requires_approval
        self.source = source
        self.strict = strict

    async def run(self, args: dict[str, Any], ctx: ToolContext | None = None) -> ToolResult:
        """Runs the tool; exceptions become error results the model can read."""
        try:
            result = await self._handler(dict(args or {}), ctx or ToolContext())
        except ValidationError as e:
            return ToolResult.error(
                "invalid_arguments", _validation_text(e), "check the tool's input schema", tool=self.name
            )
        except Exception as e:  # a tool failure is information for the model, not a crash
            return ToolResult.error("tool_failed", f"{type(e).__name__}: {e}", tool=self.name)
        result.tool = self.name
        return result

    def spec(self) -> dict[str, Any]:
        """Provider-neutral spec: name, description, input_schema."""
        return {"name": self.name, "description": self.description, "input_schema": self.input_schema}

    def with_name(self, name: str) -> Tool:
        clone = Tool(
            name,
            self.description,
            self.input_schema,
            self._handler,
            mutates=self.mutates,
            category=self.category,
            requires_approval=self.requires_approval,
            source=self.source,
            strict=self.strict,
        )
        return clone

    def __repr__(self) -> str:
        return f"Tool({self.name!r}, source={self.source!r})"


def to_result(value: Any) -> ToolResult:
    """Normalizes what a tool function returned into a :class:`ToolResult`."""
    if isinstance(value, ToolResult):
        return value
    if value is None:
        return ToolResult.ok("ok")
    if isinstance(value, str):
        return ToolResult.ok(value)
    if isinstance(value, bytes):
        return ToolResult.ok("image", images=[value])
    if isinstance(value, BaseModel):
        return ToolResult.ok(data=value.model_dump(mode="json"))
    if isinstance(value, dict):
        return ToolResult.ok(data=json.loads(json.dumps(value, default=str)))
    if isinstance(value, (list, tuple, int, float, bool)):
        return ToolResult.ok(json.dumps(value, default=str), {"value": value})
    return ToolResult.ok(str(value))


_ARG_RE = re.compile(r"^(\*{0,2}\w+)\s*(?:\([^)]*\))?:\s*(.*)$")


def parse_docstring(doc: str | None) -> tuple[str, dict[str, str]]:
    """Summary text and per-argument descriptions from a Google-style docstring."""
    if not doc:
        return "", {}
    summary: list[str] = []
    args: dict[str, str] = {}
    section = ""
    current = ""
    arg_indent: int | None = None
    for line in inspect.cleandoc(doc).splitlines():
        stripped = line.strip()
        if stripped in ("Args:", "Arguments:", "Parameters:"):
            section, arg_indent = "args", None
            continue
        if stripped in ("Returns:", "Raises:", "Example:", "Examples:", "Yields:", "Note:"):
            section = "other"
            continue
        if section == "args":
            if not stripped:
                continue
            indent = len(line) - len(line.lstrip())
            m = _ARG_RE.match(stripped)
            if m and (arg_indent is None or indent <= arg_indent):
                arg_indent = indent
                current = m.group(1).lstrip("*")
                args[current] = m.group(2).strip()
            elif current:
                args[current] += " " + stripped
        elif section == "":
            summary.append(line)
    return "\n".join(summary).strip(), args


def _ctx_param(fn: ToolFn, hints: dict[str, Any]) -> str | None:
    for pname, ann in hints.items():
        if ann is ToolContext:
            return pname
    sig = inspect.signature(fn)
    return "ctx" if "ctx" in sig.parameters and "ctx" not in hints else None


def _type_hints(fn: ToolFn) -> dict[str, Any]:
    """Resolved annotations; names that cannot be resolved (local imports) fall back to Any, and an
    annotation spelled ``ToolContext`` always means the injected context."""
    try:
        return get_type_hints(fn, localns={"ToolContext": ToolContext})
    except Exception:
        pass
    out: dict[str, Any] = {}
    glb = dict(getattr(fn, "__globals__", {}))
    glb.setdefault("ToolContext", ToolContext)
    for pname, ann in getattr(fn, "__annotations__", {}).items():
        if isinstance(ann, str):
            try:
                ann = eval(ann, glb)  # noqa: S307 - evaluating the function's own annotation
            except Exception:
                ann = ToolContext if ann.endswith("ToolContext") else Any
        out[pname] = ann
    return out


def _function_tool(
    fn: ToolFn,
    *,
    name: str | None,
    description: str | None,
    mutates: bool,
    category: str,
    requires_approval: bool | None,
) -> Tool:
    hints = _type_hints(fn)
    ctx_name = _ctx_param(fn, hints)
    summary, arg_docs = parse_docstring(fn.__doc__)
    fields: dict[str, Any] = {}
    for pname, param in inspect.signature(fn).parameters.items():
        if pname == ctx_name or param.kind in (inspect.Parameter.VAR_POSITIONAL, inspect.Parameter.VAR_KEYWORD):
            continue
        ann = hints.get(pname, Any)
        default = ... if param.default is inspect.Parameter.empty else param.default
        from pydantic import Field

        fields[pname] = (ann, Field(default, description=arg_docs.get(pname)))
    model = create_model(f"{fn.__name__}_args", **fields)
    schema = model.model_json_schema()
    schema.pop("title", None)
    for prop in schema.get("properties", {}).values():
        prop.pop("title", None)
    is_async = inspect.iscoroutinefunction(fn)

    async def handler(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
        parsed = model.model_validate(args)
        kwargs = {k: getattr(parsed, k) for k in fields}
        if ctx_name:
            kwargs[ctx_name] = ctx
        if is_async:
            value = await fn(**kwargs)
        else:
            value = await asyncio.to_thread(fn, **kwargs)
        return to_result(value)

    return Tool(
        name or fn.__name__,
        description or summary or fn.__name__,
        schema,
        handler,
        mutates=mutates,
        category=category,
        requires_approval=requires_approval,
    )


@overload
def tool(fn: ToolFn, /) -> Tool: ...


@overload
def tool(
    *,
    name: str | None = None,
    description: str | None = None,
    mutates: bool = False,
    category: str = "python",
    requires_approval: bool | None = None,
) -> Callable[[ToolFn], Tool]: ...


def tool(
    fn: ToolFn | None = None,
    /,
    *,
    name: str | None = None,
    description: str | None = None,
    mutates: bool = False,
    category: str = "python",
    requires_approval: bool | None = None,
) -> Tool | Callable[[ToolFn], Tool]:
    """Turns a (sync or async) function into a :class:`Tool`. Usable bare or with options."""

    def wrap(f: ToolFn) -> Tool:
        return _function_tool(
            f,
            name=name,
            description=description,
            mutates=mutates,
            category=category,
            requires_approval=requires_approval,
        )

    return wrap(fn) if fn is not None else wrap


def engine_tool(
    session: AgentSession | AsyncEngine, spec: dict[str, Any] | Any, *, requires_approval: bool | None = None
) -> Tool:
    """Wraps an engine tool (from ``tools/list``) so an agent calls it through its own session."""
    if not isinstance(spec, dict):
        spec = spec.model_dump(by_alias=True)
    name = str(spec["name"])
    meta = spec.get("_meta") or {}
    annotations = spec.get("annotations") or {}

    async def handler(args: dict[str, Any], ctx: ToolContext) -> ToolResult:
        return await session.call(name, args)

    return Tool(
        name,
        str(spec.get("description", "")),
        dict(spec.get("inputSchema") or {"type": "object"}),
        handler,
        mutates=not bool(annotations.get("readOnlyHint", False)),
        category=str(meta.get("skywalker/category", "engine")),
        requires_approval=requires_approval,
        source="engine",
    )


class Toolset:
    """An ordered, name-unique collection of tools."""

    def __init__(self, tools: Iterable[Tool] = ()) -> None:
        self._tools: dict[str, Tool] = {}
        for t in tools:
            self.add(t)

    def add(self, t: Tool, *, replace: bool = True) -> Toolset:
        if not replace and t.name in self._tools:
            raise ValueError(f"duplicate tool '{t.name}'")
        self._tools[t.name] = t
        return self

    def extend(self, tools: Iterable[Tool]) -> Toolset:
        for t in tools:
            self.add(t)
        return self

    def get(self, name: str) -> Tool | None:
        return self._tools.get(name)

    def names(self) -> list[str]:
        return list(self._tools)

    def specs(self) -> list[dict[str, Any]]:
        return [t.spec() for t in self._tools.values()]

    def only(self, names: Iterable[str]) -> Toolset:
        keep = set(names)
        return Toolset(t for t in self._tools.values() if t.name in keep)

    def without(self, names: Iterable[str]) -> Toolset:
        drop = set(names)
        return Toolset(t for t in self._tools.values() if t.name not in drop)

    def __iter__(self) -> Iterator[Tool]:
        return iter(self._tools.values())

    def __len__(self) -> int:
        return len(self._tools)

    def __contains__(self, name: object) -> bool:
        return name in self._tools


def image_result(png: bytes, text: str = "image") -> ToolResult:
    return ToolResult.ok(text, images=[png])


def b64(data: bytes) -> str:
    return base64.b64encode(data).decode()


def _validation_text(e: ValidationError) -> str:
    parts = []
    for err in e.errors()[:5]:
        loc = ".".join(str(x) for x in err.get("loc", ()))
        parts.append(f"args.{loc}: {err.get('msg')}")
    return "; ".join(parts)
