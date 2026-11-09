sitvfs_giomoduledir = gio_dep.get_variable('giomoduledir',
  pkgconfig_define: ['libdir', get_option('prefix') / get_option('libdir')],
  default_value: get_option('libdir') / 'gio' / 'modules')

shared_module('gio-sitvfs',
  files('sitvfs.c', '../gtk/sitring.c'),
  include_directories: [confinc, include_directories('..', '../gtk')],
  dependencies: [gio_dep, giounix_dep],
  install: true,
  install_dir: sitvfs_giomoduledir,
)
