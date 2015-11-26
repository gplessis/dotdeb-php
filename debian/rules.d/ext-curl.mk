ext_PACKAGES     += curl
curl_DESCRIPTION := CURL
curl_EXTENSIONS  := curl
curl_config      := --with-curl=shared,/usr
export curl_EXTENSIONS
export curl_DESCRIPTION
