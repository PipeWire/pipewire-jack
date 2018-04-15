project('pipewire_jack', 'c',
  version : '0.1.9',
  meson_version : '>= 0.36.0',
  default_options : [ 'warning_level=1',
                      'c_std=gnu99',
                      'buildtype=debugoptimized' ])

configinc = include_directories('.')

cc = meson.get_compiler('c')

# Find dependencies
pipewire_dep = dependency('libpipewire-0.1')
jack_dep = dependency('jack', version : '>= 1.9.10', required : false)

subdir('src')
