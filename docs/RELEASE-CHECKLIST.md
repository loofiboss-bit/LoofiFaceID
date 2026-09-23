# KFaceAuth v5.1.0 release checklist

This list is for the current unreleased stabilization target. Passing local
code checks does not qualify a camera, authentication flow, or release. See
[the qualification status](RELEASE-QUALIFICATION-V5.1.md) for the current
boundary and required physical/security evidence.

## Local package closure

- [ ] Confirm version metadata, generated CMake metadata, source archive, RPM,
      and SRPM all use `5.1.0`.
- [ ] Run CMake build/CTest, Python tests, Rust format/Clippy/tests, QML lint,
      translation validation, C++ formatting, and model verification.
- [ ] Inspect the staged RPM payload and confirm it contains no daemon, PAM
      module, systemd unit, sysusers entry, SELinux policy, migration tool, key,
      profile, or user data.
- [ ] Build and inspect the Fedora 44 RPM and SRPM; run the RPM smoke test.
- [ ] Confirm the default build option
      `KFACEAUTH_BUILD_EXPERIMENTAL_AUTH_COMPONENTS=OFF`.
- [ ] Verify source-archive reproducibility and release-artifact checksums.
- [ ] Read back CI results for the exact candidate commit.

## Face-unlock release gates

- [ ] Integrate a button-driven face action into KDE's ordinary lock-screen
      authentication flow and provide a dedicated lock-screen PAM service.
- [ ] Complete an independent security review and implement a separate fresh
      unlock profile, password-confirmed activation/deactivation, server-held
      keys, bounded attempts, and fail-closed multi-frame randomized challenges.
- [ ] Verify password unlock is unchanged when face unlock is enabled, disabled,
      unavailable, timed out, or after wake from sleep.
- [ ] Complete the physical camera/user matrix and attack thresholds in
      `RELEASE-QUALIFICATION-V5.1.md`; publish aggregate conditions/results only.
- [ ] Keep the feature disabled and omit it from release claims while any gate
      remains open.

## Publication authority

- [ ] Obtain explicit authorization before committing, tagging, publishing a
      GitHub release, or uploading COPR artifacts.
