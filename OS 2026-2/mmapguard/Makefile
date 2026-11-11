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

.PHONY: all clean modules tools demo test informe informe-completo informes \
        entrega distclean help

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Herramientas de espacio de usuario (mg_encrypt, mg_test, mg_bench)
tools:
	$(MAKE) -C tools

# Datos de demostracion, en claro y cifrados; requiere las herramientas
demo: tools
	./demo/gen_demo.sh

# Banco de pruebas de concepto; requiere el modulo cargado.
# Pase TESTFLAGS=--load para que el propio script lo cargue y lo descargue.
test: tools
	./tests/run_pocs.sh $(TESTFLAGS)

# Informe en PDF. Necesita weasyprint, markdown y pygments; se instalan sin
# privilegios en un entorno virtual:
#   python3 -m venv ~/.venv-informe
#   ~/.venv-informe/bin/pip install weasyprint markdown pygments
#   make informe PYTHON=~/.venv-informe/bin/python INFORMEFLAGS='--autor "Nombre"'
#
# 'informe' produce la version de entrega, "reporte parcial 3 resumen.pdf"
# (20 paginas): toma los documentos condensados de docs/breve/ y omite los
# anexos. 'informe-completo' produce "reporte parcial 3 completo.pdf"
# (62 paginas) con los documentos integros de docs/ mas el README y el registro
# del banco como anexos.
PYTHON ?= python3
PDF_RESUMEN  ?= reporte parcial 3 resumen.pdf
PDF_COMPLETO ?= reporte parcial 3 completo.pdf

informe:
	$(PYTHON) docs/build_report.py --breve -o "$(PDF_RESUMEN)" $(INFORMEFLAGS)

informe-completo:
	$(PYTHON) docs/build_report.py -o "$(PDF_COMPLETO)" $(INFORMEFLAGS)

informes: informe informe-completo

# Paquete para el buzon de interactiva: fuentes, banco de pruebas, documentacion
# y los dos PDF. Omite los binarios, el .ko (su vermagic solo sirve en este
# kernel) y demo/grande.* (32 MB, se regenera con 'make demo'). Construya antes
# los informes con 'make informes PYTHON=...'.
ZIPNAME ?= Parcial 25% - Sistemas Operativos - Sebastian Martinez Maya.zip
entrega:
	@test -f "$(PDF_RESUMEN)" -a -f "$(PDF_COMPLETO)" || \
	  { echo "Faltan los PDF: corra antes 'make informes PYTHON=<venv>/bin/python'"; exit 1; }
	@rm -rf .paquete && mkdir -p .paquete/mmapguard/src .paquete/mmapguard/tools \
	  .paquete/mmapguard/tests .paquete/mmapguard/demo .paquete/mmapguard/docs/breve
	@cp Makefile README.md .gitignore .paquete/mmapguard/
	@cp "$(PDF_RESUMEN)" "$(PDF_COMPLETO)" .paquete/mmapguard/
	@test -f ../prompts.txt && cp ../prompts.txt .paquete/mmapguard/ || true
	@cp src/mmapguard.c src/mmapguard_uapi.h .paquete/mmapguard/src/
	@cp tools/Makefile tools/*.c .paquete/mmapguard/tools/
	@cp tests/run_pocs.sh tests/mg_poc.py .paquete/mmapguard/tests/
	@test -f tests/last-run.log && cp tests/last-run.log .paquete/mmapguard/tests/ || true
	@cp demo/gen_demo.sh demo/secreto.txt demo/secreto.enc .paquete/mmapguard/demo/
	@cp docs/*.md docs/arquitectura.svg docs/build_report.py .paquete/mmapguard/docs/
	@cp docs/breve/*.md .paquete/mmapguard/docs/breve/
	@rm -f "$(ZIPNAME)"
	@cd .paquete && zip -r -9 -X "../$(ZIPNAME)" mmapguard > /dev/null
	@rm -rf .paquete
	@echo "Paquete listo: $(ZIPNAME)"
	@unzip -l "$(ZIPNAME)" | tail -3

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
	@echo "  make test [TESTFLAGS=--load]         - ejecuta el banco de PoC (tests/)"
	@echo "  make informe PYTHON=<venv>/bin/python - 'reporte parcial 3 resumen.pdf', 20 pags."
	@echo "  make informe-completo PYTHON=...     - 'reporte parcial 3 completo.pdf', 62 pags."
	@echo "  make informes PYTHON=...             - ambos"
	@echo "  make entrega                         - .zip para el buzon (necesita los PDF)"
	@echo "  make clean                           - borra artefactos de compilacion"
	@echo "  make distclean                       - borra tambien los datos de demo"
