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

executable('landell-gui', files('main.cpp'),
  dependencies : gui_deps,
  override_options : gui_opts)

pacer_lib = static_library('pacer', files('pacer.cpp'))
lcd_view_lib = static_library('lcd_view', files('lcd_view.cpp'),
  dependencies : gui_deps,
  override_options : gui_opts)

test('pacer',
  executable('test_pacer', files('test_pacer.cpp'),
    link_with : pacer_lib, override_options : gui_opts))
test('lcd_view',
  executable('test_lcd_view', files('test_lcd_view.cpp'),
    link_with : lcd_view_lib, dependencies : gui_deps,
    override_options : gui_opts))
