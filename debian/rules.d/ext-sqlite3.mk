ext_PACKAGES        += sqlite3
sqlite3_DESCRIPTION := SQLite3
sqlite3_EXTENSIONS  := sqlite3 pdo_sqlite
sqlite3_config      := --with-sqlite3=shared,/usr
pdo_sqlite_config   := --with-pdo-sqlite=shared,/usr
export sqlite3_EXTENSIONS
export sqlite3_DESCRIPTION
