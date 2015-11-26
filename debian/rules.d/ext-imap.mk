ext_PACKAGES     += imap
imap_DESCRIPTION := IMAP
imap_EXTENSIONS  := imap
imap_config      := --with-imap=shared,/usr \
	            --with-kerberos \
	            --with-imap-ssl=yes
export imap_EXTENSIONS
export imap_DESCRIPTION
