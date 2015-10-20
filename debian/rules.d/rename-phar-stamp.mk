rename-phar-stamp:
	mv -u debian/tmp/usr/bin/phar.phar debian/tmp/usr/bin/phar.phar$(PHP_NAME_VERSION)
	rm debian/tmp/usr/bin/phar
	ln -s phar.phar$(PHP_NAME_VERSION) debian/tmp/usr/bin/phar$(PHP_NAME_VERSION)
	mv -u debian/tmp/usr/share/man/man1/phar.phar.1 debian/tmp/usr/share/man/man1/phar.phar$(PHP_NAME_VERSION).1
	mv -u debian/tmp/usr/share/man/man1/phar.1 debian/tmp/usr/share/man/man1/phar$(PHP_NAME_VERSION).1
	touch rename-phar-stamp
