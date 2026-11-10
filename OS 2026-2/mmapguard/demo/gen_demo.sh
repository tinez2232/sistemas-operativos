#!/usr/bin/env bash
# Genera los datos de demostracion de mmapguard, en claro y cifrados.
# Los archivos son deterministas: el mismo comando produce siempre los mismos
# bytes, de modo que las cifras del informe son reproducibles.
#
#   secreto.txt     4.764 bytes  ->   2 paginas
#   grande.txt  16.777.285 bytes -> 4.097 paginas
#
# Uso:  ./gen_demo.sh [clave_hex_32]

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

KEY="${1:-000102030405060708090a0b0c0d0e0f}"
ENC="../tools/mg_encrypt"

if [ ! -x "$ENC" ]; then
    echo "Falta $ENC. Compile primero con: make -C ../tools" >&2
    exit 1
fi

echo "[*] Generando secreto.txt (2 paginas)..."
python3 -I -c '
import sys
w = sys.stdout.write
w("NOMINA CONFIDENCIAL - EAFIT 2026-2 - NO DIVULGAR\n")
w("Empleado,Cargo,Salario\n")
for i in range(1, 201):
    w("emp%d,analista,%d\n" % (i, 3001731 + (i - 1) * 1731))
' > secreto.txt

echo "[*] Generando grande.txt (4.097 paginas, 16 MB)..."
python3 -I -c '
import sys
w = sys.stdout.write
for i in range(241680):
    w("registro %08d: nomina confidencial EAFIT 2026-2, valor %d\n" % (i, i * 7919))
' > grande.txt

echo "[*] Cifrando con AES-128-CTR..."
"$ENC" secreto.txt secreto.enc "$KEY"
"$ENC" grande.txt  grande.enc  "$KEY"

echo
printf '%-14s %12s bytes  %6s paginas\n' \
    secreto.enc "$(stat -c%s secreto.enc)" "$(( ($(stat -c%s secreto.enc) + 4095) / 4096 ))"
printf '%-14s %12s bytes  %6s paginas\n' \
    grande.enc  "$(stat -c%s grande.enc)"  "$(( ($(stat -c%s grande.enc)  + 4095) / 4096 ))"
echo "[+] Listo."
