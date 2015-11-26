ifneq ($(DEB_HOST_ARCH),$(filter $(DEB_HOST_ARCH),hurd-i386 m68k hppa ppc64))
  PACKAGES += interbase
  interbase_DESCRIPTION := Interbase
  interbase_EXTENSIONS  := interbase pdo_firebird
  interbase_config      := --with-interbase=shared,/usr
  pdo_firebird_config   := --with-pdo-firebird=shared,/usr
  export interbase_EXTENSIONS
  export interbase_DESCRIPTION
endif
