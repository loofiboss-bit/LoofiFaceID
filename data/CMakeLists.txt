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

    install(
        FILES
            pam/sddm-kfaceauth
            pam/kde-kfaceauth
        DESTINATION "${KDE_INSTALL_DATADIR}/kfaceauth/pam"
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
            selinux/kfaceauth_sddm.te
        DESTINATION "${KDE_INSTALL_DATADIR}/kfaceauth/selinux"
    )

    find_program(CHECKMODULE_EXECUTABLE checkmodule REQUIRED)
    find_program(SEMODULE_PACKAGE_EXECUTABLE semodule_package REQUIRED)

    set(KFACEAUTH_SELINUX_MODULES kfaceauth kfaceauth_sddm)
    foreach(module IN LISTS KFACEAUTH_SELINUX_MODULES)
        set(module_source "${CMAKE_CURRENT_SOURCE_DIR}/selinux/${module}.te")
        set(module_binary "${CMAKE_CURRENT_BINARY_DIR}/${module}.mod")
        set(module_package "${CMAKE_CURRENT_BINARY_DIR}/${module}.pp")
        if(module STREQUAL "kfaceauth")
            set(module_file_contexts "${CMAKE_CURRENT_SOURCE_DIR}/selinux/kfaceauth.fc")
            add_custom_command(
                OUTPUT "${module_package}"
                COMMAND "${CHECKMODULE_EXECUTABLE}" -M -m -o "${module_binary}" "${module_source}"
                COMMAND "${SEMODULE_PACKAGE_EXECUTABLE}" -o "${module_package}" -m "${module_binary}" -f "${module_file_contexts}"
                DEPENDS "${module_source}" "${module_file_contexts}"
                VERBATIM
                COMMENT "Building SELinux module ${module}"
            )
        else()
            add_custom_command(
                OUTPUT "${module_package}"
                COMMAND "${CHECKMODULE_EXECUTABLE}" -M -m -o "${module_binary}" "${module_source}"
                COMMAND "${SEMODULE_PACKAGE_EXECUTABLE}" -o "${module_package}" -m "${module_binary}"
                DEPENDS "${module_source}"
                VERBATIM
                COMMENT "Building SELinux module ${module}"
            )
        endif()
        list(APPEND KFACEAUTH_SELINUX_PACKAGES "${module_package}")
    endforeach()
    add_custom_target(kfaceauth_selinux_policy ALL DEPENDS ${KFACEAUTH_SELINUX_PACKAGES})
    install(FILES ${KFACEAUTH_SELINUX_PACKAGES}
        DESTINATION "${KDE_INSTALL_DATADIR}/kfaceauth/selinux"
    )

    # Polkit policy
    install(
        FILES
            polkit/org.kde.kfaceauth.policy
        DESTINATION "${KDE_INSTALL_DATADIR}/polkit-1/actions"
    )

endif()
