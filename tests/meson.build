test_roc_modules = executable('test-roc-modules',
    'test-roc-modules.c',
    dependencies : [roc_dep, pw_dep, spa_dep,
                    cc.find_library('m'),
                    dependency('threads')],
    # Needed for SPA_STRINGIFY and spa_format_audio_raw_build.
    include_directories : include_directories('..'),
)

# PIPEWIRE_MODULE_DIR prepended to the PipeWire module search path so that
# the freshly-built modules are found without requiring `meson install`.
test('roc-modules', test_roc_modules,
    timeout     : 90,
    is_parallel : false,
    should_fail : false,
    env         : ['PIPEWIRE_MODULE_DIR=' + meson.project_build_root() + ':' + moduledir],
)
