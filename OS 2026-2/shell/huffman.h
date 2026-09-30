#ifndef HUFFMAN_H
#define HUFFMAN_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

/**
 * ====================================================================================
 * MOTOR DE COMPRESION HUFFMAN CONCURRENTE
 * ====================================================================================
 * Este modulo implementa el algoritmo de Codificacion de Huffman paralelizado con un
 * pool de hilos POSIX (pthreads). Es independiente del shell: no imprime nada por si
 * mismo, solo publica su avance en una estructura HuffProgress protegida por un mutex,
 * de modo que otro hilo (el que dibuja la interfaz) pueda consultarlo cuando quiera.
 *
 * Formato del archivo .huff generado:
 *
 *   [ Cabecera 2072 bytes ]
 *      magic[4]      "HUFF"
 *      version       uint16   (= 1)
 *      flags         uint16   (reservado, 0)
 *      chunk_size    uint32   (tamano del bloque sin comprimir)
 *      nchunks       uint32   (numero de bloques)
 *      orig_size     uint64   (tamano exacto del archivo original)
 *      freq[256]     uint64   (tabla de frecuencias global)
 *   [ Indice: nchunks * 24 bytes ]
 *      offset        uint64   (posicion del bloque comprimido en el archivo)
 *      nbits         uint64   (bits utiles del bloque; casi nunca multiplo de 8)
 *      raw_len       uint32   (bytes que ocupaba el bloque sin comprimir)
 *      pad           uint32   (alineacion)
 *   [ Datos: bloques comprimidos, en orden secuencial ]
 *
 * Guardar la tabla de frecuencias (y no la tabla de codigos) permite que el
 * descompresor reconstruya EXACTAMENTE el mismo arbol, porque la construccion es
 * determinista: el min-heap desempata frecuencias iguales por orden de creacion.
 *
 * Guardar el indice de bloques es lo que habilita la descompresion paralela: cada
 * bloque es independiente y se sabe en que bit empieza y cuantos bytes produce.
 */

#define HUFF_MAGIC        "HUFF"
#define HUFF_VERSION      1u
#define HUFF_CHUNK_SIZE   (64u * 1024u)   /* 64 KiB por bloque */
#define HUFF_SYMBOLS      256
#define HUFF_HEADER_BYTES (4 + 2 + 2 + 4 + 4 + 8 + (8 * HUFF_SYMBOLS))
#define HUFF_INDEX_ENTRY  24
#define HUFF_MAX_CODE_LEN 64              /* Limite del tipo uint64_t que guarda el codigo */

/* Modo de operacion del trabajo */
typedef enum {
    HUFF_MODE_COMPRESS,
    HUFF_MODE_DECOMPRESS
} HuffMode;

/* Estado observable de un trabajo */
typedef enum {
    HUFF_STATE_RUNNING,
    HUFF_STATE_DONE,
    HUFF_STATE_ERROR
} HuffState;

/**
 * Estructura de progreso compartida entre el pool de hilos (que la escribe) y el
 * hilo de interfaz (que la lee). TODO acceso debe hacerse con el mutex tomado;
 * huff_progress_snapshot() es la forma segura de leerla de una sola vez.
 */
typedef struct {
    pthread_mutex_t mutex;
    HuffState       state;
    char            phase[48];     /* Fase legible: "conteo", "codificacion", ... */
    uint64_t        total_units;   /* Unidades totales de trabajo */
    uint64_t        done_units;    /* Unidades completadas */
    char            error[192];    /* Mensaje si state == HUFF_STATE_ERROR */
    uint64_t        in_size;       /* Bytes de entrada */
    uint64_t        out_size;      /* Bytes de salida (valido al terminar) */
    unsigned        nthreads;      /* Hilos del pool */
    uint32_t        nchunks;       /* Bloques procesados */
} HuffProgress;

/* Tipo opaco: todo el estado interno del trabajo vive en huffman.c */
typedef struct HuffJob HuffJob;

/**
 * Crea un trabajo de compresion o descompresion. No arranca ningun hilo todavia;
 * solo valida los parametros y reserva la estructura de progreso.
 * Retorna NULL si no hay memoria o si nthreads es 0.
 */
HuffJob *huff_job_create(const char *in_path, const char *out_path,
                         HuffMode mode, unsigned nthreads);

/**
 * Ejecuta el trabajo completo. El hilo que llama a esta funcion actua como
 * COORDINADOR: crea el pool de trabajadores, construye el arbol de Huffman y
 * escribe la salida en orden usando la cola sincronizada. Bloquea hasta terminar,
 * por eso el shell la invoca desde un hilo de segundo plano.
 * Retorna 0 en exito y -1 en error (el detalle queda en HuffProgress.error).
 */
int huff_job_run(HuffJob *job);

/* Puntero a la estructura de progreso del trabajo (valida hasta huff_job_destroy). */
HuffProgress *huff_job_progress(HuffJob *job);

/* Libera toda la memoria del trabajo. Solo debe llamarse cuando huff_job_run retorno. */
void huff_job_destroy(HuffJob *job);

/**
 * Copia de forma atomica (bajo el mutex) el estado del progreso. Cualquier puntero
 * de salida puede ser NULL si ese dato no interesa.
 */
void huff_progress_snapshot(HuffProgress *p, HuffState *state, double *percent,
                            char *phase, size_t phase_sz,
                            char *err, size_t err_sz,
                            uint64_t *in_size, uint64_t *out_size);

/* Numero de hilos por omision: nucleos disponibles, acotado al rango [2, 16]. */
unsigned huff_default_threads(void);

#endif /* HUFFMAN_H */
