# Claude-Assisted CI Pipeline

Three modes for integrating Claude into the GitHub Actions CI pipeline. All trigger on build failure and use the Anthropic API to analyze the failure with full source context.

**Implemented**: Mode D and Mode D + Slack (`tools/claude_diagnose.py`, `.github/workflows/claude-diagnose.yml`)
**Documented**: Mode C (design reference only)

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

On CI failure, Claude reads the build log, correlates it with recent commits, and posts a GitHub comment on the commit with a diagnosis and suggested fix. Human applies it manually. Optionally also sends a Slack notification.

**Why this mode**: safer than Mode C — no automated commits, no automated PRs. Claude acts as a smart observer that saves you the time of reading the raw build log and tracing it to the right file and line.

**Flow**:

```
CI fails
    ↓
GitHub Actions triggers claude-diagnose job
    ↓
claude_diagnose.py reads build log + git log + relevant src files
    ↓
Claude API: diagnose root cause, identify file + line, suggest fix
    ↓
gh api: post comment on failing commit with full diagnosis
    ↓
(if SLACK_WEBHOOK_URL set) POST Block Kit message to Slack
    ↓
Human reads comment/Slack message, applies fix manually
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
    runs-on: ubuntu-24.04
    permissions:
      contents: read
      actions: read

    steps:
      - uses: actions/checkout@v4
        with:
          fetch-depth: 10

      - name: Download build log
        uses: actions/download-artifact@v4
        with:
          name: build-log-linux
          run-id: ${{ github.event.workflow_run.id }}
          github-token: ${{ secrets.GITHUB_TOKEN }}

      - name: Install anthropic
        run: pip install anthropic --quiet

      - name: Run Claude diagnose
        env:
          ANTHROPIC_API_KEY: ${{ secrets.ANTHROPIC_API_KEY }}
          GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}
          GITHUB_REPOSITORY: ${{ github.repository }}
          GITHUB_SHA: ${{ github.event.workflow_run.head_sha }}
          SLACK_WEBHOOK_URL: ${{ secrets.SLACK_WEBHOOK_URL }}
        run: python3 tools/claude_diagnose.py --log build-log.txt
```

---

## The script (`tools/claude_diagnose.py`)

Mode D implementation. Slack is opt-in via `--slack-webhook` flag or `SLACK_WEBHOOK_URL` env var.

```python
#!/usr/bin/env python3
"""
Mode D CI pipeline — Claude diagnoses a failed build, posts a GitHub commit comment,
and optionally sends a Slack notification.

Usage (local test):
    ANTHROPIC_API_KEY=... GITHUB_TOKEN=... GITHUB_REPOSITORY=verticalrectangle/pop-maker-studio \
    GITHUB_SHA=<sha> python3 tools/claude_diagnose.py --log build-log.txt

With Slack:
    ... python3 tools/claude_diagnose.py --log build-log.txt --slack-webhook https://hooks.slack.com/...
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

import anthropic
import urllib.request
import json

REPO_ROOT     = Path(__file__).parent.parent
SRC_DIRS      = ["src", "tools", "mcp_server"]
MAX_SRC_CHARS = 60_000
LOG_TAIL      = 300


def read_log(path: str) -> str:
    text = Path(path).read_text(errors="ignore")
    lines = text.splitlines()
    return "\n".join(lines[-LOG_TAIL:])


def collect_source(log: str) -> str:
    mentioned = set(re.findall(r'[\w/]+\.(?:cpp|h|py|cmake|txt)', log))
    chunks, total = [], 0
    for name in sorted(mentioned):
        for d in SRC_DIRS:
            for candidate in (REPO_ROOT / d).rglob(name):
                try:
                    text = candidate.read_text(errors="ignore")
                    if total + len(text) > MAX_SRC_CHARS:
                        return "\n\n".join(chunks)
                    chunks.append(f"=== {candidate.relative_to(REPO_ROOT)} ===\n{text}")
                    total += len(text)
                except OSError:
                    pass
    return "\n\n".join(chunks)


def recent_commits() -> str:
    result = subprocess.run(
        ["git", "log", "--oneline", "-10"],
        capture_output=True, text=True, cwd=REPO_ROOT
    )
    return result.stdout.strip()


def call_claude(log: str, source: str, commits: str) -> str:
    client = anthropic.Anthropic()
    message = client.messages.create(
        model="claude-opus-4-7",
        max_tokens=1024,
        messages=[{
            "role": "user",
            "content": f"""A C++ CI build has failed. Diagnose the root cause.

Return exactly three sections:
1. **Root cause** — 2-3 sentences, specific file and line number if visible in the log
2. **Fix** — the exact code change needed (short snippet, not a full diff)
3. **Category** — one of: code bug | dependency issue | config issue | environment issue

Be specific and concise. A developer will apply this fix manually.

## Build log (last {LOG_TAIL} lines)
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


def post_commit_comment(sha: str, body: str) -> None:
    repo  = os.environ["GITHUB_REPOSITORY"]
    token = os.environ["GITHUB_TOKEN"]
    result = subprocess.run([
        "gh", "api",
        f"repos/{repo}/commits/{sha}/comments",
        "-f", f"body={body}",
    ], env={**os.environ, "GH_TOKEN": token}, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"gh api error: {result.stderr}", file=sys.stderr)
        sys.exit(1)


def post_slack_message(webhook_url: str, sha: str, diagnosis: str) -> None:
    repo        = os.environ.get("GITHUB_REPOSITORY", "")
    commit_url  = f"https://github.com/{repo}/commit/{sha}"
    first_line  = diagnosis.splitlines()[0].replace("**Root cause**", "").strip(" —:-")

    payload = {
        "blocks": [
            {
                "type": "header",
                "text": {"type": "plain_text", "text": "🔴 Build Failed"}
            },
            {
                "type": "section",
                "text": {
                    "type": "mrkdwn",
                    "text": f"*`{sha[:8]}`* — {first_line}"
                },
                "accessory": {
                    "type": "button",
                    "text": {"type": "plain_text", "text": "View diagnosis"},
                    "url": commit_url
                }
            }
        ]
    }
    data = json.dumps(payload).encode()
    req  = urllib.request.Request(webhook_url, data=data,
                                  headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req) as resp:
        if resp.status != 200:
            print(f"Slack webhook error: {resp.status}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True, help="Path to build log file")
    parser.add_argument("--slack-webhook", default=os.environ.get("SLACK_WEBHOOK_URL"),
                        help="Slack incoming webhook URL (or set SLACK_WEBHOOK_URL)")
    args = parser.parse_args()

    log     = read_log(args.log)
    source  = collect_source(log)
    commits = recent_commits()

    print("Calling Claude...", flush=True)
    diagnosis = call_claude(log, source, commits)
    print(diagnosis)

    sha = os.environ.get("GITHUB_SHA", "")
    if not sha:
        print("No GITHUB_SHA — skipping comment post")
        return

    body = f"## 🤖 Claude CI Diagnosis\n\n{diagnosis}\n\n---\n*Generated by [claude_diagnose.py](tools/claude_diagnose.py) on build failure*"
    post_commit_comment(sha, body)
    print(f"Comment posted to {sha[:8]}")

    if args.slack_webhook:
        post_slack_message(args.slack_webhook, sha, diagnosis)
        print("Slack message sent.")


if __name__ == "__main__":
    main()
```

---

## Setup

**1. Add secrets**

In GitHub repo settings → Secrets → Actions:
```
ANTHROPIC_API_KEY = sk-ant-...
SLACK_WEBHOOK_URL = https://hooks.slack.com/services/...   # optional
```

**2. Ensure build log is uploaded as artifact**

In your existing build workflow, add after the build step:
```yaml
- name: Upload build log
  if: failure()
  uses: actions/upload-artifact@v4
  with:
    name: build-log-linux
    path: build-log.txt
```

And pipe build output to that file:
```yaml
- name: Build
  run: cmake --build build --parallel 2>&1 | tee build-log.txt; exit ${PIPESTATUS[0]}
```

**3. Script dependencies**

The workflow installs `anthropic` inline (`pip install anthropic --quiet`). No other dependencies — Slack uses stdlib `urllib.request`.

---

## Security considerations

- `ANTHROPIC_API_KEY` and `SLACK_WEBHOOK_URL` are repo secrets — never logged, never in commit comments
- Mode C (PR) only runs `patch -p1` on Claude's diff — no `eval`, no `exec`, no shell injection surface
- Mode D (comment) makes no writes to the repo — read-only CI job (`contents: read`, `actions: read`)
- Slack payload is built in Python with `json.dumps` — no shell interpolation of Claude output
- Claude never sees secrets, only source files and the build log

---

## Cost

Each invocation is one `claude-opus-4-7` API call with up to ~60K tokens of source context plus 300 lines of build log. At typical pricing this is under $1 per failure. For a low-traffic repo this is negligible; for high-volume CI consider gating on branch (e.g. only trigger on `main` failures).
