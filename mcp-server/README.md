# walshard-mcp

**MCP bridge for Walshard Studio.** Drive the cocos2dx scene editor
from Claude Code, Claude Desktop, Cursor, or any other Model Context
Protocol client.

This package wraps the editor's TCP line protocol (port 9876) as a
set of MCP tools served over stdio. Every editor mutation goes
through the same undo stack a human user works with — an agent's
mistake is one tool call away from being unmade.

## Requirements

- Python **3.10+**
- A running Walshard Studio instance (the MCP server connects to TCP
  127.0.0.1:9876; override with `OCS_INSPECT_PORT`).

## Install

```bash
# Inside the walshardstudio repo:
cd mcp-server
pip install -e .
```

Or, for a hermetic deployment that Claude Desktop can spawn directly,
use `uv`:

```bash
uv tool install --from /absolute/path/to/mcp-server walshard-mcp
```

## Run

```bash
walshard-mcp          # stdio transport (the only one supported today)
```

The server logs to **stderr**; never write to stdout — that channel
carries JSON-RPC.

## Wire to Claude Code

```bash
claude mcp add walshard -- walshard-mcp
```

Or, if `walshard-mcp` isn't on `PATH`:

```bash
claude mcp add walshard -- python -m walshard_mcp
```

## Wire to Claude Desktop

Edit `~/Library/Application Support/Claude/claude_desktop_config.json`
(macOS) or `%APPDATA%\Claude\claude_desktop_config.json` (Windows):

```json
{
  "mcpServers": {
    "walshard": {
      "command": "/absolute/path/to/.venv/bin/walshard-mcp",
      "env": {
        "OCS_INSPECT_PORT": "9876"
      }
    }
  }
}
```

Quit and relaunch Claude Desktop (full quit, not just close window —
it caches server defs at launch).

## Environment

| Var | Default | Purpose |
|---|---|---|
| `OCS_INSPECT_PORT` | `9876` | TCP port the editor listens on |
| `WALSHARD_HOST` | `127.0.0.1` | Override if running the editor over the network (not recommended — no TLS) |
| `OCS_INSPECT_TOKEN` | unset | Pre-shared auth token; passed via `auth <token>` before commands when set on both sides |
| `WALSHARD_TIMEOUT_S` | `10` | Per-call socket timeout |

## Tools shipped (MVP)

**Inspection:** `describe`, `tree`, `tree_since`, `selected`, `getattr_`,
`listbindings`

**Mutation:** `select`, `setattr_`, `setcolor`, `setvisible`, `setfile`,
`setlabel`, `addchild`, `delete`, `addbinding`, `delbinding`

**Visual feedback:** `screenshot`, `screenshot_canvas`, `preview_visual`

**Lifecycle:** `validate`, `save`, `export_csb`, `undo`, `redo`, `mode`

**Escape hatch:** `raw` (send an arbitrary command line — useful for
commands the typed tools above don't yet cover, e.g. `apply-to-scenes`,
`gen-controller`, `svg-bake`, `vmap-add-layer`, `litsprite-add-light`,
the per-window `pan`/`zoom`/`rotate` controls, etc.).

Run `describe` first — it lists every command the running editor
build understands.

## Troubleshooting

- **"connection refused"**: editor is not running. Launch
  `Walshard.app` (open a `.csd` if you want a target scene) first.
- **"editor error: not authenticated"**: editor was launched with
  `OCS_INSPECT_TOKEN`; set the same value in the MCP server's env.
- **Claude Desktop shows the server as red/failed**: tail
  `~/Library/Logs/Claude/mcp-server-walshard.log`; the SDK logs every
  startup error there.
- **Tool calls hang for ~10 s then fail**: bump `WALSHARD_TIMEOUT_S`,
  or check for a long-running editor command (e.g. `svg-bake` on a
  large SVG).

## License

PolyForm Noncommercial 1.0.0 — same as the editor. See the top-level
`LICENSE` file in this repository.
