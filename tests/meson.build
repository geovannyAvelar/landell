python = find_program('python3', required : false)

if python.found() and run_command(python, '-c', 'import pytest', check : false).returncode() == 0
  test('python',
    python,
    args : ['-m', 'pytest', '-q', '-p', 'no:cacheprovider', meson.current_source_dir()],
    env : {
      'DP32G030_LIB' : dp32g030_lib.full_path(),
      'LANDELL_BIN' : landell_exe.full_path(),
      'PYTHONPATH' : join_paths(meson.project_source_root(), 'tools'),
    },
    depends : [dp32g030_lib, landell_exe],
    timeout : 120)
endif
