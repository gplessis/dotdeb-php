ext_PACKAGES      += odbc
odbc_DESCRIPTION := ODBC
odbc_EXTENSIONS  := odbc pdo_odbc
odbc_config      := --with-unixODBC=shared,/usr
pdo_odbc_config  := --with-pdo-odbc=shared,unixODBC,/usr
export odbc_EXTENSIONS
export odbc_DESCRIPTION
