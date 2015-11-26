ext_PACKAGES   += gd
gd_DESCRIPTION := GD
gd_EXTENSIONS  := gd
gd_config      := \
	--with-gd=shared \
	--enable-gd-native-ttf \
	--with-jpeg-dir=shared,/usr \
	--with-xpm-dir=shared,/usr/X11R6 \
	--with-png-dir=shared,/usr \
	--with-freetype-dir=shared,/usr \
	--with-vpx-dir=shared,/usr
export gd_EXTENSIONS
export gd_DESCRIPTION
