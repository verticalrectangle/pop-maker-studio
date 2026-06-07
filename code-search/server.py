#!/usr/bin/env python3
"""
Pop Maker Studio code search — MCP server.
Exposes search_code so Claude can semantically search the source tree.

Usage:
    python3 ingest.py      # build index first
    python3 server.py      # then start this (Claude Code does this automatically)
"""

import asyncio
from pathlib import Path

import chromadb
from sentence_transformers import SentenceTransformer
from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp.types import Tool, TextContent

# ── Config ─────────────────────────────────────────────────────────────────────

CHROMA_PATH = Path(__file__).parent / "chroma_db"
COLLECTION  = "pms_code"
MODEL_NAME  = "all-MiniLM-L6-v2"
TOP_N       = 5

# ── Load on startup ────────────────────────────────────────────────────────────

print("Loading embedding model...", flush=True)
_model = SentenceTransformer(MODEL_NAME)

if not CHROMA_PATH.exists():
    raise RuntimeError("Index not found. Run `python3 ingest.py` first.")

_client     = chromadb.PersistentClient(path=str(CHROMA_PATH))
_collection = _client.get_collection(COLLECTION)
print(f"Loaded {_collection.count()} chunks from {CHROMA_PATH}", flush=True)

# ── MCP server ─────────────────────────────────────────────────────────────────

server = Server("pms-code-search")


@server.list_tools()
async def list_tools() -> list[Tool]:
    return [
        Tool(
            name="search_code",
            description=(
                "Semantic search over the pop-maker-studio C++ and Python source tree. "
                "Returns the most relevant code chunks with file path and line number. "
                "Use this before reading files to locate the right section quickly."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "query": {
                        "type": "string",
                        "description": "Plain language description of what you're looking for",
                    },
                    "n": {
                        "type": "integer",
                        "description": f"Number of chunks to return (default {TOP_N})",
                        "default": TOP_N,
                    },
                },
                "required": ["query"],
            },
        )
    ]


@server.call_tool()
async def call_tool(name: str, arguments: dict) -> list[TextContent]:
    if name != "search_code":
        raise ValueError(f"Unknown tool: {name}")

    query = arguments["query"]
    n     = int(arguments.get("n", TOP_N))

    embedding = _model.encode([query]).tolist()
    results   = _collection.query(
        query_embeddings=embedding,
        n_results=min(n, _collection.count()),
        include=["documents", "metadatas", "distances"],
    )

    docs      = results["documents"][0]
    metas     = results["metadatas"][0]
    distances = results["distances"][0]

    parts = []
    for doc, meta, dist in zip(docs, metas, distances):
        similarity = round(1 - dist, 3)
        parts.append(
            f"[{meta['file']}:{meta['line']}] (similarity: {similarity})\n```\n{doc}\n```"
        )

    return [TextContent(type="text", text="\n\n---\n\n".join(parts))]


# ── Entry point ────────────────────────────────────────────────────────────────

async def main():
    async with stdio_server() as (read_stream, write_stream):
        await server.run(read_stream, write_stream, server.create_initialization_options())


if __name__ == "__main__":
    asyncio.run(main())
