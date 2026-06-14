#!/usr/bin/env bash
# Pre-commit guard for the agent tool surface. Install once:
#     ln -sf ../../tools/pre-commit-hook.sh .git/hooks/pre-commit
#
# Fails the commit if:
#   1. src/generated/agent_tools.h is out of sync with mcp_server/server.py, or
#   2. an IPC method in src/ipc_server.cpp is unreachable by any agent.
# Both checks no-op gracefully if the python `mcp` deps aren't installed.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

python3 tools/gen_agent_tools.py --check
python3 tools/check_tool_coverage.py
