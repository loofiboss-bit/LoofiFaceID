# Fedora packaging

The Fedora 44 RPM builds the KCM and three ordinary-user workers. It installs
the exact verified YuNet FP32 and SFace FP32 artifacts with manifest, licenses,
and immutable provenance. Fedora supplies OpenCV 4.13 (the package contract
accepts OpenCV >= 4.8), OpenSSL 3, and KF6
KWallet; none is bundled.

## Package transition

`kfaceauth` 4.0.0 replaces `plasma-irlume` 3.x:

```spec
Obsoletes: plasma-irlume < 4.0.0
Provides:  plasma-irlume = %{version}-%{release}
```

The replacement removes the old `kcm_irlume` plugin, desktop entry, and
`plasma-irlume-camera-preview-worker` through normal RPM ownership. The new
package installs only `kcm_kfaceauth` and its three ordinary-user workers. It
has no migration scriptlet and never reads, creates, changes, or removes user
configuration, KWallet entries, biometric profiles, PAM, or authselect state.

## Build and reproduce

```bash
SOURCE_DATE_EPOCH=0 packaging/fedora/create-source-archive.sh
rpmbuild -ba packaging/fedora/kfaceauth.spec --define "_sourcedir $PWD"
```

Repeat the source archive twice and compare SHA-256. Repeat SRPM/RPM builds at
the same normalized rpmbuild path and compare bytes. The complete prepared
source set is offline: Cargo is locked/offline and the required crates are
vendored under `engine/vendor`; no model is downloaded.

## Inspect

```bash
rpm_path="$(find "$HOME/rpmbuild/RPMS" -name 'kfaceauth-[0-9]*.rpm' -print -quit)"
rpm -qpl "$rpm_path"
rpm -qp --requires "$rpm_path"
rpm -qp --scripts "$rpm_path"
rpm -qp --dump "$rpm_path"
rpm2cpio "$rpm_path" | cpio -itv
rpmlint "$rpm_path"
packaging/fedora/rpm-smoke-test.sh "$rpm_path"
```

Inspect worker modes/ownership, ELF `NEEDED` entries, file capabilities,
scriptlets, model hashes, and license/provenance payload. The package must have
no PAM file/module, authselect mutation, service unit, privileged helper,
setuid/capability, evaluator, fake provider, or authentication scriptlet.

Release qualification additionally uses ordinary dependency-resolved
`dnf install`, upgrade, and remove in a clean Fedora 44 environment. `--nodeps`
is never clean-install evidence. Installation/upgrade must not create a
profile or KWallet key. Removal must not inspect or delete user-home data;
profile deletion is only an explicit application action.

The isolated smoke test builds a payload-only `plasma-irlume` 3.0.0 fixture,
upgrades it to the tested `kfaceauth` RPM, verifies that every legacy KCM file
is removed, reinstalls the new package, and checks complete package removal
while preserving unrelated user and PAM sentinels.
