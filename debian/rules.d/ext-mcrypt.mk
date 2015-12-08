ext_PACKAGES     += mcrypt
mcrypt_DESCRIPTION := libmcrypt
mcrypt_EXTENSIONS  := mcrypt
mcrypt_config      := --with-mcrypt=shared,/usr
export mcrypt_EXTENSIONS
export mcrypt_DESCRIPTION

