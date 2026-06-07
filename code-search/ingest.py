#!/usr/bin/env python3
"""
Chunk and embed pop-maker-studio source code into a local Chroma vector store.
Run once, then re-run any time the source changes significantly.

Usage:
    python3 ingest.py
"""

import re
from pathlib import Path
import chromadb
from sentence_transformers import SentenceTransformer

# ── Config ─────────────────────────────────────────────────────────────────────

REPO_ROOT   = Path(__file__).parent.parent
SRC_DIRS    = ["src", "mcp_server"]
EXTENSIONS  = {".cpp", ".h", ".py"}
EXCLUDE     = {"vendor"}

CHROMA_PATH = Path(__file__).parent / "chroma_db"
COLLECTION  = "pms_code"
MODEL_NAME  = "all-MiniLM-L6-v2"

WINDOW      = 60   # lines per chunk
OVERLAP     = 15   # lines of overlap between chunks
MIN_LINES   = 10   # skip chunks shorter than this

# Control-flow keywords whose ){ we don't want to mistake for a function signature
_SKIP_KW = frozenset({"if", "else", "for", "while", "do", "switch", "try", "catch"})


# ── Context header ─────────────────────────────────────────────────────────────

def context_header(lines: list[str], chunk_start_0: int) -> str:
    """
    Scan backward from chunk_start_0 (0-indexed) with brace-depth tracking to
    find the enclosing function signature and class/struct/namespace line.

    Anchors on ){  (not column-0) so it works for methods, lambdas-as-args, etc.
    Grabs up to 5 lines before the opening brace to handle multi-line signatures.
    Also picks up the nearest enclosing class/struct/namespace { line.

    Stopping point: pure heuristic — no tree-sitter. Brace counting ignores
    string literals and comments, so deeply nested macro magic or raw-string
    literals with unbalanced braces will misfire. In practice this codebase
    doesn't do that; switch to tree-sitter if misfires become annoying.
    """
    depth    = 0
    func_sig = None
    class_sig = None

    i = chunk_start_0 - 1
    while i >= 0:
        line = lines[i]

        # Backward brace accounting: } takes us deeper into nested scopes,
        # { takes us out — when depth goes negative we've found the opening
        # brace of our enclosing scope.
        depth += line.count('}') - line.count('{')

        if depth < 0 and func_sig is None:
            # Collect this line plus up to 4 lines above it (multi-line sigs).
            sig_lines = [l.rstrip() for l in lines[max(0, i - 4): i + 1] if l.strip()]
            sig = " ".join(sig_lines)

            if re.search(r'\)\s*\{', sig):
                # Reject control-flow: first non-whitespace word must not be a keyword
                first_word = sig.lstrip().split()[0].lstrip('~') if sig.strip() else ""
                if first_word not in _SKIP_KW:
                    func_sig = sig.strip()

            depth = 0  # reset so we keep scanning for the enclosing class

        # Enclosing class / struct / namespace — grab and stop
        if re.match(r'\s*(class|struct|namespace)\b', line) and '{' in line:
            class_sig = line.strip().rstrip('{').strip()
            break

        i -= 1

    parts = []
    if class_sig:
        parts.append(class_sig)
    if func_sig:
        parts.append(func_sig)

    if not parts:
        return ""
    return "// context: " + " > ".join(parts) + "\n"


# ── Chunking ───────────────────────────────────────────────────────────────────

def chunk_file(path: Path) -> list[tuple[str, int]]:
    """Sliding window chunker with context-header prepend."""
    lines = path.read_text(errors="ignore").splitlines()
    chunks = []
    step = WINDOW - OVERLAP
    for i in range(0, max(1, len(lines) - MIN_LINES + 1), step):
        window = lines[i:i + WINDOW]
        if len(window) < MIN_LINES:
            break
        header = context_header(lines, i) if path.suffix in {".cpp", ".h"} else ""
        chunks.append((header + "\n".join(window), i + 1))  # 1-indexed line number
    return chunks


# ── Ingest ─────────────────────────────────────────────────────────────────────

def main():
    print(f"Loading embedding model: {MODEL_NAME}")
    model = SentenceTransformer(MODEL_NAME)

    client = chromadb.PersistentClient(path=str(CHROMA_PATH))
    try:
        client.delete_collection(COLLECTION)
    except Exception:
        pass
    collection = client.create_collection(COLLECTION)

    docs, ids, metas = [], [], []
    chunk_idx = 0
    file_count = 0

    for src_dir in SRC_DIRS:
        base = REPO_ROOT / src_dir
        if not base.exists():
            continue
        for path in sorted(base.rglob("*")):
            if not path.is_file():
                continue
            if path.suffix not in EXTENSIONS:
                continue
            if any(ex in path.parts for ex in EXCLUDE):
                continue

            rel = path.relative_to(REPO_ROOT)
            for text, line in chunk_file(path):
                docs.append(text)
                ids.append(f"chunk_{chunk_idx}")
                metas.append({
                    "file": str(rel),
                    "line": line,
                })
                chunk_idx += 1
            file_count += 1

    print(f"Found {file_count} files → {len(docs)} chunks")

    if not docs:
        print("Nothing to index.")
        return

    print("Embedding...")
    embeddings = model.encode(docs, show_progress_bar=True).tolist()

    print("Storing in Chroma...")
    batch = 500
    for i in range(0, len(docs), batch):
        collection.add(
            documents=docs[i:i + batch],
            embeddings=embeddings[i:i + batch],
            ids=ids[i:i + batch],
            metadatas=metas[i:i + batch],
        )

    print(f"\nDone. {len(docs)} chunks indexed into {CHROMA_PATH}")


if __name__ == "__main__":
    main()
