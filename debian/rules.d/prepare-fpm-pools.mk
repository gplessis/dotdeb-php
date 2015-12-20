ifeq ($(PHP_MAJOR_VERSION),5)
# PHP 5.x version
# install the FPM configuration files to respective locations and customize them for Debian
prepare-fpm-pools:
	mkdir -p debian/$(PHP_FPM)/etc/php/$(PHP_NAME_VERSION)/fpm/
	sed -r '/('"'"'|\[)www('"'"'|\])/Q' \
	    < debian/tmp/etc/php-fpm.conf.default | \
	sed -e's,^pid[[:space:]]*=[[:space:]].*,pid = /run/php/$(PHP_FPM).pid,' \
	    -e's,^error_log[[:space:]]*=[[:space:]].*,error_log = /var/log/$(PHP_FPM).log,' \
	    -e's,^include[[:space:]]*=[[:space:]]*.*,include=/etc/php/$(PHP_NAME_VERSION)/fpm/pool.d/*.conf,' \
	    > debian/$(PHP_FPM)/etc/php/$(PHP_NAME_VERSION)/fpm/php-fpm.conf

	mkdir -p debian/$(PHP_FPM)/etc/php/$(PHP_NAME_VERSION)/fpm/pool.d/
	sed -nr '/('"'"'|\[)www('"'"'|\])/{h;p;d};x;/www/{x;p}' \
	    < debian/tmp/etc/php-fpm.conf.default | \
	sed -e's,^listen = .*,listen = /run/php/$(PHP_FPM).sock,' \
	    -e's{^;listen\.owner{listen.owner{;' \
            -e's{^;listen\.group{listen.group{;' \
	    > debian/$(PHP_FPM)/etc/php/$(PHP_NAME_VERSION)/fpm/pool.d/www.conf

	rm -f \
	    debian/tmp/etc/php-fpm.conf.default
else
# install the FPM configuration files to respective locations and customize them for Debian
prepare-fpm-pools:
	mkdir -p debian/$(PHP_FPM)/etc/php/$(PHP_NAME_VERSION)/fpm/
	sed -e's,^pid[[:space:]]*=[[:space:]].*,pid = /run/php/$(PHP_FPM).pid,' \
	    -e's,^error_log[[:space:]]*=[[:space:]].*,error_log = /var/log/$(PHP_FPM).log,' \
	    -e's,^include[[:space:]]*=[[:space:]]*.*,include=/etc/php/$(PHP_NAME_VERSION)/fpm/pool.d/*.conf,' \
	  < debian/tmp/etc/php-fpm.conf.default \
	  > debian/$(PHP_FPM)/etc/php/$(PHP_NAME_VERSION)/fpm/php-fpm.conf

	mkdir -p debian/$(PHP_FPM)/etc/php/$(PHP_NAME_VERSION)/fpm/pool.d/
	sed -e's,^listen = .*,listen = /run/php/$(PHP_FPM).sock,' \
	    -e's{^;listen\.owner{listen.owner{;' \
	    -e's{^;listen\.group{listen.group{;' \
	  < debian/tmp/etc/php-fpm.d/www.conf.default \
	  > debian/$(PHP_FPM)/etc/php/$(PHP_NAME_VERSION)/fpm/pool.d/www.conf

	rm -f  \
	  debian/tmp/etc/php-fpm.conf.default \
	  debian/tmp/etc/php-fpm.d/www.conf.default
endif
