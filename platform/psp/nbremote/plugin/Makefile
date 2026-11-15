# NB Remote's ARK VSH plugin (docs/PSP_PORT.md "NB Remote"), the pspsdk way for plugins (build_prx.mak), from the shared core
# in src/psp/nbremote. Run from an output folder (the top Makefile's psp-remote does it):
#   make -C build/psp/nbremote_plugin/launch -f <repo>/platform/psp/nbremote/plugin/Makefile VARIANT=launch  -> nbremote.prx
#   make -C build/psp/nbremote_plugin/net    -f <repo>/platform/psp/nbremote/plugin/Makefile VARIANT=net     -> nbremote_net.prx
# A user-mode module with no newlib: ARK's small libansic gives memcpy and friends; NB Remote's own printf does the rest.
HERE := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
SRC := $(abspath $(HERE)/../../../../src/psp/nbremote)
VPATH := $(SRC):$(HERE)
VARIANT ?= launch
BUILD_TAG ?= 0 unknown

CORE = nbr_plugin.o nbr_core.o nbr_text.o nbr_sha256.o nbr_http.o nbr_plat_psp.o nbr_launch_psp.o exports.o
ifeq ($(VARIANT),net)
TARGET = nbremote_net
OBJS = $(CORE) nbr_net_psp.o
NET_DEFS = -DNBR_NO_NET=0 -DNBR_PLUGIN_NET=1
NET_LIBS = -lpspnet -lpspnet_inet -lpspnet_apctl -lpspnet_resolver -lpsputility -lpspwlan
else
TARGET = nbremote
OBJS = $(CORE)
NET_DEFS = -DNBR_NO_NET=1 -DNBR_PLUGIN_NET=0
NET_LIBS =
endif

INCDIR = $(SRC)
CFLAGS = -std=gnu99 -O2 -G0 -Wall -Wextra -Wno-unused-parameter -fno-pic \
         -DNBR_CFW_IMPORTS=1 $(NET_DEFS) '-DNBR_BUILD="$(BUILD_TAG)"'
LDFLAGS =
PSP_FW_VERSION = 660
BUILD_PRX = 1
# everything by name, no newlib: ARK's libansic first (memcpy, strlen...), and never libpspkernel: its stubs are the *ForKernel
# libraries under the same names (sceIoOpen, memcpy...), which a user module must not import (tools/psp/prx_info.py --forbid
# checks); libpspuser has the user ones. libgcc for 64-bit divides. No --gc-sections: it drops the exports.
LIBS = -nostdlib -lansic $(NET_LIBS) -lpspctrl -lpsppower -lpsprtc -lpspsystemctrl_user -lpspmodinfo -lpspsdk -lpspuser -lansic -lgcc

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak
