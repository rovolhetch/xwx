project(
  'xwx',
  'c',
  meson_version: '>= 1.8.0',
  version: '0.1',
  default_options: [
    'c_std=c99',
    'warning_level=3',
  ],
)

cc = meson.get_compiler('c')
add_project_arguments(cc.get_supported_arguments([
  '-D_POSIX_C_SOURCE=200809L',
  '-Wno-unused-parameter',
]), language: 'c')

if get_option('debug')
  add_project_arguments('-DDEBUG', language: 'c')
endif

wayland_client = dependency('wayland-client')
xkbcommon = dependency('xkbcommon')

wl_mod = import('wayland')

tablet_v2 = wl_mod.find_protocol(
  'tablet',
  version: 2,
)

cursor_shape = wl_mod.find_protocol(
  'cursor-shape',
  state: 'staging',
  version: 1,
)

generated = wl_mod.scan_xml(
  'protocol/river-window-management-v1.xml',
  'protocol/river-xkb-bindings-v1.xml',
  'protocol/river-layer-shell-v1.xml',
  tablet_v2,
  cursor_shape,
)

exe = executable(
  'xwx',
  'main.c',
  generated,
  dependencies: [
    wayland_client,
    xkbcommon,
  ],
  install: false,
)
