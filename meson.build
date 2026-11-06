project('landell', 'c',
  version : '0.1.0',
  default_options : ['c_std=c11', 'warning_level=3', 'werror=true', 'cpp_std=c++17'])

subdir('src/DP32G030')

landell_exe = executable('landell',
  files('src/main.c', 'src/file.c'),
  dependencies : dp32g030_dep,
  install : true)

if get_option('gui').enabled()
  add_languages('cpp', native : false)
  subdir('gui')
endif

subdir('tests')
