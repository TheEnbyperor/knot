knot_modules_queryignoretsig_la_SOURCES = knot/modules/queryignoretsig/queryignoretsig.c

if STATIC_MODULE_queryignoretsig
libknotd_la_SOURCES += $(knot_modules_queryignoretsig_la_SOURCES)
endif

if SHARED_MODULE_queryignoretsig
knot_modules_queryignoretsig_la_LDFLAGS = $(KNOTD_MOD_LDFLAGS)
knot_modules_queryignoretsig_la_CPPFLAGS = $(KNOTD_MOD_CPPFLAGS)
knot_modules_queryignoretsig_la_LIBADD = $(libcontrib_LIBS)
pkglib_LTLIBRARIES += knot/modules/queryignoretsig.la
endif
