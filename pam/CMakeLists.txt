# SPDX-License-Identifier: GPL-3.0-or-later

find_package(PAM QUIET)
if(NOT PAM_FOUND)
    find_library(PAM_LIBRARY NAMES pam)
    find_path(PAM_INCLUDE_DIR NAMES security/pam_modules.h)
    if(PAM_LIBRARY AND PAM_INCLUDE_DIR)
        set(PAM_FOUND TRUE)
    endif()
endif()

if(NOT PAM_FOUND)
    message(FATAL_ERROR "libpam is required to build pam_kfaceauth")
endif()

add_library(pam_kfaceauth MODULE src/pam_kfaceauth.c)

target_include_directories(pam_kfaceauth PRIVATE ${PAM_INCLUDE_DIR})
target_link_libraries(pam_kfaceauth PRIVATE ${PAM_LIBRARY})

set_target_properties(pam_kfaceauth PROPERTIES
    PREFIX ""
    C_STANDARD 17
    C_STANDARD_REQUIRED ON
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
)

target_compile_options(pam_kfaceauth PRIVATE
    -Wall
    -Wextra
    -Wconversion
    -Werror
)

install(TARGETS pam_kfaceauth DESTINATION ${CMAKE_INSTALL_LIBDIR}/security)
