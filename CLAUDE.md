# CLAUDE.md — bridge to the repository's agent system

This file is a REAL FILE on purpose (not a symlink): a committed symlink
checks out as a one-line text file on Windows (`core.symlinks=false`) and
Claude's Edit/Write tools refuse symlinked paths. It also works on every
Claude Code version, including those before v2.1.277 (2026-09-18), whose
AGENTS.md fallback does not exist.

It contains ONLY imports — no rules of its own. The single source of truth
is `AGENTS.md`; the session-protocol summary lives in `.agents/CLAUDE.md`
and may not diverge from it. Do not add content here without also adding it
there.

@AGENTS.md
@.agents/CLAUDE.md
