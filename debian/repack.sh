#!/bin/sh

: <<=cut
=pod

=head1 NAME

repack.stub - script to repack upstream tarballs from uscan

=head1 INSTRUCTIONS

put this in debian/repack.stub and add "debian sh debian/repack.stub" to
the end of the line in debian/watch. you will also need to add a version
mangle to debian/watch.

then create a debian/repack.local. this is a shell script that is
sourced under "set -e", so be careful to check returns codes.

=head1 FUNCTIONS

=over 4

=item rm

rm is replaced by a function that does some magic ("rm -rv" by default), but also changes MANIFEST if $MANIFEST is 1

=item mv

mv is replaced by a function that just does mv (by default), but also changes MANIFEST if $MANIFEST is 1

=item requires_version

requires_version is there for future usage for requiring certain versions of the script

=back

=head1 VARIABLES

=over 4

=item SUFFIX

defaults to +dfsg

what to append to the upstream version

=item RM_OPTS

defaults to -vrf

options to pass to rm

=item MANIFEST

defaults to 0, set to 1 to turn on.

this will manipulate MANIFEST files in CPAN tarballs.

=item UP_BASE

this is the directory where the upstream source is.

=back

=head1 COPYRIGHT AND LICENSE

Copyright 2009, Ryan Niebur <ryan@debian.org>

This program is free software; you can redistribute it and/or modify it
under the same terms as Perl itself.

=cut

if [ -z "$REPACK_SH" ]; then
    if [ -x /usr/share/pkg-perl-tools/repack.sh ]; then
        REPACK_SH='/usr/share/pkg-perl-tools/repack.sh'
    elif [ -f ../../scripts/repack.sh ]; then
        REPACK_SH=../../scripts/repack.sh
    fi
    if [ -z "$REPACK_SH" ] && which repack.sh > /dev/null; then
        REPACK_SH=$(which repack.sh)
    fi
fi

if [ ! -f "$REPACK_SH" ]; then
    echo "Couldn't find a repack.sh. please put it in your PATH, put it at ../../scripts/repack.sh, or put it somewhere else and set the REPACK_SH variable"
    echo "You can get it from http://anonscm.debian.org/gitweb/?p=pkg-perl/packages/pkg-perl-tools.git;a=blob_plain;f=scripts/repack.sh;hb=HEAD"
    exit 1
fi

exec "$REPACK_SH" "$@"
