# playtest-loop

Run the playtest, triage, fix, verify loop with studio roster members as subagents until the targets hold.

Drive a studio loop for this goal: **the arguments the user typed after the command**

Load `skywalker-studio` first (identity, loops, playtests) and follow it exactly.

1. `studio_overview {include_catalog:true}`. If the roster is empty or lacks the roles needed (creative_director, level_designer, gameplay_programmer, environment_artist, playtester, critic), run the `studio-setup` command.
2. Make sure the game is measurable (player tagged `player`, goals `goal`, hazards `hazard`, events emitted). If not, fix that first or assign it to the gameplay programmer.
3. `studio_loop_define {template:"playtest_fix_verify", goal:"<the goal, with numbers>", stop:{max_iterations:3, metric_targets:{...}}}` (reuse an existing loop if one fits).
4. `studio_loop_start {loop}`. For every returned assignment, launch the matching roster subagent (same studio id) with its `prompt`; run `parallel` assignments concurrently. Each subagent adopts its role (`studio_agent_brief`), passes `as`, and returns a short report.
5. `studio_loop_advance {loop, reports:[...]}` with the collected reports. If the status is `awaiting_approval`, show the human what is pending and ask; then `approve:true`.
6. Repeat until `done` or `stopped`. Finish with a summary from `studio_loop_status {loop, history:true}`: iterations, playtest metrics trend, verified vs regressed feedback, open items, and your recommendation.

Never decide feedback yourself unless you are acting for the human; the creative director subagent does that. Keep each iteration small and undoable.
