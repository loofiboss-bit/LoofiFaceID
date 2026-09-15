%global source_date_epoch_from_changelog 1
%global use_source_date_epoch_as_buildtime 1
%global clamp_mtime_to_source_date_epoch 1

Name:           kfaceauth
Version:        5.0.0
Release:        1%{?dist}
Summary:        Biometric facial authentication daemon, PAM module and KDE settings

License:        GPL-3.0-or-later AND MIT AND Apache-2.0
URL:            https://github.com/loofiboss-bit/LoofiFaceID
Source0:        %{url}/releases/download/v%{version}/%{name}-%{version}.tar.gz

Obsoletes:      plasma-irlume < 5.0.0
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
BuildRequires:  checkpolicy
BuildRequires:  pam-devel
BuildRequires:  rust
BuildRequires:  rustfmt
BuildRequires:  systemd-devel

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
KFaceAuth v5.0 is an ultra-low-latency, publication-grade biometric facial
authentication architecture for Linux and KDE Plasma 6. It features a hardened
system daemon with Landlock and Seccomp isolation, Linux PAM module, zero-copy
sealed shared memory frame transfer, hardware acceleration via OpenVINO and
Vulkan, presentation attack detection conforming to ISO/IEC 30107-3 (active
eye-blink tracking, randomized micro-pose challenge response, passive FFT
moire and texture analysis, and multi-spectrum NIR qualification), and a
modernized QML/Kirigami 30 FPS scene graph user interface in KDE System
Settings.

%prep
%autosetup -p1

%build
%cmake \
    %{?kfaceauth_cmake_extra} \
    -DBUILD_TESTING=ON
%cmake_build

%install
%cmake_install
install -d -m 0750 %{buildroot}%{_sharedstatedir}/kfaceauth
install -d -m 0750 %{buildroot}%{_sysconfdir}/kfaceauth
install -d -m 0700 %{buildroot}%{_sysconfdir}/kfaceauth/keys
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
%doc docs/*.md
%{_qt6_plugindir}/plasma/kcms/systemsettings/kcm_kfaceauth.so
%{_libexecdir}/kfaceauth-camera-preview-worker
%{_libexecdir}/kfaceauth-vision-worker
%{_libexecdir}/kfaceauth-identity-worker
%{_libexecdir}/kfaceauthd
%{_bindir}/kfaceauth-migrate-vault
%{_libdir}/security/pam_kfaceauth.so
%{_unitdir}/kfaceauth.service
%{_unitdir}/kfaceauth.socket
%{_sysusersdir}/kfaceauth.conf
%dir %{_datadir}/kfaceauth
%dir %{_datadir}/kfaceauth/selinux
%{_datadir}/kfaceauth/selinux/kfaceauth.te
%{_datadir}/kfaceauth/selinux/kfaceauth.fc
%{_datadir}/kfaceauth/selinux/kfaceauth.if
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
%dir %attr(0750,root,kfaceauth) %{_sharedstatedir}/kfaceauth
%dir %attr(0750,root,kfaceauth) %{_sysconfdir}/kfaceauth
%dir %attr(0700,root,root) %{_sysconfdir}/kfaceauth/keys

%changelog
* Tue Sep 15 2026 Loofi <noreply@example.invalid> - 5.0.0-1
- Release v5.0.0
- Milestone 1: Persistent pool, warm models, and zero-copy memfd IPC frame transfer
- Milestone 2: QML/Kirigami UX modernization, 30 FPS scene graph, guided enrollment
- Milestone 3: Hardware acceleration with OpenVINO and Vulkan GPU offload
- Milestone 4: Privilege separation, kfaceauthd system daemon, PAM module (pam_kfaceauth)
- Milestone 5: Presentation attack detection (PAD) conforming to ISO/IEC 30107-3

* Tue Jul 28 2026 Loofi <noreply@example.invalid> - 4.0.0-1
- Start the standalone native architecture with fail-closed engine status
- Preserve the bounded unprivileged camera preview
- Add bounded one-frame production YuNet detection through Fedora OpenCV
- Keep fake inference test-only and preserve the verified model supply chain
- Add bounded encrypted user-session enrollment and explicit local comparison
