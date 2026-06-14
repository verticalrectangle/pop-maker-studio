#!/usr/bin/env python3
"""Fail if any IPC method in src/ipc_server.cpp is not reachable by an agent.

Every method the editor socket serves should be exposed as an MCP tool, OR be a
documented alias target (a friendly MCP verb wraps it), OR be on the explicit
INTERNAL_IPC allowlist (the agent's own loop, debug hooks, status pollers folded
into blocking tools). Anything else is an accidental gap — a capability the agent
can't reach — and this script flags it so new IPC methods can't silently rot.

The alias map and the internal allowlist live in mcp_server/server.py so they are
maintained in exactly one place (#7). Run from the repo root:
    python3 tools/check_tool_coverage.py
Exit 1 on any uncovered method. Use --list to print the full coverage table.
"""
import asyncio
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IPC = ROOT / "src" / "ipc_server.cpp"
sys.path.insert(0, str(ROOT / "mcp_server"))

import server  # noqa: E402  (path set above)


def main() -> int:
    ipc_methods = set(re.findall(r'method == "([a-zA-Z0-9_]+)"', IPC.read_text()))
    if not ipc_methods:
        print("error: no IPC methods found — wrong path?", file=sys.stderr)
        return 2

    tools = asyncio.run(server.list_tools())
    mcp_names = {t.name for t in tools}

    alias_targets = set(server.MCP_IPC_ALIASES.values())
    internal = set(server.INTERNAL_IPC)

    covered = mcp_names | alias_targets | internal
    gaps = sorted(ipc_methods - covered)

    # Reverse sanity: an alias target or internal entry that no longer exists as
    # an IPC method is dead config worth knowing about.
    stale_alias = sorted(alias_targets - ipc_methods)
    stale_internal = sorted(internal - ipc_methods)

    if "--list" in sys.argv:
        direct = sorted(m for m in ipc_methods if m in mcp_names)
        aliased = sorted(m for m in ipc_methods if m in alias_targets and m not in mcp_names)
        hidden = sorted(m for m in ipc_methods
                        if m in internal and m not in mcp_names | alias_targets)
        ipc_to_mcp = {ipc: mcp for mcp, ipc in server.MCP_IPC_ALIASES.items()}
        print(f"IPC methods: {len(ipc_methods)}   MCP tools: {len(mcp_names)}")
        print(f"\ndirect ({len(direct)}): {', '.join(direct)}")
        print(f"\naliased ({len(aliased)}): "
              + ", ".join(f"{ipc_to_mcp[m]}→{m}" for m in aliased))
        print(f"\ninternal/withheld ({len(hidden)}): {', '.join(hidden)}")

    ok = True
    if gaps:
        ok = False
        print("UNCOVERED IPC methods (no MCP tool, alias, or allowlist entry):",
              file=sys.stderr)
        for m in gaps:
            print(f"  - {m}", file=sys.stderr)
        print("\nFix by adding an MCP tool in server.py, an entry in MCP_IPC_ALIASES,\n"
              "or — if it is genuinely internal — adding it to INTERNAL_IPC.",
              file=sys.stderr)
    if stale_alias:
        ok = False
        print(f"\nstale MCP_IPC_ALIASES targets (no such IPC method): {stale_alias}",
              file=sys.stderr)
    if stale_internal:
        ok = False
        print(f"\nstale INTERNAL_IPC entries (no such IPC method): {stale_internal}",
              file=sys.stderr)

    if ok:
        print(f"tool coverage OK — all {len(ipc_methods)} IPC methods reachable "
              f"or explicitly internal.")
        return 0
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
