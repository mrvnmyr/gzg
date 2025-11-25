project('gzg', 'c',
  version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=2'])

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required: true)

xcb_dep = dependency('xcb', required: true)
cairo_dep = dependency('cairo', required: true)
xcb_keysyms_dep = dependency('xcb-keysyms', required: true)
xcb_xtest_dep = dependency('xcb-xtest', required: true)
xcb_xkb_dep = dependency('xcb-xkb', required: true)

exe = executable('gzg', 'main.c',
  dependencies: [xcb_dep, cairo_dep, xcb_keysyms_dep, xcb_xtest_dep, xcb_xkb_dep, m_dep],
  install: false)

test('build-smoke', exe, is_parallel: true, args: ['--help'])
