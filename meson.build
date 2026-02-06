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
  
  scons_options = [
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
  build_result = run_command(
    'sh', '-c',
    'cd "' + roc_toolkit_dir + '" && ' +
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

common_src = files()
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

