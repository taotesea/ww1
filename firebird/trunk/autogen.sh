#!/bin/sh
#
# Run this to generate all the initial makefiles, etc.
#
# $Id: autogen.sh,v 1.4 2002-09-11 11:30:43 eku Exp $

PKG_NAME=Firebird2
SRCDIR=`dirname $0`
DIE=0

VER=` autoconf --version | grep '^[Aa]utoconf'|sed 's/^[^0-9]*//'`
case "$VER" in
 0* | 1\.* | 2\.[0-9] | 2\.[0-9][a-z]* | \
 2\.[1-4][0-9] | 2\.5[0-2][a-z]* )
  echo
  echo "**Error**: You must have autoconf 2.53 or later installed."
  echo "Download the appropriate package for your distribution/OS,"
  echo "or get the source tarball at ftp://ftp.gnu.org/pub/gnu/autoconf/"
  DIE=1
  ;;
esac

# Put other tests for programs here!


# If anything failed, exit now.
if test "$DIE" -eq 1; then
  exit 1
fi

# Give a warning if no arguments to 'configure' have been supplied.
if test -z "$*"; then
  echo "**Warning**: I am going to run \`configure' with no arguments."
  echo "If you wish to pass any to it, please specify them on the"
  echo \`$0\'" command line."
  echo
fi

# Generate configure from configure.in
echo "Running autoconf ..."
autoconf || exit 1

# If NOCONFIGURE is set, skip the call to configure
if test x$NOCONFIGURE = x
then
  echo Running $SRCDIR/configure $conf_flags "$@" ...
  rm -f config.cache config.log
  $SRCDIR/configure $conf_flags "$@" \
  && echo Now type \`make\' to compile $PKG_NAME
else
  echo Skipping configure process.
fi

# EOF
