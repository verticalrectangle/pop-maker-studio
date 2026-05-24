# Claude-Assisted CI Pipeline

Two modes for integrating Claude into the GitHub Actions CI pipeline. Both trigger on build or test failure and use the Anthropic API to analyze the failure with full source context.

---

## Mode C — Claude opens a fix PR

On CI failure, Claude reads the build log and the relevant source files, writes a fix, and opens a pull request. A human reviews and merges. CI runs again on the PR.

**Why this mode**: C++ can't be verified locally by Claude before pushing — the fix needs CI to validate it. A PR gives CI a second shot at catching a bad fix before it lands on main. Claude does the diagnosis and legwork; you stay in the loop.

**Flow**:

```
CI fails
    ↓
GitHub Actions triggers claude-fix job
    ↓
claude-fix.py reads build log + relevant src files
    ↓
Claude API: diagnose + produce patch
    ↓
Apply patch, push to branch claude/fix-{run_id}
    ↓
gh pr create with diagnosis in body
    ↓
CI runs on PR
    ↓
Human reviews + merges (or closes if fix is wrong)
```

**GitHub Actions workflow** (`.github/workflows/claude-fix.yml`):

```yaml
name: Claude Fix PR

on:
  workflow_run:
    workflows: ["Build"]
    types: [completed]

jobs:
  claude-fix:
    if: ${{ github.event.workflow_run.conclusion == 'failure' }}
    runs-on: ubuntu-22.04
    permissions:
      contents: write
      pull-requests: write

    steps:
      - uses: actions/checkout@v4

      - name: Download build log
        uses: actions/download-artifact@v4
        with:
          name: build-log
          run-id: ${{ github.event.workflow_run.id }}
          github-token: ${{ secrets.GITHUB_TOKEN }}

      - name: Run Claude fix script
        env:
          ANTHROPIC_API_KEY: ${{ secrets.ANTHROPIC_API_KEY }}
          GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}
        run: python3 tools/claude-fix.py --log build-log.txt --mode pr
```

---

## Mode D — Claude diagnoses and comments

On CI failure, Claude reads the build log, correlates it with recent commits, and posts a GitHub comment on the commit with a diagnosis and suggested fix. Human applies it manually.

**Why this mode**: safer than Mode C — no automated commits, no automated PRs. Claude acts as a smart observer that saves you the time of reading the raw build log and tracing it to the right file and line.

**Flow**:

```
CI fails
    ↓
GitHub Actions triggers claude-diagnose job
    ↓
claude-fix.py reads build log + git log + relevant src files
    ↓
Claude API: diagnose root cause, identify file + line, suggest fix
    ↓
gh api: post comment on failing commit with full diagnosis
    ↓
Human reads comment, applies fix manually
```

**GitHub Actions workflow** (`.github/workflows/claude-diagnose.yml`):

```yaml
name: Claude Diagnose

on:
  workflow_run:
    workflows: ["Build"]
    types: [completed]

jobs:
  claude-diagnose:
    if: ${{ github.event.workflow_run.conclusion == 'failure' }}
    runs-on: ubuntu-22.04
    permissions:
      contents: read
      pull-requests: write

    steps:
      - uses: actions/checkout@v4
        with:
          fetch-depth: 10

      - name: Download build log
        uses: actions/download-artifact@v4
        with:
          name: build-log
          run-id: ${{ github.event.workflow_run.id }}
          github-token: ${{ secrets.GITHUB_TOKEN }}

      - name: Run Claude diagnose script
        env:
          ANTHROPIC_API_KEY: ${{ secrets.ANTHROPIC_API_KEY }}
          GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}
        run: python3 tools/claude-fix.py --log build-log.txt --mode comment
```

---

## The script (`tools/claude-fix.py`)

Both modes use the same script with a `--mode` flag.

```python
#!/usr/bin/env python3
"""
Claude-assisted CI failure handler.

Modes:
  --mode pr       Produce a fix and open a pull request
  --mode comment  Diagnose and post a GitHub commit comment
"""

import argparse
import os
import subprocess
import sys
import textwrap
from pathlib import Path

import anthropic

REPO_ROOT = Path(__file__).parent.parent
SRC_DIRS  = ["src", "tools", "mcp_server"]
MAX_SRC_CHARS = 80_000   # cap on source context sent to Claude


def read_build_log(path: str) -> str:
    log = Path(path).read_text(errors="ignore")
    # Keep last 300 lines — the tail is where errors live
    lines = log.splitlines()
    return "\n".join(lines[-300:])


def collect_source(log: str) -> str:
    """Heuristic: find filenames mentioned in the log and read those files."""
    import re
    mentioned = set(re.findall(r'[\w/]+\.(?:cpp|h|py|cmake)', log))
    chunks = []
    total = 0
    for name in mentioned:
        for d in SRC_DIRS:
            candidates = list((REPO_ROOT / d).rglob(name))
            for c in candidates:
                try:
                    text = c.read_text(errors="ignore")
                    if total + len(text) > MAX_SRC_CHARS:
                        break
                    chunks.append(f"=== {c.relative_to(REPO_ROOT)} ===\n{text}")
                    total += len(text)
                except OSError:
                    pass
    return "\n\n".join(chunks)


def recent_commits() -> str:
    result = subprocess.run(
        ["git", "log", "--oneline", "-10"],
        capture_output=True, text=True
    )
    return result.stdout.strip()


def call_claude(log: str, source: str, commits: str, mode: str) -> str:
    client = anthropic.Anthropic()

    if mode == "pr":
        task = textwrap.dedent("""
            The CI build has failed. Produce a minimal fix.

            Return your response as:
            1. A brief diagnosis (2-3 sentences)
            2. A unified diff of the fix (```diff ... ```)
            3. A one-line PR title

            Do not change anything unrelated to the failure.
            Do not add comments explaining the fix.
        """)
    else:
        task = textwrap.dedent("""
            The CI build has failed. Diagnose the root cause.

            Return:
            1. Root cause (2-3 sentences, specific file and line if possible)
            2. The exact change needed to fix it (code snippet, not a diff)
            3. Whether this looks like a code bug, a dependency issue, or a config issue

            Be specific. A developer will apply this fix manually.
        """)

    message = client.messages.create(
        model="claude-opus-4-7",
        max_tokens=2048,
        messages=[{
            "role": "user",
            "content": f"""
{task}

## Build log (last 300 lines)
```
{log}
```

## Recent commits
```
{commits}
```

## Relevant source files
{source}
""".strip()
        }]
    )
    return message.content[0].text


def apply_diff(diff: str) -> bool:
    """Apply a unified diff. Returns True on success."""
    result = subprocess.run(
        ["patch", "-p1", "--forward"],
        input=diff, text=True,
        capture_output=True
    )
    return result.returncode == 0


def post_github_comment(body: str) -> None:
    sha = subprocess.run(
        ["git", "rev-parse", "HEAD"], capture_output=True, text=True
    ).stdout.strip()
    repo = os.environ.get("GITHUB_REPOSITORY", "")
    token = os.environ["GITHUB_TOKEN"]

    subprocess.run([
        "gh", "api",
        f"repos/{repo}/commits/{sha}/comments",
        "-f", f"body={body}",
    ], env={**os.environ, "GH_TOKEN": token}, check=True)


def open_pr(diagnosis: str, diff: str, title: str) -> None:
    import re
    run_id = os.environ.get("GITHUB_RUN_ID", "unknown")
    branch = f"claude/fix-{run_id}"

    subprocess.run(["git", "checkout", "-b", branch], check=True)
    if not apply_diff(diff):
        print("Patch failed to apply — skipping PR", file=sys.stderr)
        return

    subprocess.run(["git", "add", "-u"], check=True)
    subprocess.run([
        "git", "commit", "-m", f"fix: {title}"
    ], check=True)
    subprocess.run([
        "git", "push", "origin", branch
    ], check=True)

    body = f"## Claude diagnosis\n\n{diagnosis}\n\n---\n\n🤖 Auto-generated by Claude CI"
    subprocess.run([
        "gh", "pr", "create",
        "--title", f"fix: {title}",
        "--body", body,
        "--head", branch,
    ], check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--log",  required=True)
    parser.add_argument("--mode", required=True, choices=["pr", "comment"])
    args = parser.parse_args()

    log     = read_build_log(args.log)
    source  = collect_source(log)
    commits = recent_commits()

    print(f"Calling Claude ({args.mode} mode)...")
    response = call_claude(log, source, commits, args.mode)
    print(response)

    if args.mode == "comment":
        body = f"## 🤖 Claude CI diagnosis\n\n{response}"
        post_github_comment(body)
        print("Comment posted.")

    elif args.mode == "pr":
        import re
        diff_match  = re.search(r'```diff\n(.*?)```', response, re.DOTALL)
        title_match = re.search(r'(?:PR title:|title:)\s*(.+)', response, re.IGNORECASE)
        diff  = diff_match.group(1)  if diff_match  else ""
        title = title_match.group(1) if title_match else "automated fix"
        diag  = response.split("```")[0].strip()
        if diff:
            open_pr(diag, diff, title)
        else:
            print("No diff found in Claude response — falling back to comment mode")
            post_github_comment(f"## 🤖 Claude CI diagnosis\n\n{response}")


if __name__ == "__main__":
    main()
```

---

## Setup

**1. Add secret**

In GitHub repo settings → Secrets → Actions:
```
ANTHROPIC_API_KEY = sk-ant-...
```

**2. Ensure build log is uploaded as artifact**

In your existing build workflow, add after the build step:
```yaml
- name: Upload build log
  if: failure()
  uses: actions/upload-artifact@v4
  with:
    name: build-log
    path: build-log.txt
```

And pipe build output to that file:
```yaml
- name: Build
  run: cmake --build build -j$(nproc) 2>&1 | tee build-log.txt; exit ${PIPESTATUS[0]}
```

**3. Install script dependencies**
```bash
pip install anthropic
```

---

## Security considerations

- `ANTHROPIC_API_KEY` is a repo secret — never logged, never in PR bodies
- Mode C (PR) only runs `patch -p1` on Claude's diff — no `eval`, no `exec`, no shell injection surface
- Mode D (comment) makes no writes to the repo — read-only CI job
- Both modes have `contents: read` or `contents: write` scoped permissions only — no admin access
- Claude never sees secrets, only source files and the build log

---

## Cost

Each invocation is one `claude-opus-4-7` API call with up to ~80K tokens of source context plus 300 lines of build log. At typical pricing this is under $1 per failure. For a low-traffic repo this is negligible; for high-volume CI consider gating on branch (e.g. only trigger on `main` failures).
