ext_PACKAGES       += sybase
sybase_DESCRIPTION := Sybase
sybase_EXTENSIONS  := pdo_dblib
pdo_dblib_config   := --with-pdo-dblib=shared,/usr
export sybase_EXTENSIONS
export sybase_DESCRIPTION
