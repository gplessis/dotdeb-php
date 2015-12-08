ext_PACKAGES       += readline
readline_DESCRIPTION := readline
readline_EXTENSIONS  := readline
readline_config      := --with-libedit=shared,/usr
export readline_EXTENSIONS
export readline_DESCRIPTION
