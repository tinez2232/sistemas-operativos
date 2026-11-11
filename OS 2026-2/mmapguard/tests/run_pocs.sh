#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# run_pocs.sh - Orquestador del banco de pruebas de mmapguard.
#
# Parcial 25% - Seguridad de Archivos en Capa Kernel y mmap.
#
# Verifica el entorno, ejecuta cada prueba de concepto contra el modulo
# cargado y resume el resultado en una tabla con codigo de salida agregado,
# de modo que sirva tanto para la demostracion como para integracion continua.
#
# Las PoC en si viven en mg_poc.py (acceso crudo a mmap/mprotect/madvise por
# ctypes) y en ../tools/mg_test (cliente de referencia en C). Este script no
# implementa ataques: los ordena, los interpreta y correlaciona el resultado
# con el registro del kernel.
#
# Uso:
#   ./run_pocs.sh [opciones]
#
#   --file <ruta>    archivo cifrado a usar      (def. demo/secreto.enc)
#   --claro <ruta>   texto en claro de referencia (def. demo/secreto.txt)
#   --load           carga el modulo si no esta cargado (usa sudo) y lo
#                    descarga al terminar
#   --keep           con --load, no descargar al terminar
#   --no-dmesg       no intentar leer el registro del kernel
#   --quick          acorta la fase de martilleo de A5 (1s en vez de 2s)
#   -h, --help       esta ayuda
#
# Codigo de salida: 0 todo PASS - 1 algun FAIL - 2 algun INCONCLUSIVE -
#                   3 problema de entorno

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
PY="$HERE/mg_poc.py"
DEV=/dev/mmapguard
MODNAME=mmapguard

ENC="$ROOT/demo/secreto.enc"
CLARO="$ROOT/demo/secreto.txt"
DO_LOAD=0
DO_KEEP=0
DO_DMESG=1
A5_SECONDS=2
LOADED_BY_US=0

# Registro completo de la ejecucion, para no depender de capturas de pantalla.
# El script se reinvoca una vez con la salida duplicada a tests/last-run.log.
LOG="$HERE/last-run.log"
if [ -z "${MG_TEE:-}" ]; then
    export MG_TEE=1
    [ -t 1 ] && export MG_COLOR=1
    "$0" "$@" 2>&1 | tee "$LOG"
    rc=${PIPESTATUS[0]}
    echo
    echo "[*] Registro completo en: $LOG"
    exit "$rc"
fi

if { [ -t 1 ] || [ "${MG_COLOR:-}" = "1" ]; } && [ -z "${NO_COLOR:-}" ]; then
    B=$'\033[1m'; R=$'\033[0m'; GREEN=$'\033[0;32m'; RED=$'\033[0;31m'
    YELL=$'\033[1;33m'; CYAN=$'\033[0;36m'
else
    B=""; R=""; GREEN=""; RED=""; YELL=""; CYAN=""
fi

usage() { sed -n '3,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
    case "$1" in
        --file)     ENC="$2"; shift 2 ;;
        --claro)    CLARO="$2"; shift 2 ;;
        --load)     DO_LOAD=1; shift ;;
        --keep)     DO_KEEP=1; shift ;;
        --no-dmesg) DO_DMESG=0; shift ;;
        --quick)    A5_SECONDS=1; shift ;;
        -h|--help)  usage; exit 0 ;;
        *) echo "Opcion desconocida: $1" >&2; usage >&2; exit 3 ;;
    esac
done

# --------------------------------------------------------------------------
# Registro de resultados
# --------------------------------------------------------------------------

LABELS=(); CODES=()
FAILS=0; ENVS=0; INCONCS=0

verdict_of() {
    case "$1" in
        0) echo "${GREEN}PASS${R}" ;;
        1) echo "${RED}FAIL${R}" ;;
        2) echo "${YELL}INCONCLUSIVE${R}" ;;
        *) echo "${YELL}ENV${R}" ;;
    esac
}

record() {
    LABELS+=("$1"); CODES+=("$2")
    case "$2" in
        0) ;;
        1) FAILS=$((FAILS + 1)) ;;
        2) INCONCS=$((INCONCS + 1)) ;;
        *) ENVS=$((ENVS + 1)) ;;
    esac
}

run_poc() {   # run_poc <etiqueta> <comando...>
    local label="$1"; shift
    local rc=0
    "$@" || rc=$?
    record "$label" "$rc"
    echo
}

skip_poc() { # skip_poc <etiqueta> <motivo>
    echo "${CYAN}--- $1${R}"
    echo "    ${YELL}-> INCONCLUSIVE: $2${R}"
    record "$1" 2
    echo
}

# --------------------------------------------------------------------------
# Limpieza
# --------------------------------------------------------------------------

A0DIR=""

cleanup() {
    [ -n "$A0DIR" ] && [ -d "$A0DIR" ] && rm -rf "$A0DIR"
    if [ "$LOADED_BY_US" = 1 ] && [ "$DO_KEEP" = 0 ]; then
        echo "${CYAN}[*] Descargando $MODNAME (lo cargo este script)${R}"
        sudo rmmod "$MODNAME" 2>/dev/null \
            || echo "    no se pudo descargar; hagalo a mano: sudo rmmod $MODNAME" >&2
    fi
}
trap cleanup EXIT

# --------------------------------------------------------------------------
# Verificacion del entorno
# --------------------------------------------------------------------------

echo "${B}${CYAN}=== mmapguard: banco de pruebas de concepto ===${R}"
echo "    kernel        : $(uname -r)"
echo "    usuario       : uid=$(id -u) ($(id -un))"
echo "    archivo cifrado: $ENC"
echo "    texto en claro : $CLARO"
echo

fatal_env() { echo "${RED}[!] $1${R}" >&2; exit 3; }

command -v python3 >/dev/null 2>&1 || fatal_env "falta python3"

for f in "$ENC" "$CLARO"; do
    [ -f "$f" ] || fatal_env "falta '$f'. Genere los datos con: make -C '$ROOT' demo"
done
[ -x "$ROOT/tools/mg_test" ] \
    || echo "${YELL}[*] tools/mg_test no esta compilado (make -C '$ROOT' tools)${R}"

# Carga del modulo, si se pidio
if ! lsmod | grep -q "^${MODNAME}\b"; then
    if [ "$DO_LOAD" = 1 ]; then
        [ -f "$ROOT/$MODNAME.ko" ] || fatal_env \
            "falta $ROOT/$MODNAME.ko. Compile con: make -C '$ROOT' KDIR=... CC=gcc-13 modules"
        echo "${CYAN}[*] Cargando $MODNAME.ko con authorized_uid=$(id -u) (requiere sudo)${R}"
        sudo insmod "$ROOT/$MODNAME.ko" "authorized_uid=$(id -u)" \
            || fatal_env "insmod fallo; revise dmesg"
        LOADED_BY_US=1
        sudo chmod 666 "$DEV" || fatal_env "no se pudo ajustar los permisos de $DEV"
        echo
    else
        fatal_env "el modulo $MODNAME no esta cargado. Use --load, o cargue a mano:
    sudo insmod '$ROOT/$MODNAME.ko' authorized_uid=\$(id -u)
    sudo chmod 666 $DEV"
    fi
fi

[ -e "$DEV" ] || fatal_env "$DEV no existe aunque el modulo esta cargado"
[ -r "$DEV" ] || fatal_env "sin permiso de lectura sobre $DEV (sudo chmod 666 $DEV)"

# El UID autorizado es legible (module_param con permiso 0444); comprobarlo
# aqui evita confundir un EPERM de politica con un error de configuracion.
AUTH_UID=""
if [ -r "/sys/module/$MODNAME/parameters/authorized_uid" ]; then
    AUTH_UID="$(cat "/sys/module/$MODNAME/parameters/authorized_uid")"
    echo "    authorized_uid del modulo: $AUTH_UID"
    if [ "$AUTH_UID" != "$(id -u)" ] && [ "$(id -u)" != 0 ]; then
        echo "${YELL}[*] El modulo autoriza al UID $AUTH_UID pero usted es $(id -u):${R}"
        echo "${YELL}    las PoC que exigen autorizacion daran EPERM.${R}"
    fi
fi
# La clave NO debe ser legible desde sysfs (module_param con permiso 0).
if [ -e "/sys/module/$MODNAME/parameters/key_hex" ]; then
    echo "${RED}[!] La clave esta expuesta en sysfs${R}"
else
    echo "    clave en sysfs: no expuesta (correcto)"
fi
echo

DMESG_MARK=""
if [ "$DO_DMESG" = 1 ]; then
    DMESG_MARK="$(dmesg 2>/dev/null | tail -1)"
fi

# --------------------------------------------------------------------------
# Ejecucion
# --------------------------------------------------------------------------

# A0 - requiere cambiar de UID, lo que exige privilegios: se omite si no hay
# sudo no interactivo disponible.
# Bajar de privilegios exige sudo. Se prefiere la via no interactiva; si no
# hay credencial en cache pero si una terminal, se deja que sudo pregunte.
SUDO=""
if command -v setpriv >/dev/null 2>&1; then
    if sudo -n true 2>/dev/null; then
        SUDO="sudo -n"
    elif [ -t 0 ]; then
        SUDO="sudo"
    fi
fi

if [ -z "$SUDO" ]; then
    skip_poc "A0  UID no autorizado" \
        "requiere setpriv y sudo para ejecutar bajo otro UID"
elif [ "$(stat -c '%a' "$DEV")" != "666" ]; then
    skip_poc "A0  UID no autorizado" \
        "el UID de prueba necesita que $DEV sea 666 (ahora es $(stat -c '%a' "$DEV"))"
else
    # El UID de prueba no puede alcanzar el repositorio si el directorio
    # personal es 750 (lo habitual): falla al abrir el propio mg_poc.py, antes
    # de ejercitar nada. Se prepara una copia en una ruta atravesable por
    # cualquiera, en vez de relajar los permisos del directorio personal.
    if A0DIR="$(mktemp -d "${TMPDIR:-/tmp}/mg-a0.XXXXXX")" \
       && chmod 755 "$A0DIR" \
       && cp "$PY" "$ENC" "$A0DIR/" \
       && chmod 644 "$A0DIR/$(basename "$PY")" "$A0DIR/$(basename "$ENC")"; then
        [ "$SUDO" = "sudo" ] && echo "${CYAN}[*] A0 baja de privilegios con sudo setpriv${R}"
        run_poc "A0  UID no autorizado" \
            $SUDO setpriv --reuid=65534 --regid=65534 --clear-groups \
            python3 "$A0DIR/$(basename "$PY")" unauth "$A0DIR/$(basename "$ENC")"
    else
        skip_poc "A0  UID no autorizado" \
            "no se pudo preparar una copia accesible para el UID de prueba"
    fi
fi

run_poc "A1  Cifrado en reposo"        python3 "$PY" a1   "$ENC" "$CLARO"
run_poc "OK  Descifrado correcto"      python3 "$PY" read "$ENC" "$CLARO"
run_poc "A2  mmap PROT_WRITE/SHARED"   python3 "$PY" a2   "$ENC" "$CLARO"
run_poc "A3  Escalada con mprotect"    python3 "$PY" a3   "$ENC" "$CLARO"
run_poc "A4  Evasion de O_RDONLY"      python3 "$PY" a4   "$ENC" "$CLARO"
export MG_A5_SECONDS="$A5_SECONDS"
run_poc "A5  Carrera con madvise"      python3 "$PY" a5   "$ENC" "$CLARO"

# Cliente de referencia en C: una sola sesion con las tres PoC originales,
# para reproducir los contadores publicados en el informe.
if [ -x "$ROOT/tools/mg_test" ]; then
    echo "${CYAN}--- REF  Cliente de referencia en C (tools/mg_test)${R}"
    rc=0
    (cd "$(dirname "$ENC")" && "$ROOT/tools/mg_test" "$(basename "$ENC")") || rc=$?
    record "REF Cliente en C" "$rc"
    echo
else
    skip_poc "REF Cliente en C" "tools/mg_test no esta compilado"
fi

# --------------------------------------------------------------------------
# Registro del kernel
# --------------------------------------------------------------------------

if [ "$DO_DMESG" = 1 ]; then
    echo "${B}${CYAN}--- Registro del kernel (lo que el modulo audito) ---${R}"
    if LOG="$(dmesg 2>/dev/null | grep "$MODNAME" | tail -20)" && [ -n "$LOG" ]; then
        echo "$LOG" | sed 's/^/    /'
    else
        echo "    no se pudo leer dmesg sin privilegios; pruebe: sudo dmesg | grep $MODNAME"
    fi
    echo
fi

# --------------------------------------------------------------------------
# Resumen
# --------------------------------------------------------------------------

echo "${B}${CYAN}=== Resumen ===${R}"
printf '    %-28s %s\n' "PRUEBA" "RESULTADO"
for i in "${!LABELS[@]}"; do
    printf '    %-28s %s\n' "${LABELS[$i]}" "$(verdict_of "${CODES[$i]}")"
done
echo
TOTAL=${#LABELS[@]}
PASSES=$((TOTAL - FAILS - ENVS - INCONCS))
echo "    $PASSES/$TOTAL PASS, $FAILS FAIL, $INCONCS INCONCLUSIVE, $ENVS ENV"

if [ "$FAILS" -gt 0 ]; then
    echo "${RED}[!] Hay fallos de seguridad: la defensa no se comporto como se espera.${R}"
    exit 1
fi
if [ "$ENVS" -gt 0 ]; then
    echo "${YELL}[*] Entorno incompleto: alguna prueba no pudo ejecutarse.${R}"
    exit 3
fi
if [ "$INCONCS" -gt 0 ]; then
    echo "${YELL}[*] Todas las pruebas concluyentes pasaron; alguna quedo sin concluir.${R}"
    exit 2
fi
echo "${GREEN}[+] Todas las pruebas pasaron.${R}"
exit 0
