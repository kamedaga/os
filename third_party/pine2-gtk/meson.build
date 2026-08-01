project(
  'pine2-gtk',
  'c',
  version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=3'],
)

gtk = dependency('gtk+-3.0', version: '>=3.22')
curl = dependency('libcurl')
json_c = dependency('json-c', version: '>=0.13')
cmark_gfm = dependency('libcmark-gfm', static: true)
cmark_gfm_extensions = meson.get_compiler('c').find_library(
  'cmark-gfm-extensions',
  static: true,
)

executable(
  'pine2-gtk',
  [
    'src/main.c',
    'src/markdown-renderer.c',
    'src/pine-api.c',
  ],
  dependencies: [gtk, curl, json_c, cmark_gfm, cmark_gfm_extensions],
  # CapabilityOS keeps GTK and the common Alpine closure in /usr/lib while
  # the independently built curl closure lives in /opt/curl/lib.  Keep the
  # system directory first so curl's private copies cannot replace GTK's
  # dependencies, and make the application directly executable without an
  # LD_LIBRARY_PATH shell wrapper.
  build_rpath: '/usr/lib:/opt/curl/lib',
  install_rpath: '/usr/lib:/opt/curl/lib',
  install: true,
)

install_data(
  'data/app.pine2.Pine2.desktop',
  install_dir: get_option('datadir') / 'applications',
)
