project('pipewire-roc-modules', 'c',
  version : '1.0',
  meson_version : '>=0.56.0'
)

fs = import('fs')
cc = meson.get_compiler('c')

force_static_roc = get_option('force-static-roc')
if not force_static_roc
  roc_dep = dependency('roc', required : false, static : false, method : 'pkg-config')
else
  roc_dep = disabler()
endif

if force_static_roc or not roc_dep.found()
  if force_static_roc
    message('force-static-roc option enabled, building roc-toolkit from 3rdparty')
  else
    message('System roc not found, building roc-toolkit from 3rdparty')
  endif
  
  roc_toolkit_dir = meson.current_source_dir() / '3rdparty' / 'roc-toolkit'
  
  if not fs.is_dir(roc_toolkit_dir)
    error('roc dependency not found system-wide and 3rdparty/roc-toolkit does not exist')
  endif
  
  scons = find_program('scons', required : true)
  roc_install_prefix = meson.project_build_root() / 'roc-toolkit'

  # Force roc-toolkit's scons to use the same compiler family as meson.
  # Otherwise scons may auto-detect clang while meson uses gcc, leading to
  # mismatched toolchains and 3rd-party (e.g. openfec) build failures.
  cc_id = cc.get_id()
  if cc_id == 'gcc'
    roc_compiler = 'gcc'
  elif cc_id == 'clang'
    roc_compiler = 'clang'
  else
    roc_compiler = 'cc'
  endif

  scons_options = [
    '--compiler=' + roc_compiler,
    '--enable-static',
    '--disable-shared',
    '--disable-tools',
    '--enable-werror',
    '--build-3rdparty=all',
    '--disable-pulseaudio',
    '--disable-openssl',
    '--disable-libunwind',
    '--prefix=' + roc_install_prefix
  ]
  
  message('Building roc-toolkit with scons...')
  # openfec's CMakeLists.txt declares cmake_minimum_required(VERSION 2.x).
  # CMake >= 4.0 (shipped in ubuntu:26.04, fedora rawhide, alpine edge, etc.)
  # removes compatibility with < 3.5 and refuses to configure. Setting
  # CMAKE_POLICY_VERSION_MINIMUM=3.5 makes CMake apply 3.5 policies, allowing
  # configuration to proceed. See https://cmake.org/cmake/help/latest/variable/CMAKE_POLICY_VERSION_MINIMUM.html
  scons_env_prefix = 'CMAKE_POLICY_VERSION_MINIMUM=3.5 '
  build_result = run_command(
    'sh', '-c',
    'cd "' + roc_toolkit_dir + '" && ' +
    scons_env_prefix +
    scons.full_path() + ' -Q ' + ' '.join(scons_options),
    check: false,
    capture: true
  )
  
  if build_result.returncode() != 0
    error('Failed to build roc-toolkit:\n' + build_result.stdout() + '\n' + build_result.stderr())
  endif
  
  message('roc-toolkit build completed successfully')
  message('Installing roc-toolkit to: ' + roc_install_prefix)
  install_result = run_command(
    'sh', '-c',
    'cd "' + roc_toolkit_dir + '" && ' +
    scons_env_prefix +
    scons.full_path() + ' -Q ' + ' '.join(scons_options) + ' install',
    check: false,
    capture: true
  )
  
  if install_result.returncode() != 0
    error('Failed to install roc-toolkit:\n' + install_result.stdout() + '\n' + install_result.stderr())
  endif
  
  message('roc-toolkit is built')
  
  # System libraries needed by static roc
  thread_dep = dependency('threads')
  dl_lib = cc.find_library('dl', required : true)
  m_lib = cc.find_library('m', required : true)
  rt_lib = cc.find_library('rt', required : false)
  stdcpp_lib = cc.find_library('stdc++', required : true)
  
  roc_lib_path = roc_install_prefix / 'lib' / 'libroc.a'
  
  roc_dep = declare_dependency(
    link_args : [roc_lib_path],
    include_directories : include_directories('roc-toolkit/include'),
    dependencies : [thread_dep, dl_lib, m_lib, rt_lib, stdcpp_lib]
  )
endif

pw_dep = dependency('libpipewire-0.3', required : true, static : false, method : 'pkg-config')
spa_dep = dependency('libspa-0.2', required : true, static : false, method : 'pkg-config')

moduledir = pw_dep.get_variable(pkgconfig: 'moduledir')
deps = [roc_dep, pw_dep, spa_dep]

common_src = files('src/module-roc/common.c')
sink_src = files('src/module-roc-sink.c')
source_src = files('src/module-roc-source.c')
sink_plugin = shared_library('pipewire-module-roc-sink',
  [sink_src, common_src], dependencies : deps,
  install : true,
  install_dir : moduledir
)
source_plugin = shared_library('pipewire-module-roc-source',
  [source_src, common_src], dependencies : deps,
  install : true,
  install_dir : moduledir
)

subdir('tests')
