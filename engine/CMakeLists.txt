# SPDX-License-Identifier: BSD-2-Clause

find_program(CARGO_EXECUTABLE cargo REQUIRED)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(KFACEAUTH_CARGO_PROFILE "debug")
    set(KFACEAUTH_CARGO_PROFILE_ARGUMENTS)
else()
    set(KFACEAUTH_CARGO_PROFILE "release")
    set(KFACEAUTH_CARGO_PROFILE_ARGUMENTS --release)
endif()

set(KFACEAUTH_CARGO_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/target")
set(KFACEAUTH_VISION_WORKER_BINARY
    "${KFACEAUTH_CARGO_TARGET_DIR}/${KFACEAUTH_CARGO_PROFILE}/${KFACEAUTH_VISION_WORKER}"
)
set(KFACEAUTH_IDENTITY_WORKER_BINARY
    "${KFACEAUTH_CARGO_TARGET_DIR}/${KFACEAUTH_CARGO_PROFILE}/${KFACEAUTH_IDENTITY_WORKER}"
)
set(KFACEAUTH_REPRODUCIBLE_CXXFLAGS
    "$ENV{CXXFLAGS} -gno-record-gcc-switches"
)
set(KFACEAUTH_RUSTFLAGS)
if(DEFINED ENV{RUSTFLAGS} AND NOT "$ENV{RUSTFLAGS}" STREQUAL "")
    separate_arguments(KFACEAUTH_RUSTFLAGS UNIX_COMMAND "$ENV{RUSTFLAGS}")
endif()
list(APPEND KFACEAUTH_RUSTFLAGS
    "--remap-path-prefix=${PROJECT_SOURCE_DIR}=."
)
string(ASCII 31 KFACEAUTH_RUSTFLAG_SEPARATOR)
list(JOIN KFACEAUTH_RUSTFLAGS
    "${KFACEAUTH_RUSTFLAG_SEPARATOR}"
    KFACEAUTH_ENCODED_RUSTFLAGS
)
if(KFACEAUTH_HAS_OPENVINO)
    set(KFACEAUTH_OPENVINO_ENV "1")
else()
    set(KFACEAUTH_OPENVINO_ENV "0")
endif()

file(GLOB_RECURSE KFACEAUTH_RUST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.rs"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)

add_custom_command(
    OUTPUT
        "${KFACEAUTH_VISION_WORKER_BINARY}"
        "${KFACEAUTH_IDENTITY_WORKER_BINARY}"
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "CARGO_TARGET_DIR=${KFACEAUTH_CARGO_TARGET_DIR}"
        "CARGO_ENCODED_RUSTFLAGS=${KFACEAUTH_ENCODED_RUSTFLAGS}"
        "KFACEAUTH_SOURCE_ROOT=${PROJECT_SOURCE_DIR}"
        "KFACEAUTH_HAS_OPENVINO=${KFACEAUTH_OPENVINO_ENV}"
        "CXXFLAGS=${KFACEAUTH_REPRODUCIBLE_CXXFLAGS}"
        "${CARGO_EXECUTABLE}"
        --config "${PROJECT_SOURCE_DIR}/.cargo/config.toml"
        build
        --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
        --package kfaceauth-vision
        --bin "${KFACEAUTH_VISION_WORKER}"
        --package kfaceauth-identity
        --bin "${KFACEAUTH_IDENTITY_WORKER}"
        --locked
        --offline
        ${KFACEAUTH_CARGO_PROFILE_ARGUMENTS}
    DEPENDS
        ${KFACEAUTH_RUST_SOURCES}
        "${PROJECT_SOURCE_DIR}/.cargo/config.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock"
        "${PROJECT_SOURCE_DIR}/models/manifest.kfaceauth"
        "${PROJECT_SOURCE_DIR}/models/files/face_detection_yunet_2023mar.onnx"
        "${PROJECT_SOURCE_DIR}/models/files/face_recognition_sface_2021dec.onnx"
    VERBATIM
    COMMENT "Building the offline Rust vision worker"
)

add_custom_target(kfaceauth_vision_worker ALL
    DEPENDS
        "${KFACEAUTH_VISION_WORKER_BINARY}"
        "${KFACEAUTH_IDENTITY_WORKER_BINARY}"
)

add_custom_target(kfaceauth_verify_models ALL
    COMMAND
        "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tools/verify_models.py"
        --root "${PROJECT_SOURCE_DIR}/models"
    DEPENDS
        "${PROJECT_SOURCE_DIR}/models/manifest.kfaceauth"
        "${PROJECT_SOURCE_DIR}/tools/verify_models.py"
    VERBATIM
    COMMENT "Verifying the offline model supply chain"
)

install(PROGRAMS "${KFACEAUTH_VISION_WORKER_BINARY}"
    DESTINATION libexec
    RENAME "${KFACEAUTH_VISION_WORKER}"
)

install(PROGRAMS "${KFACEAUTH_IDENTITY_WORKER_BINARY}"
    DESTINATION libexec
    RENAME "${KFACEAUTH_IDENTITY_WORKER}"
)

install(DIRECTORY "${PROJECT_SOURCE_DIR}/models/"
    DESTINATION "${KDE_INSTALL_DATADIR}/${KFACEAUTH_MODEL_DIRECTORY}"
    FILES_MATCHING
        PATTERN "manifest.kfaceauth"
        PATTERN "files/*"
        PATTERN "licenses/*"
        PATTERN "provenance/*"
)
