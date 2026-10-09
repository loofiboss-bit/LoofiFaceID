%global source_date_epoch_from_changelog 1
%global use_source_date_epoch_as_buildtime 1
%global clamp_mtime_to_source_date_epoch 1
%bcond_with experimental_auth

Name:           kfaceauth
Version:        5.4.0
Release:        1%{?dist}
Summary:        Experimental local face profile and comparison utility for KDE

License:        GPL-3.0-or-later AND MIT AND Apache-2.0
URL:            https://github.com/loofiboss-bit/LoofiFaceID
Source0:        %{url}/releases/download/v%{version}/%{name}-%{version}.tar.gz

Obsoletes:      plasma-irlume < 5.1.0
Provides:       plasma-irlume = %{version}-%{release}

BuildRequires:  cmake >= 3.22
BuildRequires:  cmake-rpm-macros
BuildRequires:  systemd-rpm-macros
BuildRequires:  cargo
BuildRequires:  clang-tools-extra
BuildRequires:  clippy
BuildRequires:  desktop-file-utils
BuildRequires:  extra-cmake-modules >= 6.10.0
BuildRequires:  gcc-c++
BuildRequires:  gettext
BuildRequires:  git-core
BuildRequires:  kf6-kcmutils-devel >= 6.10.0
BuildRequires:  kf6-kcoreaddons-devel >= 6.10.0
BuildRequires:  kf6-ki18n-devel >= 6.10.0
BuildRequires:  kf6-kirigami-devel >= 6.10.0
BuildRequires:  kf6-kwallet-devel >= 6.10.0
BuildRequires:  ninja-build
BuildRequires:  opencv-devel >= 4.8.0
BuildRequires:  openssl-devel >= 3.0.0
BuildRequires:  python3
BuildRequires:  qt6-qtbase-devel >= 6.8.0
BuildRequires:  qt6-qtdeclarative-devel >= 6.8.0
BuildRequires:  qt6-qtmultimedia-devel >= 6.8.0
BuildRequires:  rust
BuildRequires:  rustfmt
BuildRequires:  systemd-devel
%if %{with experimental_auth}
BuildRequires:  checkpolicy
BuildRequires:  pam-devel
BuildRequires:  policycoreutils-devel
%endif

Requires:       kf6-kcmutils >= 6.10.0
Requires:       kf6-kirigami >= 6.10.0
Requires:       kf6-kwallet >= 6.10.0
Requires:       opencv-calib3d >= 4.8.0
Requires:       opencv-core >= 4.8.0
Requires:       opencv-dnn >= 4.8.0
Requires:       opencv-flann >= 4.8.0
Requires:       opencv-imgproc >= 4.8.0
Requires:       opencv-objdetect >= 4.8.0
Requires:       openssl-libs >= 3.0.0
Requires:       plasma-systemsettings
Requires:       qt6-qtdeclarative >= 6.8.0
Requires:       qt6-qtmultimedia >= 6.8.0

%description
KFaceAuth is an experimental local face-profile and explicit comparison
utility for a logged-in Fedora 44/KDE Plasma session. The KCM provides private
camera guidance, a five-pose enrollment flow, encrypted KWallet-backed profile
storage, and one-frame local comparison. The standard package contains no PAM
module, authentication daemon, authentication units, or SELinux policy. It
makes no PAD, performance, or authentication qualification claim.

%if %{with experimental_auth}
%package experimental-auth
Summary:        Opt-in experimental PAM face authentication for SDDM and Plasma
Requires:       %{name}%{?_isa} = %{version}-%{release}
Requires:       pam
Requires:       polkit
Requires:       policycoreutils
Requires:       systemd

%description experimental-auth
This opt-in experimental package adds an unqualified PAM face-authentication
path for SDDM and the KDE Plasma lock screen. Password authentication remains
available as the PAM fallback. This package is disabled in ordinary and COPR
builds. Do not treat it as supported authentication: physical device,
presentation-attack, usability, and independent security qualification remain
open.
%endif

%prep
%autosetup -p1

%build
%if %{with experimental_auth}
%cmake \
    %{?kfaceauth_cmake_extra} \
    -DKFACEAUTH_BUILD_EXPERIMENTAL_AUTH_COMPONENTS=ON \
    -DBUILD_TESTING=ON
%else
%cmake \
    %{?kfaceauth_cmake_extra} \
    -DKFACEAUTH_BUILD_EXPERIMENTAL_AUTH_COMPONENTS=OFF \
    -DBUILD_TESTING=ON
%endif
%cmake_build

%install
%cmake_install
if find %{buildroot}%{_datadir}/locale -type f -name 'kcm_kfaceauth.mo' -print -quit \
    | grep -q .; then
    %find_lang kcm_kfaceauth
else
    : >kcm_kfaceauth.lang
fi

%check
export QT_QPA_PLATFORM=offscreen
%ctest
python3 -m unittest discover -s tests -p 'test_*.py'
python3 tools/verify_models.py --root models
python3 tools/verify_project_identity.py
%{_qt6_bindir}/qmllint src/kcm/ui/*.qml src/kcm/ui/components/*.qml
find src tests/unit engine/vision-opencv-sys/native engine/crypto-openssl-sys/native \
    -type f \( -name '*.cpp' -o -name '*.h' \) -print0 \
    | xargs -0 clang-format --dry-run --Werror
cargo fmt --manifest-path engine/Cargo.toml --all -- --check
cargo clippy --manifest-path engine/Cargo.toml \
    --workspace --all-targets --locked --offline -- -D warnings
cargo test --manifest-path engine/Cargo.toml \
    --workspace --all-targets --locked --offline
desktop-file-validate %{buildroot}%{_datadir}/applications/kcm_kfaceauth.desktop

%files -f kcm_kfaceauth.lang
%license LICENSE
%doc CHANGELOG.md README.md
%doc docs/ANVANDARGUIDE-SV.md
%doc docs/ARCHITECTURE.md
%doc docs/BUILDING.md
%doc docs/THREAT-BOUNDARY.md
%doc docs/TROUBLESHOOTING.md
%doc docs/USER-GUIDE.md
%{_qt6_plugindir}/plasma/kcms/systemsettings/kcm_kfaceauth.so
%{_libexecdir}/kfaceauth-camera-preview-worker
%{_libexecdir}/kfaceauth-vision-worker
%{_libexecdir}/kfaceauth-identity-worker
%dir %{_datadir}/kfaceauth/models
%dir %{_datadir}/kfaceauth/models/files
%dir %{_datadir}/kfaceauth/models/licenses
%dir %{_datadir}/kfaceauth/models/provenance
%{_datadir}/kfaceauth/models/manifest.kfaceauth
%{_datadir}/kfaceauth/models/files/face_detection_yunet_2023mar.onnx
%{_datadir}/kfaceauth/models/files/face_recognition_sface_2021dec.onnx
%license %{_datadir}/kfaceauth/models/licenses/sface-Apache-2.0.txt
%license %{_datadir}/kfaceauth/models/licenses/yunet-MIT.txt
%{_datadir}/kfaceauth/models/provenance/sface-2021dec.txt
%{_datadir}/kfaceauth/models/provenance/yunet-2023mar.txt
%{_datadir}/applications/kcm_kfaceauth.desktop

%if %{with experimental_auth}
%files experimental-auth
%{_bindir}/kfaceauth-pam-setup
%{_libdir}/security/pam_kfaceauth.so
%{_libexecdir}/kfaceauth-auth-worker
%{_libexecdir}/kfaceauthd
%{_libexecdir}/kfaceauth-sync-vault
%{_libexecdir}/kfaceauth-policy
%{_libexecdir}/kfaceauth-camera-config
%{_unitdir}/kfaceauth.service
%{_unitdir}/kfaceauth.socket
%{_sysusersdir}/kfaceauth.conf
%{_datadir}/polkit-1/actions/org.kde.kfaceauth.policy
%dir %{_datadir}/kfaceauth/pam
%{_datadir}/kfaceauth/pam/sddm-kfaceauth
%{_datadir}/kfaceauth/pam/kde-kfaceauth
%dir %{_datadir}/kfaceauth/selinux
%{_datadir}/kfaceauth/selinux/kfaceauth.fc
%{_datadir}/kfaceauth/selinux/kfaceauth.te
%{_datadir}/kfaceauth/selinux/kfaceauth_sddm.te
%{_datadir}/kfaceauth/selinux/kfaceauth.pp
%{_datadir}/kfaceauth/selinux/kfaceauth_sddm.pp
%endif

%changelog
* Fri Oct 09 2026 Loofi <noreply@example.invalid> - 5.4.0-1
- Add bounded camera recovery, verified profile sync status, and experimental camera configuration
- Preserve standard package and unqualified authentication boundaries

* Mon Oct 05 2026 Loofi <noreply@example.invalid> - 5.3.0-1
- Improve local enrollment recovery, diagnostics, camera selection, and source release integrity
- Keep PAM authentication opt-in and unqualified

* Mon Oct 05 2026 Loofi <noreply@example.invalid> - 5.2.1-1
- Require systemd-devel for the standard build's libudev development files.

* Mon Oct 05 2026 Loofi <noreply@example.invalid> - 5.2.0-13
- Restrict privileged vault helper output to static, non-identifying status
- Require root in CI before ownership and rollback regression tests

* Sun Oct 04 2026 Loofi <noreply@example.invalid> - 5.2.0-12
- Allow SDDM's confined helper to connect to the dedicated KFaceAuth daemon socket

* Sun Oct 04 2026 Loofi <noreply@example.invalid> - 5.2.0-11
- Read protected system-profile readiness through the daemon status protocol
- Allow the daemon to map V4L2 device buffers under its dedicated SELinux domain

* Sun Oct 04 2026 Loofi <noreply@example.invalid> - 5.2.0-10
- Allow systemd to bind the dedicated KFaceAuth socket during activation

* Sun Oct 04 2026 Loofi <noreply@example.invalid> - 5.2.0-9
- Permit only the KFaceAuth systemd service's SELinux NoNewPrivileges transition
- Define the daemon SELinux role and executable entrypoint permissions

* Sun Oct 04 2026 Loofi <noreply@example.invalid> - 5.2.0-8
- Restore SELinux labels for system profiles and keys transactionally
- Permit systemd to mount the writable namespace only on KFaceAuth profile data

* Sun Oct 04 2026 Loofi <noreply@example.invalid> - 5.2.0-7
- Detect explicitly installed experimental authentication components at runtime

* Sun Oct 04 2026 Loofi <noreply@example.invalid> - 5.2.0-6
- Qualify standard and opt-in package payloads separately
- Limit packaged documentation to user and developer guides
- Make PAM setup rollback transactional across host mutations

* Sun Oct 04 2026 Loofi <noreply@example.invalid> - 5.2.0-3
- Store fully qualified SELinux file contexts in the module package

* Sun Oct 04 2026 Loofi <noreply@example.invalid> - 5.2.0-2
- Recognize only valid managed PAM blocks as enabled in the KCM
- Safely adopt the exact prior PAM rule while retaining password-auth fallback
- Preserve an existing system profile when its separate key is missing

* Sat Oct 03 2026 Loofi <noreply@example.invalid> - 5.2.0-1
- Make experimental PAM authentication an explicit opt-in RPM subpackage and CMake build
- Keep the standard package free of daemon, PAM, units, and SELinux policy
- Scope SELinux labeling to the socket to preserve systemd runtime-directory sandboxing
- Separate the pre-session system profile and key from the KWallet local-comparison profile
- Keep SDDM and Plasma lock-screen authentication unqualified and disabled by default

* Wed Sep 23 2026 Loofi <noreply@example.invalid> - 5.1.0-1
- Separate YuNet edge, invalid-output, and runtime errors with bounded guidance recovery
- Keep system authentication components out of the base package while KDE and physical qualification gates remain open
- Load system keys only when explicitly provisioned; status requests never create key material

* Tue Sep 15 2026 Loofi <noreply@example.invalid> - 5.0.0-1
- Experimental local profile/comparison KCM with guided five-pose enrollment
- Persistent vision guidance session, validated YuNet landmarks, and typed recovery states
- Explicit COPR-first installation boundary; no automatic PAM/SDDM/sudo/Polkit activation

* Tue Jul 28 2026 Loofi <noreply@example.invalid> - 4.0.0-1
- Start the standalone native architecture with fail-closed engine status
- Preserve the bounded unprivileged camera preview
- Add bounded one-frame production YuNet detection through Fedora OpenCV
- Keep fake inference test-only and preserve the verified model supply chain
- Add bounded encrypted user-session enrollment and explicit local comparison
