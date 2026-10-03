---
description: "Show the Skywalker studio at a glance - roster, board, feedback awaiting decisions, loops, latest playtest metrics, usage"
argument-hint: "\"[optional focus, e.g. 'feedback' or 'board']\""
---

Report the state of the Skywalker studio. Focus (if any): **$ARGUMENTS**

1. `studio_overview {}` for the whole picture.
2. Depending on the focus (or all of it, briefly): `studio_task_list {status:"open"}` (board as a compact kanban), `studio_feedback_list {status:"needs_decision"}` and `{status:"active"}` (verdicts and measured effects), `studio_loop_status {}` (loops and their state), `studio_inbox {unread_only:true}` (messages for you).
3. Summarize in under 25 lines: who is doing what, what is blocked, which feedback needs a decision and how old it is, loop status and metric trend, cost so far, and the single most valuable next action. Do not change anything.
