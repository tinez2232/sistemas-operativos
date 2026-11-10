# Makefile de mmapguard - plano de datos (modulo de kernel)
#
# Uso:
#   make KDIR=/ruta/al/arbol/del/kernel
# Si no se indica KDIR se usa el arbol estandar de la distribucion.

obj-m += mmapguard.o
mmapguard-objs := src/mmapguard.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

ccflags-y := -I$(src)/src -Wall -Wextra

.PHONY: all clean modules tools demo distclean help

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Herramientas de espacio de usuario (mg_encrypt, mg_test, mg_bench)
tools:
	$(MAKE) -C tools

# Datos de demostracion, en claro y cifrados; requiere las herramientas
demo: tools
	./demo/gen_demo.sh

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(MAKE) -C tools clean

distclean: clean
	rm -f demo/*.txt demo/*.enc

help:
	@echo "Objetivos disponibles:"
	@echo "  make modules KDIR=<arbol> CC=gcc-13  - compila mmapguard.ko"
	@echo "  make tools                           - compila mg_encrypt, mg_test, mg_bench"
	@echo "  make demo                            - genera y cifra los datos de prueba"
	@echo "  make clean                           - borra artefactos de compilacion"
	@echo "  make distclean                       - borra tambien los datos de demo"
