include $(RTE_SDK)/mk/rte.vars.mk

APP = tapbr

CFLAGS += -O0 -g
CFLAGS += $(WERROR_FLAGS) -Werror -Wno-int-to-pointer-cast
CFLAGS += $(shell pkg-config --cflags libsystemd)

LDLIBS += $(shell pkg-config --libs libsystemd)

SRCS-y := tapbr.c dbus.c

DEPDIRS-y += lib drivers

include $(RTE_SDK)/mk/rte.app.mk
