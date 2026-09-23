# Archived Milestone 4 authentication notes

This document used to describe a proposed system daemon, migration tool, PAM
module, systemd units, and SELinux confinement. Some of those source artifacts
were experimental and had not been qualified; several design claims were
incorrect. This file is not implementation evidence or authorization to use
system authentication.

Current v5.1.0 boundary:

- The default CMake build and Fedora base RPM omit the daemon, PAM module,
  systemd units, sysusers entry, and SELinux source files.
- The runtime daemon protocol no longer exports keys or accepts caller-provided
  frames for broad verification/profile operations. Authorization binds the
  requested UID to `SO_PEERCRED.uid` exactly.
- Key lookup is read-only. Status and authentication requests cannot generate
  missing keys. The old vault-migration CLI has been removed.
- KDE lock-screen integration, a fresh separate unlock profile, explicit
  password-confirmed activation/deactivation, rate limiting, real camera
  capture, and active randomized challenge-response remain incomplete.

Do not install or enable experimental authentication components. Read
[the current qualification status](RELEASE-QUALIFICATION-V5.1.md) for release
gates. Any future lock-screen integration must preserve password access and
must not alter SDDM, sudo, Polkit, or global theme configuration.
