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

# Systemd socket activation and service units
install(
    FILES
        systemd/kfaceauth.service
        systemd/kfaceauth.socket
    DESTINATION lib/systemd/system
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
    DESTINATION "${KDE_INSTALL_DATADIR}/kfaceauth/selinux"
)

find_program(CHECKMODULE_EXECUTABLE checkmodule)
if(CHECKMODULE_EXECUTABLE)
    add_custom_target(kfaceauth_validate_selinux ALL
        COMMAND "${CHECKMODULE_EXECUTABLE}" -M -m -o "${CMAKE_CURRENT_BINARY_DIR}/kfaceauth.mod" "${CMAKE_CURRENT_SOURCE_DIR}/selinux/kfaceauth.te"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/selinux/kfaceauth.te"
        COMMENT "Validating SELinux policy module with checkmodule"
    )
endif()
