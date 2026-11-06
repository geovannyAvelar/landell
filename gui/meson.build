cxx = meson.get_compiler('cpp')

sdl2 = dependency('sdl2')

skia_dir = get_option('skia_dir')
if skia_dir == ''
  error('-Dgui=enabled needs -Dskia_dir=PATH (see scripts/build-skia.sh)')
endif

skia_lib = cxx.find_library('skia', dirs : [skia_dir / 'out/Static'])
skia_dep = declare_dependency(
  dependencies : [skia_lib, cxx.find_library('pthread'), cxx.find_library('dl')],
  include_directories : include_directories(skia_dir, is_system : true))

gui_deps = [dp32g030_dep, sdl2, skia_dep]
gui_opts = ['werror=false']


pacer_lib = static_library('pacer', files('pacer.cpp'))
lcd_view_lib = static_library('lcd_view', files('lcd_view.cpp'),
  dependencies : gui_deps,
  override_options : gui_opts)

keypad_lib = static_library('keypad', files('keypad.cpp'),
  dependencies : gui_deps,
  override_options : gui_opts)

test('keypad',
  executable('test_keypad', files('test_keypad.cpp'),
    link_with : keypad_lib, dependencies : gui_deps,
    override_options : gui_opts))
panel_lib = static_library('panel', files('panel.cpp', 'glyphs.cpp'),
  link_with : keypad_lib,
  dependencies : gui_deps,
  override_options : gui_opts)

test('panel',
  executable('test_panel', files('test_panel.cpp'),
    link_with : [panel_lib, keypad_lib], dependencies : gui_deps,
    override_options : gui_opts))
test('pacer',
  executable('test_pacer', files('test_pacer.cpp'),
    link_with : pacer_lib, override_options : gui_opts))
test('lcd_view',
  executable('test_lcd_view', files('test_lcd_view.cpp'),
    link_with : lcd_view_lib, dependencies : gui_deps,
    override_options : gui_opts))

gui_exe = executable('landell-gui', files('main.cpp', '../src/file.c'),
  include_directories : include_directories('../src'),
  link_with : [lcd_view_lib, pacer_lib, panel_lib, keypad_lib],
  dependencies : gui_deps,
  install : true,
  override_options : gui_opts)

python = find_program('python3', required : false)
if python.found()
  test('screenshot', python,
    args : files('test_screenshot.py'),
    env : {'LANDELL_GUI' : gui_exe.full_path()},
    depends : gui_exe,
    timeout : 60)
endif
