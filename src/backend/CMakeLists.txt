# SPDX-License-Identifier: BSD-2-Clause

add_library(kfaceauth_core STATIC
    camerapreviewsession.cpp
    camerapreviewsession.h
    enrollmentsession.cpp
    enrollmentsession.h
    faceauthbackend.h
    identityprotocol.cpp
    identityprotocol.h
    identityworkerclient.cpp
    identityworkerclient.h
    kwalletkeyprovider.cpp
    kwalletkeyprovider.h
    localverificationsession.cpp
    localverificationsession.h
    nativefaceauthbackend.cpp
    nativefaceauthbackend.h
    refreshcoordinator.cpp
    refreshcoordinator.h
    supportreport.cpp
    supportreport.h
    systemstate.cpp
    systemstate.h
    systemauthprotocol.cpp
    systemauthprotocol.h
    systemprobe.cpp
    systemprobe.h
    visionanalysissession.cpp
    visionanalysissession.h
)

set_target_properties(kfaceauth_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_link_libraries(kfaceauth_core
    PUBLIC
        preview_protocol
        Qt6::Concurrent
        Qt6::Core
        Qt6::Gui
        KF6::Wallet
        OpenSSL::Crypto
)
target_include_directories(kfaceauth_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(kfaceauth_core PRIVATE
    KFACEAUTH_PREVIEW_WORKER_PATH="${CMAKE_INSTALL_PREFIX}/libexec/${KFACEAUTH_PREVIEW_WORKER}"
    KFACEAUTH_VISION_WORKER_PATH="${CMAKE_INSTALL_PREFIX}/libexec/${KFACEAUTH_VISION_WORKER}"
    KFACEAUTH_IDENTITY_WORKER_PATH="${CMAKE_INSTALL_PREFIX}/libexec/${KFACEAUTH_IDENTITY_WORKER}"
    KFACEAUTH_MODEL_ROOT="${CMAKE_INSTALL_PREFIX}/share/${KFACEAUTH_MODEL_DIRECTORY}"
    KFACEAUTH_SUPPORT_PREFIX="${KFACEAUTH_SUPPORT_PREFIX}"
)
