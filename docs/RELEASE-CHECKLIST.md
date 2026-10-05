# Release checklist

Use this checklist for every KFaceAuth source and package candidate. A passing
build does not qualify camera behavior, accessibility, PAM login, or
presentation-attack resistance.

## Standard package

- [ ] Confirm canonical CMake identity, Cargo, project metadata, source archive, RPM, and SRPM versions
      agree.
- [ ] Build with `KFACEAUTH_BUILD_EXPERIMENTAL_AUTH_COMPONENTS=OFF`.
- [ ] Run CTest, Python tests, Rust format/Clippy/tests, QML lint, translation
      validation, C++ formatting, and model verification.
- [ ] Inspect the staged install and RPM payload. Confirm it has no PAM module,
      daemon, auth worker, authentication units, sysusers entry, Polkit action, SELinux
      policy, setup helper, profile, key, or host readback.
- [ ] Confirm RPM documentation is the explicit user/developer allowlist and
      contains no planning or qualification records.
- [ ] Build the Fedora 44 RPM/SRPM, run `rpmlint`, and run the isolated RPM
      lifecycle smoke test.
- [ ] Verify source archive reproducibility, checksums, and release workflow
      artifact closure. Check the explicit source manifest and gitless SRPM rebuild.
- [ ] Verify tag/checkout/archive lineage; published assets must be identical on
      rerun and must never be replaced with different bytes.
- [ ] Read back CI results and release metadata for the exact candidate.
- [ ] Complete the KCM local-session hardware and accessibility record, or keep
      missing cases clearly `unverified` and make no unsupported claims.

## Experimental authentication package

- [ ] Build with `rpmbuild --with experimental_auth` in a separate CI job.
- [ ] Inspect the experimental subpackage for the intended PAM, daemon, isolated auth worker,
      systemd, sysusers, Polkit, SELinux, and setup artifacts.
- [ ] Confirm neither package has scriptlets that configure PAM or start/enable
      authentication services.
- [ ] Run sandboxed setup/rollback tests and obtain an independent security
      review before any real login-path qualification.
- [ ] Complete Enforcing-mode SELinux, SDDM, Plasma lock-screen, password
      fallback, device/session, and consent-based physical qualification in
      [RELEASE-QUALIFICATION-V5.3.md](RELEASE-QUALIFICATION-V5.3.md).
- [ ] Keep the feature experimental, opt-in, and absent from support claims
      while any gate remains open.

## Existing v5.0.0 release claims

- [x] Read back and correct the published v5.0.0 release text. Its original
      tag and assets remain unchanged for provenance.
- [x] Publish the factual correction record at
      `RELEASE-ERRATA-V5.0.0.md`; include it in the source archive and keep it
      out of the standard binary package documentation.
- [ ] Keep future public release edits within the release authority granted
      for that specific change.
