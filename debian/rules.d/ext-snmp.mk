ext_PACKAGES     += snmp
snmp_DESCRIPTION := SNMP
snmp_EXTENSIONS  := snmp
snmp_config      := --with-snmp=shared,/usr
export snmp_EXTENSIONS
export snmp_DESCRIPTION
