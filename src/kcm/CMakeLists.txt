# SPDX-License-Identifier: BSD-2-Clause

configure_file(
    kcm_kfaceauth.json.in
    "${CMAKE_CURRENT_BINARY_DIR}/${KFACEAUTH_KCM_ID}.json"
    @ONLY
)

kcmutils_add_qml_kcm(${KFACEAUTH_KCM_ID}
    DISABLE_DESKTOP_FILE_GENERATION
    SOURCES
        camerapreviewitem.cpp
        camerapreviewitem.h
        kfaceauthkcm.cpp
        kfaceauthkcm.h
        "${CMAKE_CURRENT_BINARY_DIR}/${KFACEAUTH_KCM_ID}.json"
)

target_compile_definitions(${KFACEAUTH_KCM_ID} PRIVATE
    KFACEAUTH_PLUGIN_METADATA="${CMAKE_CURRENT_BINARY_DIR}/${KFACEAUTH_KCM_ID}.json"
    KFACEAUTH_QML_URI="${KFACEAUTH_QML_URI}"
    KFACEAUTH_VERSION_STRING="${KFACEAUTH_VERSION}"
)

target_link_libraries(${KFACEAUTH_KCM_ID}
    kfaceauth_core
    Qt6::Core
    Qt6::Qml
    Qt6::Quick
    KF6::CoreAddons
    KF6::I18n
    KF6::KCMUtilsQuick
)
