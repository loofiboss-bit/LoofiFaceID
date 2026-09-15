# SPDX-License-Identifier: BSD-2-Clause

add_executable(test_systemstate test_systemstate.cpp)
target_link_libraries(test_systemstate kfaceauth_core Qt6::Core Qt6::Test)
add_test(NAME systemstate COMMAND test_systemstate)
set_tests_properties(systemstate PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_nativefaceauthbackend test_nativefaceauthbackend.cpp)
target_link_libraries(test_nativefaceauthbackend kfaceauth_core Qt6::Core Qt6::Test)
add_test(NAME nativefaceauthbackend COMMAND test_nativefaceauthbackend)

add_executable(test_refreshcoordinator test_refreshcoordinator.cpp)
target_link_libraries(test_refreshcoordinator kfaceauth_core Qt6::Core Qt6::Test)
add_test(NAME refreshcoordinator COMMAND test_refreshcoordinator)

add_executable(fake_preview_worker fake_preview_worker.cpp)
target_link_libraries(fake_preview_worker preview_protocol Qt6::Core Qt6::Gui)

add_executable(fake_vision_worker fake_vision_worker.cpp)
target_link_libraries(fake_vision_worker Qt6::Core)

add_executable(fake_identity_worker fake_identity_worker.cpp)
target_link_libraries(fake_identity_worker Qt6::Core)

add_executable(test_previewprotocol test_previewprotocol.cpp)
target_link_libraries(test_previewprotocol preview_protocol Qt6::Core Qt6::Test)
add_test(NAME previewprotocol COMMAND test_previewprotocol)

add_executable(test_identityprotocol test_identityprotocol.cpp)
target_link_libraries(test_identityprotocol kfaceauth_core Qt6::Core Qt6::Gui Qt6::Test)
add_test(NAME identityprotocol COMMAND test_identityprotocol)

add_executable(test_identityworkerclient test_identityworkerclient.cpp)
target_compile_definitions(test_identityworkerclient PRIVATE
    KFACEAUTH_FAKE_IDENTITY_WORKER_PATH="$<TARGET_FILE:fake_identity_worker>"
)
target_link_libraries(test_identityworkerclient kfaceauth_core Qt6::Core Qt6::Test)
add_dependencies(test_identityworkerclient fake_identity_worker)
add_test(NAME identityworkerclient COMMAND test_identityworkerclient)

add_executable(test_identitysessions test_identitysessions.cpp)
target_compile_definitions(test_identitysessions PRIVATE
    KFACEAUTH_FAKE_IDENTITY_WORKER_PATH="$<TARGET_FILE:fake_identity_worker>"
    KFACEAUTH_FAKE_PREVIEW_WORKER_PATH="$<TARGET_FILE:fake_preview_worker>"
)
target_link_libraries(test_identitysessions kfaceauth_core Qt6::Core Qt6::Gui Qt6::Test)
add_dependencies(test_identitysessions fake_identity_worker fake_preview_worker)
add_test(NAME identitysessions COMMAND test_identitysessions)
set_tests_properties(identitysessions PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_cameraprovider test_cameraprovider.cpp)
target_link_libraries(test_cameraprovider camera_provider Qt6::Core Qt6::Gui Qt6::Test)
add_test(NAME cameraprovider COMMAND test_cameraprovider)

add_executable(test_previewworker test_previewworker.cpp)
target_compile_definitions(test_previewworker PRIVATE
    KFACEAUTH_PREVIEW_WORKER_PATH="$<TARGET_FILE:${KFACEAUTH_PREVIEW_WORKER}>"
)
target_link_libraries(test_previewworker preview_protocol Qt6::Core Qt6::Test)
add_dependencies(test_previewworker ${KFACEAUTH_PREVIEW_WORKER})
add_test(NAME previewworker COMMAND test_previewworker)
set_tests_properties(previewworker PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_camerapreviewsession test_camerapreviewsession.cpp)
target_compile_definitions(test_camerapreviewsession PRIVATE
    KFACEAUTH_FAKE_PREVIEW_WORKER_PATH="$<TARGET_FILE:fake_preview_worker>"
)
target_link_libraries(test_camerapreviewsession kfaceauth_core Qt6::Core Qt6::Gui Qt6::Test)
add_dependencies(test_camerapreviewsession fake_preview_worker)
add_test(NAME camerapreviewsession COMMAND test_camerapreviewsession)

add_executable(test_visionanalysissession test_visionanalysissession.cpp)
target_compile_definitions(test_visionanalysissession PRIVATE
    KFACEAUTH_FAKE_PREVIEW_WORKER_PATH="$<TARGET_FILE:fake_preview_worker>"
    KFACEAUTH_FAKE_VISION_WORKER_PATH="$<TARGET_FILE:fake_vision_worker>"
)
target_link_libraries(test_visionanalysissession kfaceauth_core Qt6::Core Qt6::Gui Qt6::Test)
add_dependencies(test_visionanalysissession fake_preview_worker fake_vision_worker)
add_test(NAME visionanalysissession COMMAND test_visionanalysissession)

add_executable(test_yunet_bridge_native
    test_yunet_bridge_native.cpp
    ${PROJECT_SOURCE_DIR}/engine/vision-opencv-sys/native/yunet_bridge.cpp
)
target_include_directories(test_yunet_bridge_native PRIVATE
    ${PROJECT_SOURCE_DIR}/engine/vision-opencv-sys/native
)
target_compile_definitions(test_yunet_bridge_native PRIVATE
    KFACEAUTH_YUNET_MODEL_PATH="${PROJECT_SOURCE_DIR}/models/files/face_detection_yunet_2023mar.onnx"
    KFACEAUTH_SFACE_MODEL_PATH="${PROJECT_SOURCE_DIR}/models/files/face_recognition_sface_2021dec.onnx"
)
target_compile_options(test_yunet_bridge_native PRIVATE -fexceptions)
target_link_libraries(test_yunet_bridge_native PRIVATE
    opencv_core
    opencv_dnn
    opencv_imgproc
    opencv_objdetect
)
if(KFACEAUTH_ENABLE_NATIVE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_yunet_bridge_native PRIVATE
        -fno-omit-frame-pointer
        -fsanitize=address,undefined
    )
    target_link_options(test_yunet_bridge_native PRIVATE
        -fsanitize=address,undefined
    )
endif()
add_test(NAME yunet_bridge_native COMMAND test_yunet_bridge_native)

add_executable(test_supportreport test_supportreport.cpp)
target_compile_definitions(test_supportreport PRIVATE
    KFACEAUTH_FAKE_PREVIEW_WORKER_PATH="$<TARGET_FILE:fake_preview_worker>"
)
target_link_libraries(test_supportreport kfaceauth_core Qt6::Core Qt6::Gui Qt6::Test)
add_dependencies(test_supportreport fake_preview_worker)
add_test(NAME supportreport COMMAND test_supportreport)
set_tests_properties(supportreport PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_qmlpages
    test_qmlpages.cpp
    ${PROJECT_SOURCE_DIR}/src/kcm/camerapreviewitem.cpp
    ${PROJECT_SOURCE_DIR}/src/kcm/camerapreviewitem.h
)
target_include_directories(test_qmlpages PRIVATE ${PROJECT_SOURCE_DIR}/src/kcm)
target_compile_definitions(test_qmlpages PRIVATE
    KFACEAUTH_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    KFACEAUTH_QML_URI="${KFACEAUTH_QML_URI}"
    KFACEAUTH_VERSION_STRING="${KFACEAUTH_VERSION}"
    KFACEAUTH_FAKE_PREVIEW_WORKER_PATH="$<TARGET_FILE:fake_preview_worker>"
    KFACEAUTH_FAKE_VISION_WORKER_PATH="$<TARGET_FILE:fake_vision_worker>"
)
target_link_libraries(test_qmlpages
    kfaceauth_core
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::Test
    KF6::I18nQml
)
add_dependencies(test_qmlpages fake_preview_worker fake_vision_worker)
add_test(NAME qmlpages COMMAND test_qmlpages)
set_tests_properties(qmlpages PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_kfaceauthkcm
    test_kfaceauthkcm.cpp
    ${PROJECT_SOURCE_DIR}/src/kcm/camerapreviewitem.cpp
    ${PROJECT_SOURCE_DIR}/src/kcm/camerapreviewitem.h
    ${PROJECT_SOURCE_DIR}/src/kcm/kfaceauthkcm.cpp
    ${PROJECT_SOURCE_DIR}/src/kcm/kfaceauthkcm.h
)
target_include_directories(test_kfaceauthkcm PRIVATE ${PROJECT_SOURCE_DIR}/src/kcm)
target_compile_definitions(test_kfaceauthkcm PRIVATE
    KFACEAUTH_PLUGIN_METADATA="${PROJECT_BINARY_DIR}/src/kcm/${KFACEAUTH_KCM_ID}.json"
    KFACEAUTH_QML_URI="${KFACEAUTH_QML_URI}"
    KFACEAUTH_VERSION_STRING="${KFACEAUTH_VERSION}"
)
target_link_libraries(test_kfaceauthkcm
    kfaceauth_core
    Qt6::Core
    Qt6::Qml
    Qt6::Quick
    Qt6::Test
    KF6::CoreAddons
    KF6::I18n
    KF6::KCMUtilsQuick
)
add_test(NAME kfaceauthkcm COMMAND test_kfaceauthkcm)
set_tests_properties(kfaceauthkcm PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(camera_hardware_check camera_hardware_check.cpp)
target_link_libraries(camera_hardware_check kfaceauth_core Qt6::Core Qt6::Gui)
add_dependencies(camera_hardware_check ${KFACEAUTH_PREVIEW_WORKER})

add_executable(test_pam test_pam.cpp ${PROJECT_SOURCE_DIR}/pam/src/pam_kfaceauth.c)
target_include_directories(test_pam PRIVATE ${PROJECT_SOURCE_DIR}/pam/src)
target_link_libraries(test_pam Qt6::Core Qt6::Test)
add_test(NAME pam_conversation COMMAND test_pam)
