project('pengu-deck', 'c',
	version: '0.1.3',
	license: 'GPL-3.0-or-later',
	meson_version: '>= 0.62.0',
	default_options: [
		'c_std=gnu11',
		'warning_level=2',
		'buildtype=release',
		'b_ndebug=if-release',
	])

cc = meson.get_compiler('c')
add_project_arguments(
	cc.get_supported_arguments([
		'-Wno-unused-parameter',
		'-Wno-missing-field-initializers',
		'-Wshadow',
		'-fno-strict-aliasing',
	]),
	language: 'c')

m_dep = cc.find_library('m')
dl_dep = cc.find_library('dl', required: false)
threads_dep = dependency('threads')
glib_dep = dependency('glib-2.0', version: '>= 2.74')
gio_dep = dependency('gio-2.0', version: '>= 2.74')
gtk_dep = dependency('gtk4', version: '>= 4.10')
json_dep = dependency('json-glib-1.0', version: '>= 1.6')
curl_dep = dependency('libcurl', version: '>= 7.68')
avformat_dep = dependency('libavformat', version: '>= 59.27')
avcodec_dep = dependency('libavcodec', version: '>= 59.37')
avutil_dep = dependency('libavutil', version: '>= 57.28')
swr_dep = dependency('libswresample', version: '>= 4.7')
rubberband_dep = dependency('rubberband', version: '>= 3.0',
	required: get_option('keylock'))
alsa_dep = dependency('alsa', required: get_option('midi'))

conf = configuration_data()
conf.set_quoted('PD_VERSION', meson.project_version())
conf.set_quoted('PD_APP_ID', 'io.github.deimen24.PenguDeck')
conf.set('HAVE_RUBBERBAND', rubberband_dep.found())
conf.set('HAVE_ALSA', alsa_dep.found())
configure_file(output: 'pd-build.h', configuration: conf)
# the commit the binary was built from, so a stale install is obvious
vcs_h = vcs_tag(input: 'src/pd-vcs.h.in', output: 'pd-vcs.h',
	command: ['git', 'rev-parse', '--short', 'HEAD'], fallback: 'unknown')

miniaudio_inc = include_directories('third_party/miniaudio')

core_src = files(
	'src/analyze.c',
	'src/config.c',
	'src/cuestore.c',
	'src/decoder.c',
	'src/deck.c',
	'src/dsp.c',
	'src/encoder.c',
	'src/engine.c',
	'src/fx.c',
	'src/library.c',
	'src/media.c',
	'src/miniaudio_impl.c',
	'src/net.c',
	'src/playlists.c',
	'src/sampler.c',
	'src/scauth.c',
	'src/soundcloud.c',
	'src/track.c',
)

core_deps = [
	m_dep, dl_dep, threads_dep, glib_dep, gio_dep, json_dep, curl_dep,
	avformat_dep, avcodec_dep, avutil_dep, swr_dep, rubberband_dep,
]

core_lib = static_library('pdcore', [core_src, vcs_h],
	dependencies: core_deps,
	include_directories: miniaudio_inc,
	# miniaudio is third party code, keep its warnings out of our way
	c_args: cc.get_supported_arguments(['-Wno-sign-compare']))

core_dep = declare_dependency(
	link_with: core_lib,
	dependencies: core_deps,
	include_directories: [include_directories('src'), miniaudio_inc])

gnome = import('gnome')
resources = gnome.compile_resources('pd-resources',
	'data/pengu-deck.gresource.xml',
	source_dir: 'data',
	c_name: 'pd')

ui_src = files(
	'src/app.c',
	'src/automix.c',
	'src/main.c',
	'src/midi.c',
	'src/sccache.c',
	'src/ui/deckview.c',
	'src/ui/knob.c',
	'src/ui/libview.c',
	'src/ui/mediaview.c',
	'src/ui/meter.c',
	'src/ui/platter.c',
	'src/ui/mixerview.c',
	'src/ui/prefs.c',
	'src/ui/queueview.c',
	'src/ui/samplerview.c',
	'src/ui/sclogin.c',
	'src/ui/scview.c',
	'src/ui/waveform.c',
	'src/ui/window.c',
)

executable('pengu-deck', [ui_src, vcs_h], resources,
	dependencies: [core_dep, gtk_dep, alsa_dep],
	install: true)

subdir('data')

if get_option('tests')
	subdir('tests')
endif
