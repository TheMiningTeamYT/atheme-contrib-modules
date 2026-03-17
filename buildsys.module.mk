# SPDX-License-Identifier: ISC
# SPDX-URL: https://spdx.org/licenses/ISC.html
#
# Copyright (C) 2010-2012 William Pitcock <nenolod@dereferenced.org>
# Copyright (C) 2010-2013 Jilles Tjoelker <jilles@stack.nl>
# Copyright (C) 2010 Nathan Phillip Brink <binki@gentoo.org>
# Copyright (C) 2011 Stephen Bennett <spb@exherbo.org>
# Copyright (C) 2020 Aaron M. D. Jones <me@aaronmdjones.net>
#
# Additional extensions for building single-file modules.

.SUFFIXES: ${PLUGIN_SUFFIX}

plugindir = ${MODDIR}/modules/${MODULE}
PLUGIN=$(addsuffix ${PLUGIN_SUFFIX},$(basename ${SRCS}))

all: ${PLUGIN}
install: ${PLUGIN}

phase_cmd_cc_module = CompileModule
quiet_cmd_cc_module = $@
      cmd_cc_module = ${CC} ${DEPFLAGS} ${CFLAGS} ${PLUGIN_CFLAGS} ${CPPFLAGS} ${PLUGIN_LDFLAGS} ${LDFLAGS} -o $@ $< ${LIBS}

phase_cmd_cxx_module = CompileModule
quiet_cmd_cxx_module = $@
      cmd_cxx_module = ${CXX} ${DEPFLAGS} ${CFLAGS} ${PLUGIN_CFLAGS} ${CXXFLAGS} ${CPPFLAGS} ${PLUGIN_LDFLAGS} ${LDFLAGS} -o $@ $< ${LIBS}

.c${PLUGIN_SUFFIX}:
	$(call echo-cmd,cmd_cc_module)
	$(cmd_cc_module)

.cxx${PLUGIN_SUFFIX} .cpp${PLUGIN_SUFFIX}:
	$(call echo-cmd,cmd_cxx_module)
	$(cmd_cxx_module)