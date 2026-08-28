# src/wizardry_am/meson.build
# Wizardry 6 & 7 Automap Mod — DOSBox Staging port

wizardry_am_sources = files(
    'am_main.cpp',
    'am_wiz6.cpp',
    'am_wiz6_res.cpp',
    'am_wiz7.cpp',
    'am_wiz7_res.cpp',
    'InputBox.cpp',
)

wizardry_am_include = include_directories('.')

libwizardry_am = static_library(
    'wizardry_am',
    wizardry_am_sources,
    include_directories : [
        wizardry_am_include,
        # DOSBox Staging's own include dirs — these are defined in the top-level
        # meson.build as 'common_include' or similar; adjust the variable name
        # to match whatever the top-level build exposes.
        common_include,
    ],
    cpp_args : cpp_args,
    dependencies : [
        sdl2_dep,        # SDL2 (already required by Staging)
    ],
)
