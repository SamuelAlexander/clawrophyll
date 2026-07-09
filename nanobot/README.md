# nanobot/ — host-side agent artifacts

Everything the nanobot agent ("Phil") needs on the UNO Q's Linux side: the
workspace (persona, skills, periodic-task file) and the config overlay.

## Where nanobot reads from

At runtime nanobot reads its **workspace** live from `~/.nanobot/workspace/` on
the board — `SOUL.md`, skills, and memory are re-read each turn; `config.json`
changes need a nanobot restart. That directory is **not** the `nanobot/` tree
that `scripts/sync.sh` rsyncs to `$UNOQ_REMOTE_DIR` (that copy is just a backup of
the source). The live workspace is the runtime source of truth.

## What this repo tracks (and what it deliberately doesn't)

The board's live `~/.nanobot/workspace/` is a mix of our config, nanobot
boilerplate, and runtime state. This repo tracks only the files **we author**:

| Path | Tracked | Why |
|---|---|---|
| `workspace/SOUL.md` | ✅ | Persona / system prompt — ours |
| `workspace/USER.md` | ✅ | Who the user is — ours |
| `workspace/skills/` | ✅ | Procedures (e.g. `sense-and-decide.md`) — ours |
| `workspace/MEMORY.md` | ✅ *(seed)* | Standing instructions + Phil's notes |
| `workspace/HEARTBEAT.md` | ✅ *(seed)* | Periodic-task list; Step 5 authors the cycle here |
| `AGENTS.md`, `TOOLS.md` | ❌ | nanobot onboarding **boilerplate** — `nanobot onboard` regenerates them (the board's own `.gitignore` treats them as regenerable) |
| `cron/`, `memory/`, `sessions/`, `.git/` | ❌ | **Runtime state** — scheduled jobs, memory store, chat history, nanobot's own workspace git |
| `config.json` | ❌ | **Secrets** (Anthropic key, Telegram token) — template in `config.example.json` |

**Seed files.** `MEMORY.md` and `HEARTBEAT.md` are stateful — Sam edits standing
instructions / periodic tasks and Phil writes notes and adds heartbeat tasks into
the *live* copies, so the running files are authoritative. The repo keeps them as
seeds for first-time setup; the auto-deploy below **skips them** so a deploy never
clobbers runtime edits.

## Deploying workspace changes

`scripts/sync-workspace.sh` rsyncs the authored config (`SOUL.md`, `USER.md`,
`skills/`) → `~/.nanobot/workspace/` on the board. It uses no `--delete` (board-only
files survive) and excludes the two seed files. nanobot picks the changes up live —
no restart needed. `scripts/redeploy.sh` runs it as part of a full deploy.

```bash
scripts/sync-workspace.sh --dry-run   # preview
scripts/sync-workspace.sh             # push
```

## First-time setup

After `nanobot onboard` (which generates the boilerplate above), apply our overlay:
copy `workspace/SOUL.md`, `USER.md`, `MEMORY.md`, `HEARTBEAT.md`, and `skills/` into
`~/.nanobot/workspace/`, and apply `config.example.json` to `~/.nanobot/config.json`
(fill in the secrets). Thereafter use `scripts/sync-workspace.sh` for ongoing config
changes.
