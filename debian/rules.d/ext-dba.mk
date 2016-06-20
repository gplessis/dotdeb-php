ext_PACKAGES    += dba
dba_DESCRIPTION := DBA
dba_EXTENSIONS  := dba
dba_config = \
	--enable-dba=shared \
	--with-db4=/usr \
	--without-gdbm \
	--with-qdbm=/usr \
	--enable-inifile \
	--enable-flatfile
export dba_EXTENSIONS
export dba_DESCRIPTION
