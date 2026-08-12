# Contributing

Contributions should keep KFaceAuth a small, privacy-first KDE utility for a
bounded local comparison in the logged-in user session.

## Before changing code

- Read `README.md`, `docs/ARCHITECTURE.md`, `docs/THREAT-BOUNDARY.md`, and the
  relevant protocol or vault document.
- Preserve the current checkout and unrelated changes.
- Keep product identity in `cmake/ProjectIdentity.cmake`.
- Keep user-visible strings translatable and add Swedish catalog coverage.
- Do not add authentication, privileged, network, telemetry, runtime-download,
  or persistent-image surfaces.

## Validation

Run the relevant local gates from [docs/BUILDING.md](docs/BUILDING.md). At a
minimum, run Python tests, `git diff --check`, formatting checks, and the
focused C++/QML test when dependencies are available. Report unavailable Fedora
tooling instead of weakening a gate.

Do not include images, embeddings, keys, profiles, stable camera identifiers,
biometric paths, scores, or unredacted reports in commits, issues, fixtures,
logs, or examples. Use fake workers and synthetic frames for tests.

## Scope and review

Keep changes focused and document any deliberate boundary change. Do not
commit, tag, push, publish, install on a host, or modify authentication state
as part of a normal contribution. Release work follows
[docs/RELEASE-CHECKLIST.md](docs/RELEASE-CHECKLIST.md).
