ext_PACKAGES     += common
common_DESCRIPTION := documentation, examples and common
common_EXTENSIONS  := calendar ctype exif fileinfo ftp gettext iconv pdo phar posix shmop sockets sysvmsg sysvsem sysvshm tokenizer
calendar_config = --enable-calendar=shared
ctype_config = --enable-ctype=shared
exif_config = --enable-exif=shared
fileinfo_config = --enable-fileinfo=shared
ftp_config = --enable-ftp=shared --with-openssl-dir=/usr
gettext_config = --with-gettext=shared,/usr
iconv_config = --with-iconv=shared
pdo_config = --enable-pdo=shared
pdo_PRIORITY := 10
phar_config = --enable-phar=shared
posix_config = --enable-posix=shared
shmop_config = --enable-shmop=shared
sockets_config = --enable-sockets=shared
sysvmsg_config = --enable-sysvmsg=shared
sysvsem_config = --enable-sysvsem=shared
sysvshm_config = --enable-sysvshm=shared
tokenizer_config = --enable-tokenizer=shared
export pdo_PRIORITY
export common_EXTENSIONS
export common_DESCRIPTION
