# sky-agents-hazards: an example plugin

```bash
pip install -e python/examples/custom_plugin      # next to skywalker-agents
sky-agents plugins                                # hazard_audit, spend_limit, fairness_review, hazard_designer
```

Use it from Python:

```python
from skywalker_agents import Agent, registry
from skywalker_agents.plugins import ensure_builtins

ensure_builtins()  # loads installed plugins
audit = registry.tools.get("hazard_audit")
mira = Agent("mira", engine=engine, tools=[audit], middleware=[registry.hooks.get("spend_limit")(max_edits=10)])
await engine.host_tools([audit])  # every agent on the engine can call py_hazard_audit
```

or from a workflow spec: `{id: review, pattern: fairness_review, args: {designer: mira, critic: vera}}`.

Start your own with `sky-agents new plugin NAME`.
