AC_PREREQ([2.69])
AC_INIT([imperaterm], [1.0.0])
AC_CONFIG_SRCDIR([src/vt.c])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([m4])
AM_INIT_AUTOMAKE([foreign subdir-objects -Wall])
AM_SILENT_RULES([yes])

AC_PROG_CC
dnl IMPERATERM_CC_FLAG(flag, variable): the variable is the flag where the
dnl compiler takes it, and nothing where it does not.
AC_DEFUN([IMPERATERM_CC_FLAG], [
AC_MSG_CHECKING([whether $CC accepts $1])
imperaterm_save_CFLAGS="$CFLAGS"
CFLAGS="$CFLAGS $1"
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([], [])], [$2="$1"; AC_MSG_RESULT([yes])],
  [$2=""; AC_MSG_RESULT([no])])
CFLAGS="$imperaterm_save_CFLAGS"
AC_SUBST([$2])
])
dnl ISO C99: what the compiler needs to be told, if anything. And its warnings.
IMPERATERM_CC_FLAG([-std=c99], [C99_CFLAGS])
IMPERATERM_CC_FLAG([-Wall], [WARN_CFLAGS])

dnl CGTK with its OpenGL widget, and the fonts it already uses. The GL
dnl library itself is not linked: its functions are looked up as it runs.
PKG_CHECK_MODULES([CGTK], [gtkgl-1.4 >= 1.4.0 gtk+-1.4 >= 1.4.0 freetype2 fontconfig])

dnl CGTK gives a window the icon its program is installed with. Its
dnl version did not change when it learnt to, so the call is looked for.
imperaterm_save_LIBS="$LIBS"
LIBS="$CGTK_LIBS $LIBS"
AC_CHECK_FUNC([gtk_window_set_default_icon_name], [],
  [AC_MSG_ERROR([this CGTK is too old: it has no gtk_window_set_default_icon_name()])])
LIBS="$imperaterm_save_LIBS"

dnl Translations, from the system's gettext: the text domain is imperaterm,
dnl its catalogues in po.
AM_GNU_GETTEXT([external])
AM_GNU_GETTEXT_VERSION([0.19.6])
AM_GNU_GETTEXT_REQUIRE_VERSION([0.19.6])

AC_CONFIG_FILES([Makefile po/Makefile.in])
AC_OUTPUT
