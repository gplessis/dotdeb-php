ext_PACKAGES      += opcache
opcache_DESCRIPTION := Zend OpCache
opcache_EXTENSIONS  := opcache
opcache_PRIORITY    := 10
opcache_EXTENSION   := zend_extension
opcache_config      := --enable-opcache --enable-opcache-file --enable-huge-code-pages
export opcache_EXTENSIONS
export opcache_DESCRIPTION
export opcache_PRIORITY
export opcache_EXTENSION
