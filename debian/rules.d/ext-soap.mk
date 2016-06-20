ext_PACKAGES    += soap
soap_DESCRIPTION := SOAP
soap_EXTENSIONS  := soap
soap_config = \
	--enable-soap=shared \
	--with-libxml-dir=/usr
export soap_EXTENSIONS
export soap_DESCRIPTION
