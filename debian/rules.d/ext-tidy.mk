ext_PACKAGES     += tidy
tidy_DESCRIPTION := tidy
tidy_EXTENSIONS  := tidy
tidy_config      := --with-tidy=shared,/usr
export tidy_EXTENSIONS
export tidy_DESCRIPTION
