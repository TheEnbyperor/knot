knot_modules_updateproxy_la_SOURCES = knot/modules/updateproxy/updateproxy.c

if STATIC_MODULE_updateproxy
libknotd_la_SOURCES += $(knot_modules_updateproxy_la_SOURCES)
endif

if SHARED_MODULE_updateproxy
knot_modules_updateproxy_la_LDFLAGS = $(KNOTD_MOD_LDFLAGS)
knot_modules_updateproxy_la_CPPFLAGS = $(KNOTD_MOD_CPPFLAGS)
knot_modules_updateproxy_la_LIBADD = $(libcontrib_LIBS)
pkglib_LTLIBRARIES += knot/modules/updateproxy.la
endif
