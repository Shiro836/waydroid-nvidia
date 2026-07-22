# Agent instructions (CLAUDE.md = AGENTS.md)

If STATE.md exists in this checkout (maintainer machines only — it is not
published), always read it completely before planning or making changes. Do
not rely only on its Current state or milestone sections: read the entire
append-only Log, and treat the newest log entries as authoritative when
older sections are stale.

Before making fundamental changes, always check whether someone on the
internet has already done what we're trying to do.

Use every available means instead of guessing. Instead of brute-forcing
parameters against an unknown library, clone it and check directly.

Instead of hacks and crutches, solve problems fundamentally.

Use every available resource — sources, etc. If some command is used very
often via sudo mcp, get NOPASSWD rights for it.

## For agents working from a public clone

- Orientation: README.md, then docs/building.md (repo layout, per-component
  recipes, pins), docs/architecture.md, docs/transport-design.md.
- Everything builds locally: `packaging/aur/reproduce.sh` runs the same
  clone→apply→build path CI does. Validate hermetically (a container with
  CI parity) before pushing — do NOT use CI as a remote grep/build/debug
  service.
- patches/ is generated from working trees (dev/sync-patches on the
  maintainer box) — edit patches only by regenerating, never by hand.
- src/ files are canonical even when build scripts copy them into upstream
  trees (build/*/build.sh may overwrite tree copies at build time).
- Tests select GPUs by vendor/driver-id, never by /dev/dri node paths;
  tests/ is a portable regression suite, keep it host-agnostic.

Working recipes (dev loop, guest commands, benches, deploy rules):
docs/dev-workflow.md.
