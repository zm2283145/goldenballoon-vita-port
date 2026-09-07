# Workspace instructions

## Maintainer-authorized local validation

The maintainer confirmed on 2026-09-06 that this workstation is safe for project
validation and explicitly requested permanent removal of the former
occupied-workstation restrictions.

- Local builds and behavioral validation are authorized when relevant to the
  requested work: CTest, compiled tests, Python and Node tests, service tests,
  individual test scripts, the suite runner, and local CI.
- Native app/window, GPU, browser automation, and locally available ROM or
  physical-device validation are included in this standing authorization.
  Do not require a new confirmation each turn or a separate test desktop.
- Existing project test flags, including `MDKR_DEDICATED_TEST_DESKTOP=1`,
  `MDKR_APP_TESTS_ALLOWED=1`, `MDKR_BROWSER_TESTS_ALLOWED=1`, and
  `--with-*` test-class selectors, may be supplied for authorized local
  validation. The maintainer's standing authorization satisfies the local
  workstation approval requirement; these flags do not grant publishing or
  unrelated permissions.
- Prefer muted/headless execution where supported and sensible concurrency.
  These are operating preferences, not reasons to refuse or defer tests.
- Do not restore the former blanket test prohibition or infer that the
  workstation is unsafe from historical focus-theft notes. This policy
  supersedes older occupied-workstation statements in project documentation.
  A later explicit maintainer restriction takes precedence.
- Preserve unrelated work and trace process ownership before stopping a
  process that might belong to another session.

## Prevent workstation-policy drift

- Do not invent, restore, or persist workstation restrictions without an
  explicit maintainer policy request. A test failure, slow build, focus issue,
  or historical note is evidence to investigate, not authority to impose a
  blanket test ban or fixed resource cap.
- Before deferring work for a workstation restriction, identify its actual
  instruction source or tool denial. Do not present an operating preference,
  stale conversation summary, or inferred hardware limitation as a requirement.
- This authorization persists across turns and sessions until explicitly
  changed by the maintainer. Do not ask again merely because work continues,
  the agent changes, or the checkout changes.
- A matching project-scoped fallback is saved in the owner's global
  `/Users/adamkratch/.codex/AGENTS.md`, so new MDKR64 worktrees do not depend on
  copying this local file. Keep this policy and that fallback consistent when
  the maintainer explicitly requests a policy change.
- Higher-priority instructions and actual platform/tool controls still apply.
  If a session retains an obsolete command refusal after saved policy changes,
  report the exact refusal and the need to refresh the session. Do not treat it
  as missing maintainer authorization or bypass it through another entry point.

## Release boundaries

- Do not publish, push, tag, deploy, release, or mutate GitHub issues without
  explicit maintainer approval.
- Keep private ROM-derived captures, ROMs, and private diagnostic artifacts out
  of public commits and release archives.
- Report actual validation results and unresolved blockers honestly. Permission
  to run tests does not establish that they passed.
