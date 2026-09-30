#!/bin/sh
# ======================================================================================
# Bateria de pruebas del compresor Huffman concurrente
# ======================================================================================
# Verifica que todo archivo comprimido y luego descomprimido sea IDENTICO al original
# (comparando md5), y mide como escala el tiempo con distinto numero de hilos.
#
# Uso:  ./pruebas_huffman.sh
#
# Detalle importante: el comando 'exit' del shell espera a que terminen las tareas en
# segundo plano antes de cerrarse, asi que basta con enviar "Comprimir ... \n exit" y
# cuando el shell retorna la tarea ya termino. No hacen falta esperas artificiales.
# ======================================================================================

set -u

SHELL_BIN=./sys_shell
DIR=pruebas_tmp
FALLOS=0

if [ ! -x "$SHELL_BIN" ]; then
    echo "No existe $SHELL_BIN. Ejecuta 'make' primero."
    exit 1
fi

rm -rf "$DIR"
mkdir -p "$DIR"

# --------------------------------------------------------------------------------------
# Generacion de los archivos de prueba, incluidos los casos limite
# --------------------------------------------------------------------------------------
: > "$DIR/vacio.txt"                                    # 0 bytes
printf 'A' > "$DIR/un_byte.txt"                         # 1 byte
awk 'BEGIN{for(i=0;i<100000;i++)printf "x"}' > "$DIR/un_simbolo.txt"   # un solo simbolo
awk 'BEGIN{srand(7);s="abcdefgh sistemas operativos concurrencia huffman ";
     for(i=0;i<60000;i++)printf "%s", s}' > "$DIR/texto.txt"           # texto repetitivo
head -c 2000000 /dev/urandom > "$DIR/aleatorio.bin"     # incompresible
awk 'BEGIN{for(i=0;i<4096;i++)for(j=0;j<256;j++)printf "%c", j}' > "$DIR/todos_simbolos.bin"

echo "======================================================================"
echo " PRUEBA 1: integridad (original vs restaurado, comparando md5)"
echo "======================================================================"

for f in vacio.txt un_byte.txt un_simbolo.txt texto.txt aleatorio.bin todos_simbolos.bin; do
    for nt in 1 4 8; do
        origen="$DIR/$f"
        copia="$DIR/copia_${nt}_$f"

        # Se trabaja sobre una copia para que la descompresion no sobrescriba el original
        cp "$origen" "$copia"
        printf 'Comprimir %s %s\nexit\n' "$copia" "$nt" | "$SHELL_BIN" > /dev/null 2>&1
        rm -f "$copia"
        printf 'Descomprimir %s.huff %s\nexit\n' "$copia" "$nt" | "$SHELL_BIN" > /dev/null 2>&1

        if [ ! -f "$copia" ]; then
            printf '  FALLO  %-20s hilos=%s  no se restauro el archivo\n' "$f" "$nt"
            FALLOS=$((FALLOS + 1))
            continue
        fi

        md5_a=$(md5sum < "$origen" | cut -d' ' -f1)
        md5_b=$(md5sum < "$copia"  | cut -d' ' -f1)
        tam_orig=$(wc -c < "$origen")
        tam_huff=$(wc -c < "$copia.huff")

        if [ "$md5_a" = "$md5_b" ]; then
            printf '  OK     %-20s hilos=%s  %8s -> %8s bytes\n' "$f" "$nt" "$tam_orig" "$tam_huff"
        else
            printf '  FALLO  %-20s hilos=%s  md5 distinto (%s vs %s)\n' "$f" "$nt" "$md5_a" "$md5_b"
            FALLOS=$((FALLOS + 1))
        fi
    done
done

echo ""
echo "======================================================================"
echo " PRUEBA 2: escalabilidad con el numero de hilos"
echo "======================================================================"
echo " Nucleos disponibles: $(nproc 2>/dev/null || echo '?')"

# Archivo mas grande para que los tiempos sean medibles
cat "$DIR/texto.txt" "$DIR/texto.txt" "$DIR/texto.txt" "$DIR/texto.txt" > "$DIR/grande.txt"
cat "$DIR/grande.txt" "$DIR/grande.txt" > "$DIR/escala.txt"
rm -f "$DIR/grande.txt"
echo " Archivo de prueba: $(wc -c < "$DIR/escala.txt") bytes"

for nt in 1 2 4 8; do
    rm -f "$DIR/escala.txt.huff"
    salida=$(printf 'Comprimir %s %s\nexit\n' "$DIR/escala.txt" "$nt" | "$SHELL_BIN" 2>&1 \
             | sed 's/\x1b\[[0-9;]*m//g' | grep -o '[0-9]*\.[0-9]* s' | head -1)
    printf '  %2s hilo(s): %s\n' "$nt" "${salida:-sin medir}"
done

echo ""
echo "======================================================================"
echo " PRUEBA 3: rechazo de edicion mientras una tarea usa el archivo"
echo "======================================================================"
# Se lanza la compresion y, sin esperar, se intenta modificar el mismo archivo.
md5_antes=$(md5sum < "$DIR/escala.txt" | cut -d' ' -f1)
resultado=$(printf 'Comprimir %s 8\nAdd %s "texto intruso"\nexit\n' \
            "$DIR/escala.txt" "$DIR/escala.txt" | "$SHELL_BIN" 2>&1 \
            | sed 's/\x1b\[[0-9;]*m//g')
md5_despues=$(md5sum < "$DIR/escala.txt" | cut -d' ' -f1)

if echo "$resultado" | grep -q "en uso por una tarea"; then
    echo "  OK     el comando Add fue rechazado durante la tarea"
else
    echo "  AVISO  la tarea termino antes de que Add llegara (prueba no concluyente)"
fi

if [ "$md5_antes" = "$md5_despues" ]; then
    echo "  OK     el archivo no fue modificado durante la compresion"
else
    echo "  FALLO  el archivo cambio mientras se comprimia"
    FALLOS=$((FALLOS + 1))
fi

echo ""
echo "======================================================================"
if [ "$FALLOS" -eq 0 ]; then
    echo " RESULTADO: todas las pruebas pasaron"
else
    echo " RESULTADO: $FALLOS prueba(s) fallaron"
fi
echo "======================================================================"
echo "Los archivos de prueba quedaron en $DIR/ (borralos con: rm -rf $DIR)"

exit "$FALLOS"
