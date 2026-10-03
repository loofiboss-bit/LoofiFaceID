# SPDX-License-Identifier: BSD-2-Clause

configure_file(
    kcm_kfaceauth.desktop.in
    "${CMAKE_CURRENT_BINARY_DIR}/${KFACEAUTH_KCM_ID}.desktop"
    @ONLY
)
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/${KFACEAUTH_KCM_ID}.desktop"
    DESTINATION "${KDE_INSTALL_APPDIR}"
)

if(KFACEAUTH_BUILD_EXPERIMENTAL_AUTH_COMPONENTS)
    set(KFACEAUTH_ENABLE_SYSTEM_DATA ON)
elseif(KFACEAUTH_BUILD_SYSTEM_AUTH)
    set(KFACEAUTH_ENABLE_SYSTEM_DATA ON)
endif()

if(KFACEAUTH_ENABLE_SYSTEM_DATA)
    # Systemd socket activation and service units
    install(
        FILES
            systemd/kfaceauth.service
            systemd/kfaceauth.socket
        DESTINATION lib/systemd/system
    )

    install(
        PROGRAMS
            pam/kfaceauth-pam-setup.sh
        DESTINATION bin
        RENAME "kfaceauth-pam-setup"
    )

    # Systemd sysusers configuration
    install(
        FILES
            sysusers/kfaceauth.conf
        DESTINATION lib/sysusers.d
    )

    # SELinux policy files
    install(
        FILES
            selinux/kfaceauth.te
            selinux/kfaceauth.fc
            selinux/kfaceauth.if
            selinux/kfaceauth_sddm.te
            selinux/kfaceauth_sddm.fc
        DESTINATION "${KDE_INSTALL_DATADIR}/kfaceauth/selinux"
    )

    # Polkit policy
    install(
        FILES
            polkit/org.kde.kfaceauth.policy
        DESTINATION "${KDE_INSTALL_DATADIR}/polkit-1/actions"
    )

    find_program(CHECKMODULE_EXECUTABLE checkmodule)
    if(CHECKMODULE_EXECUTABLE)
        add_custom_target(kfaceauth_validate_selinux ALL
            COMMAND "${CHECKMODULE_EXECUTABLE}" -M -m -o "${CMAKE_CURRENT_BINARY_DIR}/kfaceauth.mod" "${CMAKE_CURRENT_SOURCE_DIR}/selinux/kfaceauth.te"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/selinux/kfaceauth.te"
            COMMENT "Validating SELinux policy module with checkmodule"
        )
        add_custom_target(kfaceauth_validate_sddm_selinux ALL
            COMMAND "${CHECKMODULE_EXECUTABLE}" -M -m -o "${CMAKE_CURRENT_BINARY_DIR}/kfaceauth_sddm.mod" "${CMAKE_CURRENT_SOURCE_DIR}/selinux/kfaceauth_sddm.te"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/selinux/kfaceauth_sddm.te"
            COMMENT "Validating SDDM SELinux policy module with checkmodule"
        )
    endif()
endif()
