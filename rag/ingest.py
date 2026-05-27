#!/usr/bin/env python3
"""
Ingest all Vertical Rectangle / Epsilver project markdown docs into Chroma.
Run once (or re-run to refresh after doc updates).

Usage:
    pip install -r rag/requirements.txt
    python3 rag/ingest.py
"""

import os
import re
from pathlib import Path

import chromadb
from sentence_transformers import SentenceTransformer

# ── Config ─────────────────────────────────────────────────────────────────────

DEV = Path.home() / "dev"

PROJECTS = [
    "pop-maker-studio",
    "silvertune",
    "silvertune-pedal",
    "silvertune-web",
    "lyric-video-blender",
    "keybpm",
    "vertrect-site",
    "Wickrunner",
    "epsilver-site",
    "epsilver-cei",
    "cei-cli",
]

CHROMA_PATH = Path(__file__).parent / "chroma_db"
COLLECTION  = "vr_docs"
MODEL_NAME  = "all-MiniLM-L6-v2"
MIN_CHUNK   = 80   # characters — skip trivially short sections


# ── Chunking ───────────────────────────────────────────────────────────────────

def chunk_markdown(text: str) -> list[str]:
    """Split markdown on headers. Each header + its body is one chunk."""
    parts = re.split(r"(?=\n#{1,3} )", text)
    chunks = []
    for part in parts:
        part = part.strip()
        if len(part) >= MIN_CHUNK:
            chunks.append(part)
    return chunks


# ── Ingest ─────────────────────────────────────────────────────────────────────

def main():
    print(f"Loading embedding model: {MODEL_NAME}")
    model = SentenceTransformer(MODEL_NAME)

    client = chromadb.PersistentClient(path=str(CHROMA_PATH))

    # Fresh collection each run
    try:
        client.delete_collection(COLLECTION)
    except Exception:
        pass
    collection = client.create_collection(COLLECTION)

    docs, ids, metas = [], [], []
    chunk_idx = 0

    for project in PROJECTS:
        project_dir = DEV / project
        if not project_dir.exists():
            print(f"  skip {project} (not found)")
            continue

        md_files = sorted(project_dir.glob("*.md"))
        if not md_files:
            print(f"  skip {project} (no .md files)")
            continue

        for md_file in md_files:
            text = md_file.read_text(errors="ignore")
            chunks = chunk_markdown(text)
            for chunk in chunks:
                docs.append(chunk)
                ids.append(f"chunk_{chunk_idx}")
                metas.append({
                    "project": project,
                    "file": md_file.name,
                })
                chunk_idx += 1

        print(f"  {project}: {len(md_files)} files, {chunk_idx} chunks total")

    if not docs:
        print("No documents found.")
        return

    print(f"\nEmbedding {len(docs)} chunks...")
    embeddings = model.encode(docs, show_progress_bar=True).tolist()

    print("Storing in Chroma...")
    # Chroma has a max batch size
    batch = 500
    for i in range(0, len(docs), batch):
        collection.add(
            documents=docs[i:i+batch],
            embeddings=embeddings[i:i+batch],
            ids=ids[i:i+batch],
            metadatas=metas[i:i+batch],
        )

    print(f"\nDone. {len(docs)} chunks indexed into {CHROMA_PATH}")


if __name__ == "__main__":
    main()
