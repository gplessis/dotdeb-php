ext_PACKAGES    += mbstring
mbstring_DESCRIPTION := MBSTRING
mbstring_EXTENSIONS  := mbstring
mbstring_config = \
	--enable-mbstring=shared \
	--enable-mbregex \
	--enable-mbregex-backtrack
export mbstring_EXTENSIONS
export mbstring_DESCRIPTION
