if is_uwp
  dsp56300_emu_ffi = declare_dependency()
  dsp_sources = files(['dsp.c', 'dsp_c.c', 'dsp_dma.c'])
else
  dsp56300_emu_ffi = dependency('dsp56300-emu-ffi', fallback: ['dsp56300', 'dsp56300_dep'])
  dsp_sources = files(['dsp.c', 'dsp_c.c', 'dsp_jit.c', 'dsp_dma.c'])
endif

subdir('interp')

libdsp = static_library(
  'dsp',
  dsp_sources + genh,
  dependencies: [dsp56300_emu_ffi],
)

dsp = declare_dependency(
  objects: [
    libdsp.extract_all_objects(recursive: false),
    libdsp_cpu_interp.extract_all_objects(recursive: false),
  ],
  dependencies: [dsp56300_emu_ffi],
)

mcpx_ss.add(dsp, files('gp_ep.c'))
