# Reference

The reference is generated from the engine itself, never written by hand: the tool registry, the reflected component
schemas, the Wander builtin registry, the CLI's help and the C API header. A test in the engine's suite fails when the
committed pages fall behind the code, so what you read here is what the engine accepts.

| Reference | Generated from | Contents |
|---|---|---|
| [Tools](tools/index.md) | `skywalker tools --json` | Every tool by category: description, argument table with types, enums and ranges, MCP annotations, and the same example as a tool call, a CLI command and an MCP request |
| [Components](components/index.md) | `component_schema` | Every component and the environment: fields, types, ranges, enums, documentation |
| [Wander builtins](wander.md) | `wander_reference` | Every builtin function by category with signature, parameters, return type and example, plus subsystem triggers |
| [Command line](cli.md) | the CLI's help | `skywalker` commands and options |
| [C API](capi.md) | `engine/capi/include/sky_api.h` | The stable C ABI used by the editor and any FFI |

## Regenerating

Contributors who change a tool, component, builtin or CLI flag regenerate the data and the pages:

```bash
cmake --build --preset release --target skywalker
python3 website/scripts/dump_data.py --cli build/release/bin/skywalker
python3 website/scripts/gen_reference.py
python3 website/scripts/check_snippets.py --cli build/release/bin/skywalker
```

See the [website README](https://github.com/amirhossein-razlighi/Skywalker/blob/main/website/README.md) for details.

## Conventions used throughout

| Convention | Value |
|---|---|
| Units | Meters, seconds, degrees; masses in kilograms |
| Axes | +Y up; entities face −Z; rotations are Euler degrees `[pitch(X), yaw(Y), roll(Z)]` |
| Colors | `"#rrggbb"` / `"#rrggbbaa"` strings or `[r, g, b(, a)]` in 0..1; emissive alpha is strength (HDR) |
| Entity references | A numeric id (stable, never reused) or an exact name |
| Paths | Relative to the project folder; models as `asset:<path>`, prefabs as `prefab:<path>` in Wander `spawn` |
| Time | Fixed 1/60 s ticks; 60 ticks are one second |
| Errors | A stable `snake_case` code, a message and a hint (often *did you mean …?*) |
