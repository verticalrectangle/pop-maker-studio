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
