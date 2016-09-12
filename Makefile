include $(RTE_SDK)/mk/rte.vars.mk

APP = tapbr

CFLAGS += -O0 -g
CFLAGS += $(WERROR_FLAGS)

SRCS-y := tapbr.c

DEPDIRS-y += lib drivers

include $(RTE_SDK)/mk/rte.app.mk
