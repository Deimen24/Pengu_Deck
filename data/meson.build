app_id = 'io.github.deimen24.PenguDeck'

install_data(app_id + '.desktop',
	install_dir: get_option('datadir') / 'applications')
install_data(app_id + '.metainfo.xml',
	install_dir: get_option('datadir') / 'metainfo')
install_data(app_id + '.svg',
	install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps')
install_data(app_id + '-128.png',
	rename: app_id + '.png',
	install_dir: get_option('datadir') / 'icons/hicolor/128x128/apps')

desktop_utils = find_program('desktop-file-validate', required: false)
if desktop_utils.found()
	test('desktop-file', desktop_utils,
		args: [meson.current_source_dir() / app_id + '.desktop'])
endif
