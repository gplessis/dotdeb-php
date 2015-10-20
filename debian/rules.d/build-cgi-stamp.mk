build-cgi-stamp: configure-cgi-stamp
	dh_auto_build --builddirectory cgi-build --parallel
	mv cgi-build/sapi/cgi/php-cgi cgi-build/sapi/cgi/cgi-bin.php$(PHP_NAME_VERSION)

	# Dirty hack to not rebuild everything twice
	cd cgi-build/main && \
		sed -i -e 's/FORCE_CGI_REDIRECT 1/FORCE_CGI_REDIRECT 0/' \
		       -e 's/DISCARD_PATH 0/DISCARD_PATH 1/' php_config.h && \
		sed -i -e 's/--enable-force-cgi-redirect/--enable-discard-path/' build-defs.h && \
		touch ../../ext/standard/info.c && \
		touch ../../sapi/cgi/cgi_main.c

	dh_auto_build --builddirectory cgi-build --parallel
	mv cgi-build/sapi/cgi/php-cgi cgi-build/sapi/cgi/usr.bin.$(PHP_CGI)

	touch build-cgi-stamp
