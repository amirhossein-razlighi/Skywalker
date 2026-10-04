"""An agent: a role, a model, tools, memory and policies, running a tool-use loop against the engine.

    engine = await AsyncEngine.auto("examples/sky_dash")
    mira = Agent("mira", engine=engine, memory=Memory.open("examples/sky_dash"))
    result = await mira.run("Make the lava bridge fair: deaths <= 1 per playtest")

What the editor sees: the agent's presence (working/idle with its activity), every tool call attributed
to ``mcp:sky-agents/<id>`` in the Activity feed, its messages and board updates in the Studio panel,
and its token usage and cost in the roster.
"""

from __future__ import annotations

import asyncio
import contextlib
import functools
import json
import time
from collections.abc import Sequence
from typing import TYPE_CHECKING, Any, Literal

from pydantic import BaseModel, Field

from ..engine.results import ToolResult
from ..errors import ApprovalDenied, BudgetExceeded, ProviderError
from ..harness.approvals import ApprovalPolicy, ApprovalRequest, Approver, AutoDeny
from ..harness.budget import Budget, BudgetTracker
from ..harness.retry import retry_async
from ..hooks import Middleware, ToolInvocation, chain_llm, chain_tool
from ..llm import get_provider
from ..llm.pricing import cost_usd
from ..llm.types import (
    ChatMessage,
    ImageBlock,
    LLMRequest,
    LLMResponse,
    Provider,
    TextBlock,
    ToolCall,
    ToolResultBlock,
    ToolSpecParam,
    Usage,
)
from ..observability import tracing as tr
from ..observability.cost import CostMeter
from ..observability.tracing import NULL_TRACER, Tracer
from ..tools.base import Tool, ToolContext, Toolset, engine_tool
from .role import Role

if TYPE_CHECKING:  # pragma: no cover
    from ..engine.client import AgentSession, AsyncEngine
    from ..memory.memory import Memory

StopKind = Literal["end_turn", "max_turns", "budget", "refusal", "max_tokens", "error", "cancelled"]
_MAX_RESULT_CHARS = 60_000
# Engine tools an autonomous agent should not drive itself (they belong to the loop runner).
LOOP_CONTROL = frozenset(
    {
        "studio_loop_start",
        "studio_loop_advance",
        "studio_loop_stop",
        "studio_loop_define",
        "studio_team_template",
        "studio_agent_define",
        "studio_agent_remove",
    }
)


class ToolCallRecord(BaseModel):
    id: str
    name: str
    args: dict[str, Any] = Field(default_factory=dict)
    is_error: bool = False
    summary: str = ""
    seconds: float = 0.0


class AgentResult(BaseModel):
    agent: str
    text: str = ""
    stop_reason: StopKind = "end_turn"
    turns: int = 0
    usage: Usage = Field(default_factory=Usage)
    cost_usd: float = 0.0
    tool_calls: list[ToolCallRecord] = Field(default_factory=list)
    error: str = ""
    model: str = ""
    seconds: float = 0.0
    messages: list[ChatMessage] = Field(default_factory=list)

    @property
    def ok(self) -> bool:
        return self.stop_reason == "end_turn" and not self.error


class Agent:
    """A configurable tool-use agent. Every knob has a sensible default; see docs/PYTHON_AGENTS.md."""

    def __init__(
        self,
        role: Role | str,
        *,
        engine: AsyncEngine | None = None,
        provider: Provider | str | None = None,
        model: str | None = None,
        tools: Sequence[Tool] | Toolset = (),
        engine_tools: bool | Sequence[str] = True,
        memory: Memory | None = None,
        recall_k: int = 6,
        log_episodes: bool = True,
        approver: Approver | None = None,
        policy: ApprovalPolicy | None = None,
        budget: Budget | None = None,
        budget_tracker: BudgetTracker | None = None,
        middleware: Sequence[Middleware] = (),
        tracer: Tracer | None = None,
        cost_meter: CostMeter | None = None,
        effort: Literal["low", "medium", "high", "xhigh", "max"] = "high",
        max_tokens: int = 64000,
        max_turns: int | None = None,
        report_usage: bool = True,
        presence: bool = True,
        system_extra: str = "",
        loop_member: bool = False,
        llm_attempts: int = 3,
        tool_timeout: float | None = 600,
        llm_timeout: float | None = None,
    ) -> None:
        self.role = role if isinstance(role, Role) else Role.adhoc(role)
        self._role_given = isinstance(role, Role)
        self.engine = engine
        self._provider_spec = provider
        self.provider: Provider | None = provider if provider is not None and not isinstance(provider, str) else None
        self.model = model
        self.extra_tools = Toolset(tools if isinstance(tools, Toolset) else list(tools))
        self.engine_tools = engine_tools
        self.memory = memory
        self.recall_k = recall_k
        self.log_episodes = log_episodes
        self.approver = approver
        self.policy = policy
        self.tracker = (
            budget_tracker.child(budget, f"agent:{self.role.id}")
            if budget_tracker
            else BudgetTracker(budget, scope=f"agent:{self.role.id}")
        )
        self.middleware = list(middleware)
        self.tracer = tracer or NULL_TRACER
        self.cost_meter = cost_meter or CostMeter()
        self.effort = effort
        self.max_tokens = max_tokens
        self.max_turns = max_turns
        self.report_usage = report_usage
        self.presence = presence
        self.system_extra = system_extra
        self.loop_member = loop_member
        self.llm_attempts = llm_attempts
        self.tool_timeout = tool_timeout
        self.llm_timeout = llm_timeout
        self.toolset = Toolset()
        self.history: list[ChatMessage] = []
        self._ready = False
        self.session: AgentSession | None = engine.as_agent(self.role.id) if engine is not None else None

    @property
    def id(self) -> str:
        return self.role.id

    # ------------------------------------------------------------------ setup
    async def setup(self) -> Agent:
        """Resolves the role from the engine, the provider, the toolset and the approval policy."""
        if self._ready:
            return self
        if self.engine is not None and not self.role.system_prompt:
            with contextlib.suppress(Exception):
                fetched = await Role.from_engine(self.engine, self.role.id, loop_member=self.loop_member)
                if self._role_given:  # keep explicit overrides, take the engine's prompt and tools
                    fetched = fetched.model_copy(
                        update={
                            k: v
                            for k, v in self.role.model_dump().items()
                            if v and k not in ("system_prompt", "brief_tools", "on_roster")
                        }
                    )
                self.role = fetched
        if self.provider is None:
            spec = self._provider_spec if isinstance(self._provider_spec, str) else (self.role.provider or "anthropic")
            if spec in ("mock",):
                spec = "scripted"
            self.provider = get_provider(spec)
        if self.model is None:
            if self.role.model:
                self.model = self.role.model
            elif self.provider.name == "anthropic":
                self.model = self.role.default_model or self.provider.default_model
            else:
                self.model = self.provider.default_model
        if self.max_turns is None:
            self.max_turns = self.role.max_rounds
        if self.policy is None:
            self.policy = ApprovalPolicy(autonomy=self.role.autonomy, ask_tools=self.role.ask_tools())
        if self.memory is not None and self.memory.agent != self.role.id:
            self.memory = self.memory.for_agent(self.role.id, self.role.teams, session=self.session)
        await self._build_toolset()
        self._ready = True
        return self

    async def _build_toolset(self) -> None:
        tools = Toolset()
        if self.engine is not None and self.engine_tools and self.session is not None:
            specs = {s.name: s for s in await self.engine.list_tools()}
            if isinstance(self.engine_tools, bool):
                permitted = self.role.permitted_tools() or [n for n, s in specs.items() if s.category not in ("agent",)]
            else:
                permitted = list(self.engine_tools)
            for name in permitted:
                spec = specs.get(name)
                if spec is None or (self.loop_member and name in LOOP_CONTROL):
                    continue
                tools.add(engine_tool(self.session, spec.model_dump(by_alias=True)))
        if self.memory is not None:
            tools.extend(self.memory.tools())
        tools.extend(self.extra_tools)
        assert self.policy is not None
        self.toolset = Toolset(t for t in tools if self.policy.allowed(t))

    def system_prompt(self) -> str:
        prompt = self.role.local_system_prompt()
        if self.memory is not None:
            prompt += (
                "\n\nYou have long-term memory tools (memory_recall, memory_remember): recall before you start, "
                "and remember durable facts and decisions others will need."
            )
        if self.system_extra:
            prompt += "\n\n" + self.system_extra
        return prompt

    # ------------------------------------------------------------------ running
    async def run(
        self, task: str, *, images: Sequence[bytes] = (), context: str = "", keep_history: bool = False
    ) -> AgentResult:
        """Works on ``task`` until the model stops calling tools (or a limit is hit)."""
        await self.setup()
        assert self.provider is not None and self.model is not None and self.max_turns is not None
        started = time.monotonic()
        result = AgentResult(agent=self.id, model=self.model)
        with self.tracer.span(
            f"invoke_agent {self.id}",
            "agent",
            **{
                tr.OPERATION: "invoke_agent",
                tr.AGENT_NAME: self.role.display,
                tr.AGENT_ID: self.id,
                tr.REQUEST_MODEL: self.model,
                tr.PROVIDER: self.provider.name,
            },
        ) as span:
            span.set("skywalker.task", self.tracer.preview(task))
            await self._presence("working", task.splitlines()[0][:150] if task else "")
            try:
                prompt = task
                if self.memory is not None and self.recall_k > 0:
                    hits = await self.memory.recall(task, k=self.recall_k)
                    block = self.memory.context_block(hits)
                    if block:
                        prompt = f"{task}\n\n{block}"
                if context:
                    prompt = f"{prompt}\n\n{context}"
                messages = [*(self.history if keep_history else []), ChatMessage.user(prompt, list(images))]
                await self._loop(messages, result)
                result.messages = messages
                if keep_history:
                    self.history = messages
            except BudgetExceeded as e:
                result.stop_reason, result.error = "budget", str(e)
            except asyncio.CancelledError:
                result.stop_reason, result.error = "cancelled", "cancelled"
                raise
            except ProviderError as e:
                result.stop_reason, result.error = "error", str(e)
            finally:
                result.seconds = time.monotonic() - started
                span.update(
                    {
                        tr.INPUT_TOKENS: result.usage.input_tokens,
                        tr.OUTPUT_TOKENS: result.usage.output_tokens,
                        tr.COST: round(result.cost_usd, 6),
                        "skywalker.turns": result.turns,
                        "skywalker.stop_reason": result.stop_reason,
                        "skywalker.result": self.tracer.preview(result.text),
                    }
                )
                if result.error:
                    span.fail(result.error)
                await self._presence("idle", "")
                await self._log_episode(task, result)
        return result

    async def chat(self, message: str, *, images: Sequence[bytes] = ()) -> AgentResult:
        """One turn of a continuing conversation (history kept between calls)."""
        return await self.run(message, images=images, keep_history=True)

    async def _loop(self, messages: list[ChatMessage], result: AgentResult) -> None:
        assert self.provider is not None and self.model is not None and self.max_turns is not None
        call_llm = chain_llm(self.middleware, self._call_provider)
        call_tool = chain_tool(self.middleware, self._run_tool)
        tools = [ToolSpecParam(**t.spec()) for t in self.toolset]
        while True:
            if result.turns >= self.max_turns:
                result.stop_reason = "max_turns"
                result.error = f"stopped after {result.turns} turns (max_turns)"
                return
            self.tracker.check()
            request = LLMRequest(
                model=self.model,
                system=self.system_prompt(),
                messages=list(messages),
                tools=tools,
                max_tokens=self.max_tokens,
                effort=self.effort,
                metadata={"agent": self.id, "turn": result.turns},
            )
            response = await retry_async(functools.partial(call_llm, request), attempts=self.llm_attempts)
            result.turns += 1
            self.tracker.charge_turn()
            cost = self.cost_meter.add(self.id, response.model or self.model, response.usage)
            result.usage = result.usage + response.usage
            result.cost_usd += cost
            self.tracker.charge_llm(response.usage, cost)
            await self._report_usage(response)
            if response.stop_reason == "refusal":
                result.stop_reason = "refusal"
                result.error = f"the model declined ({response.refusal_category or 'no category'})"
                return
            messages.append(response.as_message())
            if response.stop_reason == "pause_turn":
                continue  # server-side tool work paused: resend to resume
            calls = response.tool_calls
            if response.stop_reason == "max_tokens":
                if calls:  # a cut-off tool call is never run
                    messages.append(
                        ChatMessage(
                            role="user",
                            content=[
                                ToolResultBlock(
                                    tool_call_id=c.id,
                                    name=c.name,
                                    is_error=True,
                                    content=[
                                        TextBlock(
                                            text="Your output hit max_tokens before this call was complete, "
                                            "so it was not run. Re-issue it (keep arguments smaller)."
                                        )
                                    ],
                                )
                                for c in calls
                            ],
                        )
                    )
                    continue
                result.text = response.text
                result.stop_reason = "max_tokens"
                return
            if not calls:
                result.text = response.text
                result.stop_reason = "end_turn"
                return
            blocks = await asyncio.gather(*(self._execute(c, call_tool, result) for c in calls))
            messages.append(ChatMessage(role="user", content=list(blocks)))

    async def _call_provider(self, request: LLMRequest) -> LLMResponse:
        assert self.provider is not None
        with self.tracer.span(
            f"chat {request.model}",
            "llm",
            **{
                tr.OPERATION: "chat",
                tr.PROVIDER: self.provider.name,
                tr.SYSTEM: self.provider.name,
                tr.REQUEST_MODEL: request.model,
                tr.MAX_TOKENS: request.max_tokens,
                tr.AGENT_ID: self.id,
            },
        ) as span:
            coro = self.provider.complete(request)
            response = await (asyncio.wait_for(coro, self.llm_timeout) if self.llm_timeout else coro)
            span.update(
                {
                    tr.RESPONSE_MODEL: response.model,
                    tr.FINISH_REASONS: [response.stop_reason],
                    tr.INPUT_TOKENS: response.usage.input_tokens,
                    tr.OUTPUT_TOKENS: response.usage.output_tokens,
                    tr.CACHE_READ_TOKENS: response.usage.cache_read_tokens,
                    tr.CACHE_WRITE_TOKENS: response.usage.cache_write_tokens,
                    tr.COST: round(cost_usd(response.model or request.model, response.usage), 6),
                    "skywalker.tool_calls": [c.name for c in response.tool_calls],
                    "skywalker.completion": self.tracer.preview(response.text),
                }
            )
            return response

    async def _execute(self, call: ToolCall, call_tool: Any, result: AgentResult) -> ToolResultBlock:
        t0 = time.monotonic()
        record = ToolCallRecord(id=call.id, name=call.name, args=call.input)
        out = await self._guarded(call, call_tool)
        record.is_error = out.is_error
        record.summary = out.text[:200]
        record.seconds = time.monotonic() - t0
        result.tool_calls.append(record)
        text = out.text
        if len(text) > _MAX_RESULT_CHARS:
            text = text[:_MAX_RESULT_CHARS] + f"\n… (truncated {len(text) - _MAX_RESULT_CHARS} chars)"
        content: list[Any] = [TextBlock(text=text or ("(no output)" if not out.is_error else "error"))]
        if self.provider is not None and self.provider.supports_vision:
            content += [
                ImageBlock(data=b.data or "", media_type=b.mimeType or "image/png")
                for b in out.content
                if b.type == "image" and b.data
            ]
        return ToolResultBlock(tool_call_id=call.id, name=call.name, content=content, is_error=out.is_error)

    async def _guarded(self, call: ToolCall, call_tool: Any) -> ToolResult:
        tool = self.toolset.get(call.name)
        if tool is None:
            return ToolResult.error(
                "not_permitted",
                f"the tool {call.name} is not available to you",
                "use one of your listed tools, or ask the human / message the owner",
            )
        if call.invalid_json:
            return ToolResult.error(
                "INVALID_JSON",
                f"your arguments were not valid JSON: {call.raw_input[:300]}",
                "re-issue the call with a JSON object",
            )
        assert self.policy is not None
        if self.policy.needs_approval(tool):
            approver = self.approver or AutoDeny()
            decision = await approver.request(ApprovalRequest(agent=self.id, tool=tool.name, args=call.input))
            if not decision.approved:
                return ToolResult.error(
                    "declined",
                    f"{decision.by or 'the human'} declined {tool.name}"
                    + (f": {decision.note}" if decision.note else ""),
                    "try something else, or explain why it is needed",
                )
        if self.tracker.would_exceed_tools():
            raise BudgetExceeded(
                self.tracker.scope, "tool_calls", self.tracker.tool_calls + 1, self.tracker.budget.max_tool_calls or 0
            )
        self.tracker.charge_tool()
        ctx = ToolContext(
            agent_id=self.id,
            actor=self.session.actor if self.session else self.id,
            call_id=call.id,
            engine=self.engine,
            session=self.session,
            memory=self.memory,
        )
        inv = ToolInvocation(tool=tool, args=dict(call.input), ctx=ctx, call_id=call.id)
        with self.tracer.span(
            f"execute_tool {tool.name}",
            "tool",
            **{
                tr.OPERATION: "execute_tool",
                tr.TOOL_NAME: tool.name,
                tr.TOOL_CALL_ID: call.id,
                tr.TOOL_TYPE: "function",
                tr.AGENT_ID: self.id,
                "skywalker.tool.source": tool.source,
            },
        ) as span:
            span.set("skywalker.tool.args", self.tracer.preview(json.dumps(call.input, default=str)))
            try:
                coro = call_tool(inv)
                out: ToolResult = await (asyncio.wait_for(coro, self.tool_timeout) if self.tool_timeout else coro)
            except asyncio.TimeoutError:
                out = ToolResult.error("timeout", f"{tool.name} took longer than {self.tool_timeout:.0f} s")
            except ApprovalDenied as e:
                out = ToolResult.error("declined", str(e))
            span.set("skywalker.tool.is_error", out.is_error)
            span.set("skywalker.tool.result", self.tracer.preview(out.text))
            if out.is_error:
                span.fail(out.error_code or "tool_error")
        return out

    async def _run_tool(self, inv: ToolInvocation) -> ToolResult:
        return await inv.tool.run(inv.args, inv.ctx)

    # ------------------------------------------------------------------ studio integration
    async def _presence(self, status: str, activity: str) -> None:
        if not self.presence or self.session is None or not self.role.on_roster:
            return
        with contextlib.suppress(Exception):
            await self.session.call("studio_presence", {"status": status, "activity": activity})

    async def _report_usage(self, response: LLMResponse) -> None:
        if not self.report_usage or self.session is None or not self.role.on_roster:
            return
        u = response.usage
        with contextlib.suppress(Exception):
            await self.session.call(
                "studio_usage_report",
                {
                    "model": response.model or self.model or "",
                    "input_tokens": u.input_tokens,
                    "output_tokens": u.output_tokens,
                    "cache_read_tokens": u.cache_read_tokens,
                    "cache_write_tokens": u.cache_write_tokens,
                },
            )

    async def _log_episode(self, task: str, result: AgentResult) -> None:
        if self.memory is None or not self.log_episodes:
            return
        with contextlib.suppress(Exception):
            tools = sorted({c.name for c in result.tool_calls})
            await self.memory.log(
                f"Task: {task[:300]}\nOutcome ({result.stop_reason}): {result.text[:600]}"
                + (f"\nTools: {', '.join(tools)}" if tools else ""),
                tags=["run"],
                importance=0.3,
            )

    def __repr__(self) -> str:
        return f"Agent({self.id!r}, model={self.model!r})"
