# Fedora packaging

The published stable package is [KFaceAuth 5.2.1](https://github.com/loofiboss-bit/LoofiFaceID/releases/tag/v5.2.1),
for the explicit local-profile workflow on Fedora 44/KDE Plasma 6. It does not
provide an authentication factor. v5.2.1 corrects the missing standard
`systemd-devel` build requirement for `libudev` found when v5.2.0 was rebuilt
in COPR; its published SRPM passed [COPR build 11075253](https://copr.fedorainfracloud.org/coprs/build/11075253/).
The opt-in PAM subpackage remains unsupported and unqualified for login.

The ordinary Fedora 44 RPM builds the KCM and local workers. It installs the exact
verified YuNet FP32 and SFace FP32 artifacts with manifest, licenses, and
immutable provenance. Fedora supplies OpenCV 4.13 (the package contract
accepts OpenCV >= 4.8), OpenSSL 3, and KF6 KWallet; none is bundled.

Ordinary users should install the maintained COPR build:

```bash
sudo dnf copr enable loofitheboss/loofifaceid
sudo dnf install kfaceauth
```

The first-run KCM is an experimental local profile/comparison flow for the
logged-in Fedora 44/KDE Plasma 6 session. Installing the ordinary package does
not configure or activate PAM, SDDM, sudo, Polkit, or another system
authentication path. The `kfaceauth-experimental-auth` subpackage is built
only with `rpmbuild --with experimental_auth`; it is disabled in normal and
COPR builds, and remains unqualified.

## Package transition

The current spec replaces legacy `plasma-irlume` packages below 5.1.0:

```spec
Obsoletes: plasma-irlume < 5.1.0
Provides:  plasma-irlume = %{version}-%{release}
```

The replacement removes the old `kcm_irlume` plugin, desktop entry, and
`plasma-irlume-camera-preview-worker` through normal RPM ownership. The package
installs the KCM and ordinary-user workers. The opt-in authentication
subpackage installs the daemon, PAM module, SELinux modules, units, and
administrator-authorized setup helper. The packages have no migration scriptlet and
never reads, creates, changes, or removes user configuration, KWallet entries,
biometric profiles, PAM, or authselect state.

## Build and reproduce

```bash
SOURCE_DATE_EPOCH=0 packaging/fedora/create-source-archive.sh
rpmbuild -ba packaging/fedora/kfaceauth.spec --define "_sourcedir $PWD"
# Explicit engineering build only; not used by COPR:
rpmbuild -ba packaging/fedora/kfaceauth.spec --with experimental_auth --define "_sourcedir $PWD"
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
scriptlets, model hashes, and license/provenance payload. The ordinary install
must not include or configure PAM, an authentication unit, authselect, a
privileged helper, setuid/capabilities, an evaluator, a fake provider, or an
authentication scriptlet. Inspect the opt-in RPM as a separate unqualified
experiment. Neither package may silently enable a login path.

Release qualification additionally uses ordinary dependency-resolved
`dnf install`, upgrade, and remove in a clean Fedora 44 environment. `--nodeps`
is never clean-install evidence. Installation/upgrade must not create a
profile or KWallet key. Removal must not inspect or delete user-home data;
profile deletion is only an explicit application action.

The isolated smoke test builds a payload-only `plasma-irlume` 3.0.0 fixture,
upgrades it to the tested `kfaceauth` RPM, verifies that every legacy KCM file
is removed, reinstalls the new package, and checks complete package removal
while preserving unrelated user and PAM sentinels.
