"""Walshard Studio MCP server.

Bridges the editor's TCP line protocol (port 9876, override with
``OCS_INSPECT_PORT``) to the Model Context Protocol over stdio so that
Claude Code, Claude Desktop, Cursor, and any other MCP-aware client can
drive the editor as a set of tools.

The editor must be running before the MCP client invokes any tool;
each tool call opens a fresh TCP connection.

Boot:
    python -m walshard_mcp     # stdio transport, the default

Or via the console-script entry point declared in pyproject.toml:
    walshard-mcp
"""

from __future__ import annotations

import logging
import sys

from mcp.server.fastmcp import FastMCP

from .client import WalshardClient, WalshardClientConfig, WalshardError

# CRITICAL: stdio servers must never write to stdout (it corrupts the
# JSON-RPC stream). Send all logs to stderr.
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    stream=sys.stderr,
)
log = logging.getLogger("walshard-mcp")

mcp = FastMCP("walshard")

# Single shared client. Each call opens its own socket — see client.py.
_client = WalshardClient(WalshardClientConfig.from_env())


def _run(command: str) -> str:
    """Forward a raw command line to the editor. Surface ERR replies
    as MCP tool errors via exception so the model sees them as such."""
    log.debug("→ %s", command)
    try:
        reply = _client.send(command)
        log.debug("← %s", reply[:200] + ("…" if len(reply) > 200 else ""))
        return reply
    except WalshardError as e:
        # FastMCP turns raised exceptions into is_error tool results.
        raise RuntimeError(f"editor error: {e}") from e


# --- Inspection -----------------------------------------------------------

@mcp.tool()
def describe() -> str:
    """Return a high-level snapshot of the editor state: current scene
    path, mode (Edit/Play), selection, dirty flag, available panels."""
    return _run("describe")


@mcp.tool()
def tree() -> str:
    """Dump the full scene tree of the active document as an indented
    list of `path|ctype|name` triples."""
    return _run("tree")


@mcp.tool()
def tree_since(stamp_ms: int) -> str:
    """Return only the nodes whose `OCSModified` stamp is newer than
    ``stamp_ms``. Useful for incremental sync — the editor stamps every
    undo entry, so an agent can poll for diffs cheaply."""
    return _run(f"tree-since {int(stamp_ms)}")


@mcp.tool()
def selected() -> str:
    """Return the path of the currently-selected node, or empty if no
    selection."""
    return _run("selected")


@mcp.tool()
def getattr_(node_path: str, attr: str) -> str:
    """Read a single attribute from a node.

    Example: ``getattr_("sceneRoot/topToolbar", "Visible")``.
    """
    return _run(f"getattr {node_path} {attr}")


@mcp.tool()
def listbindings(node_path: str = "") -> str:
    """List event→method bindings for ``node_path`` (or the whole
    document when omitted)."""
    return _run(f"listbindings {node_path}".strip())


# --- Mutation -------------------------------------------------------------

@mcp.tool()
def select(node_path: str) -> str:
    """Set the editor selection to ``node_path``. Useful before
    follow-up `setattr` calls that operate on the selection."""
    return _run(f"select {node_path}")


@mcp.tool()
def setattr_(node_path: str, attr: str, value: str) -> str:
    """Write ``attr`` = ``value`` on ``node_path``. The editor validates
    the (attr, ctype) pair and records the change in the undo stack."""
    return _run(f"setattr {node_path} {attr} {value}")


@mcp.tool()
def setcolor(node_path: str, r: int, g: int, b: int, a: int = 255) -> str:
    """Set the displayed color of a node. Components are 0–255 ints."""
    return _run(f"setcolor {node_path} {r} {g} {b} {a}")


@mcp.tool()
def setvisible(node_path: str, visible: bool) -> str:
    """Toggle a node's `Visible` flag. Cascades through the ax-tree."""
    return _run(f"setvisible {node_path} {1 if visible else 0}")


@mcp.tool()
def setfile(node_path: str, file_path: str) -> str:
    """Swap a Sprite/ImageView's `FileData`. ``file_path`` is relative
    to the project's `Resources` root, the same convention Cocos Studio
    used."""
    return _run(f"setfile {node_path} {file_path}")


@mcp.tool()
def setlabel(node_path: str, text: str) -> str:
    """Set the displayed text of a Text/TextField node."""
    return _run(f"setlabel {node_path} {text}")


@mcp.tool()
def addchild(parent_path: str, ctype: str, name: str) -> str:
    """Add a new child node of type ``ctype`` (e.g. `SpriteObjectData`,
    `PanelObjectData`, `TextObjectData`, `VectorMapObjectData`) under
    ``parent_path``, named ``name``. Returns the new node's path."""
    return _run(f"addchild {parent_path} {ctype} {name}")


@mcp.tool()
def delete(node_path: str) -> str:
    """Remove a node and its subtree. Recorded in the undo stack."""
    return _run(f"delete {node_path}")


@mcp.tool()
def addbinding(node_path: str, event: str, method: str) -> str:
    """Attach an event binding (e.g. `Click`, `LongClick`) to a node.
    ``method`` is the C++ controller method name; `gen_controller` will
    emit a stub for it."""
    return _run(f"addbinding {node_path} {event} {method}")


@mcp.tool()
def delbinding(node_path: str, event: str) -> str:
    """Remove a binding from a node."""
    return _run(f"delbinding {node_path} {event}")


# --- Visual feedback ------------------------------------------------------

@mcp.tool()
def screenshot(out_path: str) -> str:
    """Capture the full editor window to ``out_path`` (PNG). Absolute
    path recommended; the editor writes synchronously."""
    return _run(f"screenshot {out_path}")


@mcp.tool()
def screenshot_canvas(out_path: str) -> str:
    """Capture only the canvas (the rendered scene, no chrome) to
    ``out_path`` (PNG)."""
    return _run(f"screenshot-canvas {out_path}")


@mcp.tool()
def preview_visual(node_path: str, out_path: str) -> str:
    """Render a single node + its subtree into ``out_path`` (PNG) at
    its design-time size. Useful when an agent wants to inspect one
    component without the whole scene context."""
    return _run(f"preview-visual {node_path} {out_path}")


# --- Lifecycle ------------------------------------------------------------

@mcp.tool()
def validate() -> str:
    """Validate the current document against the Cocos Studio schema +
    Walshard's extension types. Returns a list of issues or "ok"."""
    return _run("validate")


@mcp.tool()
def save() -> str:
    """Save the active document back to its `.csd` source. Sidecar
    JSON files (`*.vmaps.json`, `*.bindings.json`, `*.lights.json`) are
    written automatically when the scene contains the corresponding
    extension types."""
    return _run("save")


@mcp.tool()
def export_csb() -> str:
    """Compile the active document to its `.csb` binary in the
    project's `Resources` directory. The same path your game runtime
    will load."""
    return _run("export")


@mcp.tool()
def undo() -> str:
    """Reverse the last editor operation. The TCP RPC and the human UI
    share one undo stack — an agent's mistake is one call away from
    being unmade."""
    return _run("undo")


@mcp.tool()
def redo() -> str:
    """Re-apply the last undone operation."""
    return _run("redo")


@mcp.tool()
def mode(value: str) -> str:
    """Set editor mode. ``value`` is `Edit` (default — selection +
    handles visible) or `Play` (canvas runs animations and event
    handlers)."""
    return _run(f"mode {value}")


# --- Catch-all ------------------------------------------------------------

@mcp.tool()
def raw(command: str) -> str:
    """Send an arbitrary raw command string to the editor — escape
    hatch for commands not yet exposed as a typed tool. Returns the
    reply verbatim. Use sparingly; prefer the typed tools above."""
    return _run(command)


def main() -> None:
    log.info("walshard-mcp starting (target=%s:%d)",
             _client._cfg.host, _client._cfg.port)
    mcp.run(transport="stdio")


if __name__ == "__main__":
    main()
