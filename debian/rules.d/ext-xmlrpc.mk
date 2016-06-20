ext_PACKAGES     += xmlrpc
xmlrpc_DESCRIPTION := XMLRPC-EPI
xmlrpc_EXTENSIONS  := xmlrpc
xmlrpc_config      := \
	--with-xmlrpc=shared,/usr \
	--with-libxml-dir=/usr
export xmlrpc_EXTENSIONS
export xmlrpc_DESCRIPTION
