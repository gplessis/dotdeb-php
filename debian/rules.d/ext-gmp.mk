ext_PACKAGES     += gmp
gmp_DESCRIPTION := GMP
gmp_EXTENSIONS  := gmp
gmp_config      := --with-gmp=shared,/usr
export gmp_EXTENSIONS
export gmp_DESCRIPTION
