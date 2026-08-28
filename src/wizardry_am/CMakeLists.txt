target_sources(dosbox PRIVATE
    am_main.cpp
    am_wiz6.cpp
    am_wiz6_res.cpp
    am_wiz7.cpp
    am_wiz7_res.cpp
    InputBox.cpp
)

target_include_directories(dosbox PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if (DOSBOX_PLATFORM_WINDOWS)
    target_link_libraries(dosbox PRIVATE comctl32)
endif()