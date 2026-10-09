# Unreleased 5.4.0 candidate validation

Baseline: main `7c266c903b4d248c9b8d9cb79f77732b272711cf` (PR #23).
Implementation combines the planned 5.3.1 fixes and 5.4.0 additions in a separate
worktree. Existing integration edits were preserved. No release, installation,
PAM edit or running desktop/service mutation is part of this work.

## Reproducible local checks

```bash
cmake -S . -B build-standard -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=ON
cmake --build build-standard --parallel 3
QT_QPA_PLATFORM=offscreen ctest --test-dir build-standard --output-on-failure
cmake -S . -B build-experimental -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=ON -DKFACEAUTH_BUILD_EXPERIMENTAL_AUTH_COMPONENTS=ON
cmake --build build-experimental --parallel 3
QT_QPA_PLATFORM=offscreen ctest --test-dir build-experimental --output-on-failure
cargo fmt --manifest-path engine/Cargo.toml --all -- --check
cargo clippy --manifest-path engine/Cargo.toml --workspace --all-targets --locked --offline -- -D warnings
cargo test --manifest-path engine/Cargo.toml --workspace --all-targets --locked --offline -- --test-threads=1
python3 -m unittest discover -s tests -p 'test_*.py'
msgfmt --check --check-format --output-file=/dev/null po/sv/kcm_kfaceauth.po
/usr/lib64/qt6/bin/qmllint src/kcm/ui/*.qml src/kcm/ui/components/*.qml
python3 tools/verify_project_identity.py
python3 tools/verify_models.py --root models
```

Root-only Rust fixtures must also execute in a disposable root container with
`kfaceauth` group present; a normal user run returns early for those fixtures.
Local verification uses a network-disabled Fedora 44 container and read-only
mounts of the compiled test executables, leaving the host authentication state
untouched. These cover protected metadata, UID/corruption, directory replacement,
transactional resync, deletion, exact policy restoration and failed rollback.

Stage both installs with `DESTDIR`, then run
`packaging/fedora/verify-staged-payload.sh STAGE standard|experimental-auth`.
The standard payload excludes privileged helpers, PAM, units and Polkit policy.

## Pinned downstream integrations

- SDDM 0.21.0: `63780fcd79f1dbf81a30eef48c28c699ab15aded`.
- KScreenLocker 6.7.5: `057b3774d9ad322cfccc2683ea057aed87e0f878`.
- Plasma Desktop 6.7.5: `43f55fff3480c6eb5f60133f045bfc74b9c67d84`.

The SDDM and KScreenLocker default/experimental builds, Qt tests and staged
boundaries passed in isolated Fedora 44 containers. Plasma Desktop's active
lock-screen patch passed its pinned applicability and three source contracts.
The companion-theme adapter remains a separate pinned source contract.

## Observed software results (2026-10-09)

- Standard and experimental Qt suites: 19/19 and 20/20 passed; affected suites
  rerun after the final camera and UI changes.
- Rust workspace: 145 tests passed. A separate root execution covered all 57
  template/policy/sync/camera fixtures, including cases skipped by a normal user.
- Python: 74 tests passed, including Swedish translation coverage and source,
  SELinux/service, packaging and protocol contracts. Formatting, QML lint,
  model integrity, version identity and deterministic source archives passed.
- Standard and experimental RPM builds passed full `%check` in isolated Fedora
  44 root containers with declared build dependencies, without `--nodeps`.
  Project-configured rpmlint has zero errors and existing packaging warnings.
  Actual RPM and staged payload boundaries and the isolated legacy upgrade,
  install/reinstall/remove smoke test passed. No product was installed on the host.

## Limits

Physical camera/hotplug, suspend/resume, 20 repeated real sessions, keyboard,
screen reader, SELinux Enforcing runtime, SDDM login and Plasma unlock are
**NOT RUN**. Offscreen Swedish large-text checks at 320/480/960 pixels exercise
layout and explicit recovery actions; they do not qualify physical accessibility.
No separate qualification machine is available. Follow `HARDWARE-QUALIFICATION.md`
when that resource is available. The authentication experiment stays Off by
default, unsupported and unqualified.

Host RPM dependency preflight reports missing cargo/rust/clippy/rustfmt RPM
providers because Rust is supplied by rustup. Local `--nodeps` builds were
supplemented with the dependency-checked Fedora container builds above.
The isolated RPM smoke test is a payload/lifecycle check; it does not qualify
physical runtime behavior or a dependency-resolved host installation.
