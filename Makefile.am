ACLOCAL_AMFLAGS = -I m4
SUBDIRS = po

AM_CPPFLAGS = $(CGTK_CFLAGS) -I$(srcdir)/icons -DLOCALEDIR=\"$(localedir)\" -DPKGDATADIR=\"$(pkgdatadir)\"
AM_CFLAGS = $(C99_CFLAGS) $(WARN_CFLAGS)

bin_PROGRAMS = imperaterm
imperaterm_SOURCES = src/imperaterm.c src/imperaterm.h src/join.c src/term.c src/render.c src/render.h src/vt.c src/vt.h
imperaterm_LDADD = $(CGTK_LIBS) $(LIBINTL)

dist_man1_MANS = imperaterm.1

# What has a shell tell the terminal who is at it and where. Imperaterm has
# the shell it starts read the first; login shells read the second, if
# sysconfdir is where the system keeps its own.
dist_pkgdata_DATA = data/imperaterm.sh
profiledir = $(sysconfdir)/profile.d
profile_DATA = data/imperaterm.sh
desktopdir = $(datadir)/applications
dist_desktop_DATA = imperaterm.desktop

# The icon: the one drawing, the sizes of it rendered beforehand by
# icons/render.sh, and an XPM, which is in the program too, for whatever
# still looks in pixmaps.
iconsizes = 16 22 24 32 48 64 128 256
hicolordir = $(datadir)/icons/hicolor
scalabledir = $(hicolordir)/scalable/apps
dist_scalable_DATA = icons/imperaterm.svg
pixmapsdir = $(datadir)/pixmaps
dist_pixmaps_DATA = icons/imperaterm.xpm

install-data-local:
	for s in $(iconsizes); do \
		$(MKDIR_P) "$(DESTDIR)$(hicolordir)/$${s}x$${s}/apps" && \
		$(INSTALL_DATA) "$(srcdir)/icons/imperaterm-$$s.png" \
			"$(DESTDIR)$(hicolordir)/$${s}x$${s}/apps/imperaterm.png" || exit 1; \
	done

uninstall-local:
	for s in $(iconsizes); do \
		rm -f "$(DESTDIR)$(hicolordir)/$${s}x$${s}/apps/imperaterm.png"; \
	done

# The terminal without a window: make check runs it.
check_PROGRAMS = tests/vttest tests/bench
tests_vttest_SOURCES = tests/vttest.c src/vt.c src/vt.h
tests_bench_SOURCES = tests/bench.c src/vt.c src/vt.h
TESTS = tests/vttest

EXTRA_DIST = LICENSE README icons/render.sh \
	icons/imperaterm-16.png icons/imperaterm-22.png icons/imperaterm-24.png icons/imperaterm-32.png \
	icons/imperaterm-48.png icons/imperaterm-64.png icons/imperaterm-128.png icons/imperaterm-256.png
