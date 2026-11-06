dp32g030_inc = include_directories('.')

dp32g030_lib = static_library('dp32g030',
  files(
    'bus.c',
    'cpu.c',
    'decode.c',
    'exc.c',
    'exec.c',
    'exec_alu.c',
    'exec_branch.c',
    'exec_mem.c',
    'exec_misc.c',
    'mmio.c',
    'periph.c',
    'periph_ext.c',
    'aes.c',
    'iic.c',
    'pwm_plus.c',
    'rtc_wdt.c',
    'scs.c',
    'timer_plus.c',
    'timer_pwm.c',
    'timing.c'
  ),
  include_directories : dp32g030_inc)

dp32g030_dep = declare_dependency(
  link_with : dp32g030_lib,
  include_directories : dp32g030_inc)
