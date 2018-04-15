pipewire_jack_sources = [
  'pipewire-jack.c',
]

pipewire_jack = shared_library('jack-pipewire',
    pipewire_jack_sources,
    include_directories : [configinc],
    dependencies : [pipewire_dep, jack_dep],
    install : false,
)
