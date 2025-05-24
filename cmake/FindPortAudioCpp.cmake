# FindPortAudioCpp.cmake
#
# Locates PortAudio C++ bindings (portaudiocpp)
# First tries CMake's find_package with CONFIG mode,
# then falls back to pkg-config.
#
# Defines:
#   PortAudioCpp::PortAudioCpp - Imported target if found
#   PORTAUDIOCPP_FOUND          - TRUE if the library was found

# --- Try CMake Config Mode First ---
set(PORTAUDIOCPP_FOUND FALSE)

find_package(PortAudio CONFIG QUIET)

if(TARGET PortAudio::portaudiocpp)
    add_library(PortAudioCpp::PortAudioCpp INTERFACE IMPORTED)
    target_link_libraries(
        PortAudioCpp::PortAudioCpp
        INTERFACE PortAudio::portaudiocpp
    )
    set(PORTAUDIOCPP_FOUND TRUE)
endif()

# --- Fallback: pkg-config ---
if(NOT PORTAUDIOCPP_FOUND)
    find_package(PkgConfig QUIET)
    pkg_check_modules(PKG_PORTAUDIOCPP QUIET portaudiocpp)

    if(PKG_PORTAUDIOCPP_FOUND)
        add_library(PortAudioCpp::PortAudioCpp INTERFACE IMPORTED)
        set_target_properties(
            PortAudioCpp::PortAudioCpp
            PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${PKG_PORTAUDIOCPP_INCLUDE_DIRS}"
                INTERFACE_LINK_LIBRARIES "${PKG_PORTAUDIOCPP_LIBRARIES}"
        )
        set(PORTAUDIOCPP_FOUND TRUE)
    endif()
endif()

# --- Report result ---
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(PortAudioCpp DEFAULT_MSG PORTAUDIOCPP_FOUND)
