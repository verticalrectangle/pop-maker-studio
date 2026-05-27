#!/usr/bin/env python3
"""
Vertical Rectangle docs RAG — MCP server.
Exposes search_docs tool so Claude Code can query project documentation.

Usage:
    python3 rag/ingest.py          # build index first
    python3 rag/server.py          # then start this server
"""

from pathlib import Path

import chromadb
from sentence_transformers import SentenceTransformer
from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp import types
from mcp.types import Tool, TextContent

# ── Config ─────────────────────────────────────────────────────────────────────

CHROMA_PATH = Path(__file__).parent / "chroma_db"
COLLECTION  = "vr_docs"
MODEL_NAME  = "all-MiniLM-L6-v2"
TOP_N       = 5

# ── Load on startup ────────────────────────────────────────────────────────────

print("Loading embedding model...", flush=True)
_model = SentenceTransformer(MODEL_NAME)

if not CHROMA_PATH.exists():
    raise RuntimeError(
        "Chroma DB not found. Run `python3 rag/ingest.py` first."
    )

_client     = chromadb.PersistentClient(path=str(CHROMA_PATH))
_collection = _client.get_collection(COLLECTION)
print(f"Loaded {_collection.count()} chunks from {CHROMA_PATH}", flush=True)

# ── MCP server ─────────────────────────────────────────────────────────────────

server = Server("vr-docs")


@server.list_tools()
async def list_tools() -> list[Tool]:
    return [
        Tool(
            name="search_docs",
            description=(
                "Search Vertical Rectangle and Epsilver project documentation "
                "by semantic similarity. Returns the most relevant excerpts from "
                "READMEs and docs across all projects: Pop Maker Studio, Silvertune, "
                "Silvertune Web, Silvertune Pedal, Lyric Video Blender, Wickrunner, "
                "CEI, keybpm, and the studio/artist sites."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "query": {
                        "type": "string",
                        "description": "What to search for — plain language question or topic",
                    },
                    "n": {
                        "type": "integer",
                        "description": f"Number of results to return (default {TOP_N})",
                        "default": TOP_N,
                    },
                },
                "required": ["query"],
            },
        )
    ]


@server.call_tool()
async def call_tool(name: str, arguments: dict) -> list[TextContent]:
    if name != "search_docs":
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
            f"[{meta['project']} / {meta['file']}] (similarity: {similarity})\n{doc}"
        )

    return [TextContent(type="text", text="\n\n---\n\n".join(parts))]


# ── Entry point ────────────────────────────────────────────────────────────────

async def main():
    async with stdio_server() as (read_stream, write_stream):
        await server.run(read_stream, write_stream, server.create_initialization_options())


if __name__ == "__main__":
    import asyncio
    asyncio.run(main())
