#include "huffman.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <sys/types.h>

/**
 * ====================================================================================
 * ESTRUCTURAS INTERNAS
 * ====================================================================================
 */

/* Nodo del arbol de Huffman. symbol == -1 identifica a un nodo interno. */
typedef struct HuffNode {
    uint64_t         freq;
    int              symbol;
    uint64_t         seq;      /* Orden de creacion: desempata frecuencias iguales */
    struct HuffNode *left;
    struct HuffNode *right;
} HuffNode;

/* Codigo binario asignado a un simbolo. Los bits utiles son los 'len' menos significativos. */
typedef struct {
    uint64_t bits;
    uint8_t  len;
} HuffCode;

/* Bloque ya codificado, esperando su turno para ser escrito en el archivo de salida. */
typedef struct {
    uint8_t  *buf;      /* Buffer con los bits comprimidos (NULL si aun no esta listo) */
    size_t    nbytes;   /* Bytes que ocupa buf */
    uint64_t  nbits;    /* Bits utiles dentro de buf */
    uint32_t  raw_len;  /* Bytes que ocupaba el bloque original */
    int       ready;    /* Bandera protegida por out_mutex */
} ChunkOut;

/* Entrada del indice tal como se guarda en el archivo */
typedef struct {
    uint64_t offset;
    uint64_t nbits;
    uint32_t raw_len;
    uint32_t pad;
} IndexEntry;

struct HuffJob {
    /* --- Configuracion --- */
    char      in_path[1024];
    char      out_path[1024];
    char      tmp_path[1064];   /* Se escribe aqui y se renombra al final (atomicidad) */
    HuffMode  mode;
    unsigned  nthreads;

    /* --- Datos de entrada --- */
    int       fd_in;
    int       fd_out;
    uint64_t  in_size;
    uint8_t  *in_buf;           /* Archivo de entrada completo en memoria */
    uint32_t  nchunks;
    uint32_t  chunk_size;
    uint64_t  orig_size;        /* Solo descompresion: tamano del archivo restaurado */

    /* --- Fase 1: conteo de frecuencias --- */
    uint64_t        freq[HUFF_SYMBOLS];
    pthread_mutex_t freq_mutex;  /* Protege la reduccion de las tablas locales */

    /* --- Sincronizacion de fases --- */
    pthread_barrier_t barrier;

    /* --- Arbol y codigos (solo lectura durante la fase 3) --- */
    HuffNode  *nodes;            /* Arena de nodos: evita miles de malloc/free */
    size_t     nodes_used;
    HuffNode  *root;
    HuffCode   codes[HUFF_SYMBOLS];

    /* --- Reparto dinamico de bloques entre trabajadores --- */
    uint32_t        next_chunk;
    pthread_mutex_t work_mutex;

    /* --- Cola de salida ordenada --- */
    ChunkOut       *out;         /* Un slot por bloque */
    IndexEntry     *index;
    pthread_mutex_t out_mutex;
    pthread_cond_t  out_cond;    /* Se senala cuando un bloque queda listo */
    sem_t           slots;       /* Limita los buffers en vuelo (contrapresion) */
    int             slots_ok;    /* 1 si el semaforo fue inicializado */

    /* --- Error compartido ---
     * Se protege con out_mutex, el mismo candado de la cola de salida. Unificarlos es
     * deliberado: el escritor necesita esperar por "el bloque esta listo O algo fallo",
     * y con un solo mutex y una sola variable de condicion no existe ningun anidamiento
     * de candados, por lo que es imposible construir un ciclo de espera (interbloqueo). */
    int             failed;

    /* --- Progreso publicado hacia la interfaz --- */
    HuffProgress    progress;
};

/* Argumento individual de cada hilo trabajador */
typedef struct {
    HuffJob *job;
    unsigned id;
} WorkerArg;

/**
 * ====================================================================================
 * UTILIDADES DE PROGRESO Y ERROR
 * ====================================================================================
 */

/* Registra el primer error ocurrido. Los errores posteriores no sobrescriben el original. */
static void job_fail(HuffJob *job, const char *fmt, ...) {
    char msg[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* Marca el fallo y despierta de inmediato a quien este esperando en la cola, para
     * que ningun hilo quede bloqueado aguardando un bloque que ya nunca llegara. */
    pthread_mutex_lock(&job->out_mutex);
    int first = !job->failed;
    job->failed = 1;
    pthread_cond_broadcast(&job->out_cond);
    pthread_mutex_unlock(&job->out_mutex);

    /* Solo el primer error se publica: es la causa raiz, los siguientes son su efecto. */
    if (first) {
        pthread_mutex_lock(&job->progress.mutex);
        job->progress.state = HUFF_STATE_ERROR;
        snprintf(job->progress.error, sizeof(job->progress.error), "%s", msg);
        pthread_mutex_unlock(&job->progress.mutex);
    }
}

static int job_failed(HuffJob *job) {
    pthread_mutex_lock(&job->out_mutex);
    int f = job->failed;
    pthread_mutex_unlock(&job->out_mutex);
    return f;
}

static void progress_set_phase(HuffJob *job, const char *phase) {
    pthread_mutex_lock(&job->progress.mutex);
    snprintf(job->progress.phase, sizeof(job->progress.phase), "%s", phase);
    pthread_mutex_unlock(&job->progress.mutex);
}

static void progress_add(HuffJob *job, uint64_t units) {
    pthread_mutex_lock(&job->progress.mutex);
    job->progress.done_units += units;
    pthread_mutex_unlock(&job->progress.mutex);
}

void huff_progress_snapshot(HuffProgress *p, HuffState *state, double *percent,
                            char *phase, size_t phase_sz,
                            char *err, size_t err_sz,
                            uint64_t *in_size, uint64_t *out_size) {
    pthread_mutex_lock(&p->mutex);
    if (state)  *state = p->state;
    if (percent) {
        *percent = (p->total_units > 0)
                 ? ((double)p->done_units * 100.0 / (double)p->total_units)
                 : (p->state == HUFF_STATE_RUNNING ? 0.0 : 100.0);
        if (*percent > 100.0) *percent = 100.0;
    }
    if (phase && phase_sz > 0)  snprintf(phase, phase_sz, "%s", p->phase);
    if (err && err_sz > 0)      snprintf(err, err_sz, "%s", p->error);
    if (in_size)  *in_size  = p->in_size;
    if (out_size) *out_size = p->out_size;
    pthread_mutex_unlock(&p->mutex);
}

unsigned huff_default_threads(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 2)  n = 2;    /* Siempre al menos 2 para que exista concurrencia real */
    if (n > 16) n = 16;
    return (unsigned)n;
}

/**
 * ====================================================================================
 * MIN-HEAP DETERMINISTA
 * ====================================================================================
 * Ordena por frecuencia y, ante frecuencias iguales, por orden de creacion (seq).
 * El desempate explicito es lo que garantiza que el compresor y el descompresor
 * construyan arboles identicos a partir de la misma tabla de frecuencias.
 */
static int node_less(const HuffNode *a, const HuffNode *b) {
    if (a->freq != b->freq) return a->freq < b->freq;
    return a->seq < b->seq;
}

static void heap_push(HuffNode **heap, size_t *n, HuffNode *node) {
    size_t i = (*n)++;
    heap[i] = node;
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (!node_less(heap[i], heap[parent])) break;
        HuffNode *t = heap[i]; heap[i] = heap[parent]; heap[parent] = t;
        i = parent;
    }
}

static HuffNode *heap_pop(HuffNode **heap, size_t *n) {
    HuffNode *top = heap[0];
    heap[0] = heap[--(*n)];
    size_t i = 0;
    while (1) {
        size_t l = 2 * i + 1, r = 2 * i + 2, m = i;
        if (l < *n && node_less(heap[l], heap[m])) m = l;
        if (r < *n && node_less(heap[r], heap[m])) m = r;
        if (m == i) break;
        HuffNode *t = heap[i]; heap[i] = heap[m]; heap[m] = t;
        i = m;
    }
    return top;
}

/**
 * Construye el arbol de Huffman global a partir de job->freq.
 * La arena de nodos tiene capacidad para 2*256 nodos: con S simbolos distintos el
 * arbol tiene S hojas y S-1 nodos internos, mas un nodo extra para el caso especial
 * de un unico simbolo distinto.
 */
static int build_tree(HuffJob *job) {
    job->nodes = calloc(2 * HUFF_SYMBOLS + 2, sizeof(HuffNode));
    if (job->nodes == NULL) {
        job_fail(job, "sin memoria para el arbol de Huffman");
        return -1;
    }
    job->nodes_used = 0;

    HuffNode *heap[HUFF_SYMBOLS + 2];
    size_t heap_n = 0;
    uint64_t seq = 0;

    /* Una hoja por cada simbolo presente, insertadas en orden de simbolo (determinismo) */
    for (int s = 0; s < HUFF_SYMBOLS; s++) {
        if (job->freq[s] == 0) continue;
        HuffNode *leaf = &job->nodes[job->nodes_used++];
        leaf->freq = job->freq[s];
        leaf->symbol = s;
        leaf->seq = seq++;
        heap_push(heap, &heap_n, leaf);
    }

    if (heap_n == 0) {
        job->root = NULL;   /* Archivo vacio: no hay arbol ni codigos */
        return 0;
    }

    /* Caso especial: un unico simbolo distinto. Sin nodo interno su codigo tendria
     * longitud 0 y seria imposible de escribir, asi que se crea una raiz artificial
     * para que el simbolo reciba un codigo de 1 bit. */
    if (heap_n == 1) {
        HuffNode *only = heap_pop(heap, &heap_n);
        HuffNode *dup = &job->nodes[job->nodes_used++];
        dup->freq = 0;
        dup->symbol = only->symbol;
        dup->seq = seq++;

        HuffNode *root = &job->nodes[job->nodes_used++];
        root->freq = only->freq;
        root->symbol = -1;
        root->seq = seq++;
        root->left = only;
        root->right = dup;
        job->root = root;
        return 0;
    }

    /* Algoritmo clasico: unir repetidamente los dos nodos de menor frecuencia */
    while (heap_n > 1) {
        HuffNode *a = heap_pop(heap, &heap_n);
        HuffNode *b = heap_pop(heap, &heap_n);
        HuffNode *parent = &job->nodes[job->nodes_used++];
        parent->freq = a->freq + b->freq;
        parent->symbol = -1;
        parent->seq = seq++;
        parent->left = a;
        parent->right = b;
        heap_push(heap, &heap_n, parent);
    }
    job->root = heap_pop(heap, &heap_n);
    return 0;
}

/**
 * Recorre el arbol asignando codigos: bit 0 al descender por la izquierda y bit 1
 * por la derecha. No sobrescribe un codigo ya asignado, lo que resuelve el caso del
 * simbolo duplicado en el arbol de un solo simbolo (se queda con el codigo "0").
 */
static int assign_codes(HuffJob *job, HuffNode *node, uint64_t bits, uint8_t len) {
    if (node == NULL) return 0;

    if (node->symbol >= 0) {
        if (len > HUFF_MAX_CODE_LEN) {
            job_fail(job, "codigo de %u bits: excede el limite de %d bits",
                     (unsigned)len, HUFF_MAX_CODE_LEN);
            return -1;
        }
        if (job->codes[node->symbol].len == 0) {
            job->codes[node->symbol].bits = bits;
            job->codes[node->symbol].len = len;
        }
        return 0;
    }
    if (assign_codes(job, node->left,  (bits << 1),       (uint8_t)(len + 1)) != 0) return -1;
    if (assign_codes(job, node->right, (bits << 1) | 1u,  (uint8_t)(len + 1)) != 0) return -1;
    return 0;
}

/**
 * ====================================================================================
 * ESCRITURA Y LECTURA DE BITS
 * ====================================================================================
 * Los codigos de Huffman tienen longitud variable, asi que no encajan en bytes. Estas
 * rutinas empaquetan los bits de mayor a menor peso dentro de cada byte (MSB primero).
 * El buffer destino debe venir en ceros (calloc), porque bw_put solo enciende bits.
 */
typedef struct {
    uint8_t *buf;
    size_t   byte_pos;
    int      bit_pos;
} BitWriter;

static void bw_put(BitWriter *w, uint64_t code, uint8_t len) {
    for (int i = (int)len - 1; i >= 0; i--) {
        if ((code >> i) & 1u) {
            w->buf[w->byte_pos] |= (uint8_t)(0x80u >> w->bit_pos);
        }
        if (++w->bit_pos == 8) {
            w->bit_pos = 0;
            w->byte_pos++;
        }
    }
}

/* Lee el bit numero 'i' (contando desde el inicio del buffer) */
static inline uint8_t br_get(const uint8_t *buf, uint64_t i) {
    return (uint8_t)((buf[i >> 3] >> (7 - (i & 7))) & 1u);
}

/**
 * ====================================================================================
 * REPARTO DINAMICO DE TRABAJO
 * ====================================================================================
 * En vez de asignar a cada hilo un rango fijo de bloques, los trabajadores toman el
 * siguiente bloque disponible bajo un mutex. Asi, si un bloque resulta mas costoso que
 * otro (texto muy repetitivo frente a datos aleatorios), ningun hilo queda ocioso.
 *
 * Los bloques se entregan en orden creciente, propiedad que el escritor aprovecha:
 * el bloque que el escritor espera siempre fue tomado antes que los bloques mayores,
 * por lo que hay alguien trabajando en el y el avance esta garantizado.
 *
 * Retorna 1 si entrego un bloque en *chunk_id, o 0 si ya no queda trabajo.
 */
static int take_next_chunk(HuffJob *job, uint32_t *chunk_id) {
    pthread_mutex_lock(&job->work_mutex);
    int got = 0;
    if (job->next_chunk < job->nchunks) {
        *chunk_id = job->next_chunk++;
        got = 1;
    }
    pthread_mutex_unlock(&job->work_mutex);
    return got;
}

/**
 * ====================================================================================
 * FASE 1 (POR TRABAJADOR): LECTURA PARALELA Y CONTEO DE FRECUENCIAS
 * ====================================================================================
 * Cada hilo lee sus propios bloques con pread(2), que recibe el desplazamiento como
 * parametro y por tanto NO comparte el cursor del descriptor: varios hilos pueden leer
 * el mismo archivo en paralelo sin pisarse.
 *
 * Las frecuencias se acumulan en una tabla LOCAL en la pila del hilo (reduccion local).
 * Solo al terminar todos sus bloques el hilo toma el mutex una unica vez para sumar su
 * tabla local a la global. Esto reduce la contencion de miles de bloqueos a uno solo
 * por hilo, y es el patron que pide el enunciado.
 */
static int worker_count(HuffJob *job) {
    uint64_t local[HUFF_SYMBOLS];
    memset(local, 0, sizeof(local));

    uint32_t id;
    while (take_next_chunk(job, &id)) {
        if (job_failed(job)) return -1;

        uint64_t offset = (uint64_t)id * job->chunk_size;
        size_t   len    = (size_t)((offset + job->chunk_size <= job->in_size)
                                   ? job->chunk_size
                                   : (job->in_size - offset));

        /* Lectura del bloque propio. Se repite mientras pread devuelva lecturas
         * parciales, que son legitimas y no un error. */
        size_t done = 0;
        while (done < len) {
            ssize_t n = pread(job->fd_in, job->in_buf + offset + done, len - done,
                              (off_t)(offset + done));
            if (n < 0) {
                if (errno == EINTR) continue;
                job_fail(job, "pread fallo en el bloque %u: %s", id, strerror(errno));
                return -1;
            }
            if (n == 0) {
                job_fail(job, "fin de archivo inesperado en el bloque %u", id);
                return -1;
            }
            done += (size_t)n;
        }

        const uint8_t *p = job->in_buf + offset;
        for (size_t i = 0; i < len; i++) {
            local[p[i]]++;
        }
        progress_add(job, 1);
    }

    /* Reduccion: una sola seccion critica por hilo */
    pthread_mutex_lock(&job->freq_mutex);
    for (int s = 0; s < HUFF_SYMBOLS; s++) {
        job->freq[s] += local[s];
    }
    pthread_mutex_unlock(&job->freq_mutex);
    return 0;
}

/**
 * ====================================================================================
 * FASE 3 (POR TRABAJADOR): CODIFICACION PARALELA DE BLOQUES
 * ====================================================================================
 * Con el arbol global ya construido, cada bloque se puede codificar de forma totalmente
 * independiente. El hilo:
 *   1. Espera un permiso del semaforo (contrapresion): limita cuantos buffers
 *      comprimidos pueden existir a la vez, evitando que un escritor lento haga que la
 *      memoria crezca sin control.
 *   2. Toma el siguiente bloque. El orden importa: pedir el permiso ANTES de reservar
 *      el bloque evita un interbloqueo en el que un hilo retiene el bloque que el
 *      escritor espera mientras aguarda un permiso que solo el escritor puede liberar.
 *   3. Codifica y publica el resultado, senalando la variable de condicion.
 */
static int worker_encode(HuffJob *job) {
    while (1) {
        if (job_failed(job)) return -1;

        /* 1. Permiso de memoria (ver nota sobre el orden) */
        while (sem_wait(&job->slots) != 0) {
            if (errno != EINTR) {
                job_fail(job, "sem_wait fallo: %s", strerror(errno));
                return -1;
            }
        }
        if (job_failed(job)) {
            sem_post(&job->slots);
            return -1;
        }

        /* 2. Reserva de trabajo */
        uint32_t id;
        if (!take_next_chunk(job, &id)) {
            sem_post(&job->slots);   /* No habia trabajo: devolver el permiso */
            return 0;
        }

        uint64_t offset = (uint64_t)id * job->chunk_size;
        size_t   len    = (size_t)((offset + job->chunk_size <= job->in_size)
                                   ? job->chunk_size
                                   : (job->in_size - offset));
        const uint8_t *src = job->in_buf + offset;

        /* Primera pasada: cuantos bits ocupara el bloque comprimido. Permite reservar
         * el buffer exacto en lugar de asumir el peor caso. */
        uint64_t nbits = 0;
        for (size_t i = 0; i < len; i++) {
            nbits += job->codes[src[i]].len;
        }
        size_t nbytes = (size_t)((nbits + 7) / 8);

        uint8_t *buf = calloc(nbytes > 0 ? nbytes : 1, 1);
        if (buf == NULL) {
            sem_post(&job->slots);
            job_fail(job, "sin memoria para comprimir el bloque %u", id);
            return -1;
        }

        /* Segunda pasada: escribir los codigos bit a bit */
        BitWriter w = { buf, 0, 0 };
        for (size_t i = 0; i < len; i++) {
            HuffCode c = job->codes[src[i]];
            bw_put(&w, c.bits, c.len);
        }

        /* 3. Publicar el bloque y despertar al escritor */
        pthread_mutex_lock(&job->out_mutex);
        job->out[id].buf     = buf;
        job->out[id].nbytes  = nbytes;
        job->out[id].nbits   = nbits;
        job->out[id].raw_len = (uint32_t)len;
        job->out[id].ready   = 1;
        pthread_cond_broadcast(&job->out_cond);
        pthread_mutex_unlock(&job->out_mutex);

        progress_add(job, 1);
    }
}

/* Punto de entrada de cada hilo del pool durante la compresion */
static void *worker_compress_main(void *arg) {
    WorkerArg *wa = (WorkerArg *)arg;
    HuffJob *job = wa->job;

    /* --- FASE 1: conteo paralelo --- */
    worker_count(job);

    /* --- BARRERA 1: nadie codifica hasta que la tabla global este completa --- */
    pthread_barrier_wait(&job->barrier);

    /* --- FASE 2: un solo hilo construye el arbol; los demas esperan en la barrera --- */
    if (wa->id == 0 && !job_failed(job)) {
        progress_set_phase(job, "construyendo arbol");
        if (build_tree(job) == 0 && job->root != NULL) {
            assign_codes(job, job->root, 0, 0);
        }
        /* Reiniciar el reparto de trabajo para la fase de codificacion */
        pthread_mutex_lock(&job->work_mutex);
        job->next_chunk = 0;
        pthread_mutex_unlock(&job->work_mutex);
        progress_set_phase(job, "codificando");
    }

    /* --- BARRERA 2: el arbol y los codigos ya son visibles para todos --- */
    pthread_barrier_wait(&job->barrier);

    /* --- FASE 3: codificacion paralela --- */
    if (!job_failed(job)) {
        worker_encode(job);
    } else {
        /* Aun en error hay que drenar la cola para que el escritor no espere por siempre */
        pthread_mutex_lock(&job->out_mutex);
        pthread_cond_broadcast(&job->out_cond);
        pthread_mutex_unlock(&job->out_mutex);
    }
    return NULL;
}

/**
 * ====================================================================================
 * ESCRITURA ROBUSTA EN DISCO
 * ====================================================================================
 * write(2) y pwrite(2) pueden escribir menos bytes de los pedidos o ser interrumpidos
 * por una senal (EINTR). Estas envolturas insisten hasta completar la operacion.
 */
static int pwrite_all(int fd, const void *buf, size_t len, off_t offset) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t done = 0;
    while (done < len) {
        ssize_t n = pwrite(fd, p + done, len - done, offset + (off_t)done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

static int pread_all(int fd, void *buf, size_t len, off_t offset) {
    uint8_t *p = (uint8_t *)buf;
    size_t done = 0;
    while (done < len) {
        ssize_t n = pread(fd, p + done, len - done, offset + (off_t)done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) { errno = EIO; return -1; }   /* Archivo mas corto de lo declarado */
        done += (size_t)n;
    }
    return 0;
}

/**
 * ====================================================================================
 * COORDINADOR / ESCRITOR: ENSAMBLADO EN ORDEN SECUENCIAL
 * ====================================================================================
 * Los bloques se terminan de comprimir en cualquier orden, pero el archivo debe
 * quedar en orden. Este hilo recorre los bloques del 0 al N-1 y, para cada uno, espera
 * en una VARIABLE DE CONDICION hasta que ese bloque especifico este listo.
 *
 * pthread_cond_wait libera el mutex y duerme al hilo: no hay espera activa (busy
 * waiting), el sistema operativo lo despierta solo cuando un trabajador senala.
 *
 * Tras escribir cada bloque libera su memoria y devuelve un permiso al semaforo,
 * habilitando a los trabajadores a producir el siguiente.
 */
static int writer_loop(HuffJob *job, off_t data_start, uint64_t *out_data_bytes) {
    off_t offset = data_start;

    for (uint32_t i = 0; i < job->nchunks; i++) {
        pthread_mutex_lock(&job->out_mutex);
        while (!job->out[i].ready && !job->failed) {
            pthread_cond_wait(&job->out_cond, &job->out_mutex);
        }
        int failed = job->failed;
        pthread_mutex_unlock(&job->out_mutex);

        if (failed) return -1;

        /* A partir de aqui el bloque es propiedad exclusiva del escritor: el trabajador
         * que lo produjo ya no lo toca, por eso se puede usar sin el mutex. */
        ChunkOut *c = &job->out[i];
        if (c->nbytes > 0 && pwrite_all(job->fd_out, c->buf, c->nbytes, offset) != 0) {
            job_fail(job, "pwrite fallo al escribir el bloque %u: %s", i, strerror(errno));
            return -1;
        }

        job->index[i].offset  = (uint64_t)offset;
        job->index[i].nbits   = c->nbits;
        job->index[i].raw_len = c->raw_len;
        offset += (off_t)c->nbytes;

        free(c->buf);
        c->buf = NULL;
        sem_post(&job->slots);   /* Devuelve el permiso de memoria */
    }

    *out_data_bytes = (uint64_t)(offset - data_start);
    return 0;
}

/* Serializa la cabecera en un buffer plano de HUFF_HEADER_BYTES bytes */
static void header_pack(const HuffJob *job, uint8_t *hdr) {
    memset(hdr, 0, HUFF_HEADER_BYTES);
    size_t p = 0;
    memcpy(hdr + p, HUFF_MAGIC, 4);                     p += 4;
    uint16_t version = HUFF_VERSION, flags = 0;
    memcpy(hdr + p, &version, sizeof(version));         p += sizeof(version);
    memcpy(hdr + p, &flags, sizeof(flags));             p += sizeof(flags);
    memcpy(hdr + p, &job->chunk_size, 4);               p += 4;
    memcpy(hdr + p, &job->nchunks, 4);                  p += 4;
    uint64_t orig = job->in_size;
    memcpy(hdr + p, &orig, 8);                          p += 8;
    memcpy(hdr + p, job->freq, 8 * HUFF_SYMBOLS);
}

static int header_unpack(HuffJob *job, const uint8_t *hdr) {
    if (memcmp(hdr, HUFF_MAGIC, 4) != 0) {
        job_fail(job, "el archivo no tiene el formato .huff (firma invalida)");
        return -1;
    }
    size_t p = 4;
    uint16_t version;
    memcpy(&version, hdr + p, sizeof(version));  p += sizeof(version);
    p += 2; /* flags */
    if (version != HUFF_VERSION) {
        job_fail(job, "version de formato %u no soportada (se esperaba %u)",
                 (unsigned)version, HUFF_VERSION);
        return -1;
    }
    memcpy(&job->chunk_size, hdr + p, 4);  p += 4;
    memcpy(&job->nchunks, hdr + p, 4);     p += 4;
    memcpy(&job->orig_size, hdr + p, 8);   p += 8;
    memcpy(job->freq, hdr + p, 8 * HUFF_SYMBOLS);

    if (job->chunk_size == 0 && job->nchunks > 0) {
        job_fail(job, "cabecera corrupta: tamano de bloque invalido");
        return -1;
    }
    return 0;
}

/**
 * ====================================================================================
 * FASE DE DESCOMPRESION (POR TRABAJADOR)
 * ====================================================================================
 * Gracias al indice de bloques, la descompresion tambien es paralelizable: cada bloque
 * se decodifica de forma independiente y se escribe con pwrite(2) en su posicion exacta
 * del archivo de salida (bloque i empieza en i * chunk_size).
 *
 * Como cada hilo escribe en un rango de bytes distinto y ajeno a los demas, no hace
 * falta ninguna cola ordenada: la ausencia de solapamiento es la que garantiza que no
 * haya condiciones de carrera en la salida.
 */
static void *worker_decompress_main(void *arg) {
    WorkerArg *wa = (WorkerArg *)arg;
    HuffJob *job = wa->job;

    uint32_t id;
    while (take_next_chunk(job, &id)) {
        if (job_failed(job)) return NULL;

        const IndexEntry *e = &job->index[id];
        uint32_t raw_len = e->raw_len;

        uint8_t *out = malloc(raw_len > 0 ? raw_len : 1);
        if (out == NULL) {
            job_fail(job, "sin memoria para descomprimir el bloque %u", id);
            return NULL;
        }

        /* Recorrido del arbol: cada bit elige rama; al llegar a una hoja se emite el
         * simbolo y se vuelve a la raiz. */
        const uint8_t *data = job->in_buf + e->offset;
        HuffNode *node = job->root;
        uint32_t produced = 0;

        for (uint64_t b = 0; b < e->nbits && produced < raw_len; b++) {
            node = br_get(data, b) ? node->right : node->left;
            if (node == NULL) {
                job_fail(job, "datos corruptos en el bloque %u (rama inexistente)", id);
                free(out);
                return NULL;
            }
            if (node->symbol >= 0) {
                out[produced++] = (uint8_t)node->symbol;
                node = job->root;
            }
        }

        if (produced != raw_len) {
            job_fail(job, "bloque %u incompleto: %u de %u bytes", id, produced, raw_len);
            free(out);
            return NULL;
        }

        off_t dst = (off_t)id * (off_t)job->chunk_size;
        if (raw_len > 0 && pwrite_all(job->fd_out, out, raw_len, dst) != 0) {
            job_fail(job, "pwrite fallo al restaurar el bloque %u: %s", id, strerror(errno));
            free(out);
            return NULL;
        }

        free(out);
        progress_add(job, 1);
    }
    return NULL;
}

/**
 * ====================================================================================
 * CREACION Y LIBERACION DEL TRABAJO
 * ====================================================================================
 */
HuffJob *huff_job_create(const char *in_path, const char *out_path,
                         HuffMode mode, unsigned nthreads) {
    if (in_path == NULL || out_path == NULL || nthreads == 0) return NULL;

    HuffJob *job = calloc(1, sizeof(HuffJob));
    if (job == NULL) return NULL;

    snprintf(job->in_path,  sizeof(job->in_path),  "%s", in_path);
    snprintf(job->out_path, sizeof(job->out_path), "%s", out_path);
    snprintf(job->tmp_path, sizeof(job->tmp_path), "%s.part", out_path);
    job->mode       = mode;
    job->nthreads   = nthreads;
    job->fd_in      = -1;
    job->fd_out     = -1;
    job->chunk_size = HUFF_CHUNK_SIZE;

    pthread_mutex_init(&job->freq_mutex, NULL);
    pthread_mutex_init(&job->work_mutex, NULL);
    pthread_mutex_init(&job->out_mutex, NULL);
    pthread_cond_init(&job->out_cond, NULL);
    pthread_mutex_init(&job->progress.mutex, NULL);

    job->progress.state    = HUFF_STATE_RUNNING;
    job->progress.nthreads = nthreads;
    snprintf(job->progress.phase, sizeof(job->progress.phase), "preparando");
    return job;
}

void huff_job_destroy(HuffJob *job) {
    if (job == NULL) return;

    /* Cierre de descriptores: si quedaron abiertos por un error, se cierran aqui */
    if (job->fd_in  >= 0) close(job->fd_in);
    if (job->fd_out >= 0) close(job->fd_out);

    /* Buffers de bloques que no llegaron a escribirse (ruta de error) */
    if (job->out != NULL) {
        for (uint32_t i = 0; i < job->nchunks; i++) {
            free(job->out[i].buf);
        }
        free(job->out);
    }
    free(job->index);
    free(job->in_buf);
    free(job->nodes);

    if (job->slots_ok) sem_destroy(&job->slots);
    pthread_mutex_destroy(&job->freq_mutex);
    pthread_mutex_destroy(&job->work_mutex);
    pthread_mutex_destroy(&job->out_mutex);
    pthread_cond_destroy(&job->out_cond);
    pthread_mutex_destroy(&job->progress.mutex);
    free(job);
}

HuffProgress *huff_job_progress(HuffJob *job) {
    return (job != NULL) ? &job->progress : NULL;
}

/**
 * ====================================================================================
 * SERIALIZACION DEL INDICE DE BLOQUES
 * ====================================================================================
 * Se escribe campo por campo en lugar de volcar el struct completo, para no depender
 * del relleno (padding) que el compilador pueda insertar entre miembros.
 */
static void index_pack(const IndexEntry *src, uint8_t *dst, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *p = dst + (size_t)i * HUFF_INDEX_ENTRY;
        memcpy(p + 0,  &src[i].offset,  8);
        memcpy(p + 8,  &src[i].nbits,   8);
        memcpy(p + 16, &src[i].raw_len, 4);
        memset(p + 20, 0, 4);
    }
}

static void index_unpack(IndexEntry *dst, const uint8_t *src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *p = src + (size_t)i * HUFF_INDEX_ENTRY;
        memcpy(&dst[i].offset,  p + 0,  8);
        memcpy(&dst[i].nbits,   p + 8,  8);
        memcpy(&dst[i].raw_len, p + 16, 4);
        dst[i].pad = 0;
    }
}

/* Lanza el pool y espera a que todos los hilos terminen (join, nunca detach: el
 * coordinador necesita saber que nadie sigue tocando los buffers antes de liberarlos). */
static int run_pool(HuffJob *job, void *(*entry)(void *), int writer_after_spawn,
                    off_t data_start, uint64_t *data_bytes) {
    pthread_t *tids = calloc(job->nthreads, sizeof(pthread_t));
    WorkerArg *args = calloc(job->nthreads, sizeof(WorkerArg));
    if (tids == NULL || args == NULL) {
        job_fail(job, "sin memoria para el pool de hilos");
        free(tids); free(args);
        return -1;
    }

    unsigned started = 0;
    for (unsigned i = 0; i < job->nthreads; i++) {
        args[i].job = job;
        args[i].id  = i;
        if (pthread_create(&tids[i], NULL, entry, &args[i]) != 0) {
            job_fail(job, "pthread_create fallo al crear el hilo %u", i);
            break;
        }
        started++;
    }

    /* Si no se pudieron crear todos los hilos, la barrera quedaria esperando a hilos
     * que no existen. Se aborta, pero siempre haciendo join de los que si arrancaron. */
    int spawn_ok = (started == job->nthreads);

    if (spawn_ok && writer_after_spawn) {
        writer_loop(job, data_start, data_bytes);
    }

    for (unsigned i = 0; i < started; i++) {
        pthread_join(tids[i], NULL);
    }

    free(tids);
    free(args);
    return (spawn_ok && !job_failed(job)) ? 0 : -1;
}

/**
 * ====================================================================================
 * MODO COMPRESION
 * ====================================================================================
 */
static int run_compress(HuffJob *job) {
    struct stat st;

    job->fd_in = open(job->in_path, O_RDONLY);
    if (job->fd_in < 0) {
        job_fail(job, "no se pudo abrir '%s': %s", job->in_path, strerror(errno));
        return -1;
    }
    if (fstat(job->fd_in, &st) != 0) {
        job_fail(job, "fstat fallo en '%s': %s", job->in_path, strerror(errno));
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        job_fail(job, "'%s' no es un archivo regular", job->in_path);
        return -1;
    }

    job->in_size = (uint64_t)st.st_size;
    job->nchunks = (uint32_t)((job->in_size + job->chunk_size - 1) / job->chunk_size);

    if (job->in_size > 0) {
        job->in_buf = malloc(job->in_size);
        if (job->in_buf == NULL) {
            job_fail(job, "el archivo ocupa %llu bytes y no cabe en memoria",
                     (unsigned long long)job->in_size);
            return -1;
        }
    }
    job->out   = calloc(job->nchunks > 0 ? job->nchunks : 1, sizeof(ChunkOut));
    job->index = calloc(job->nchunks > 0 ? job->nchunks : 1, sizeof(IndexEntry));
    if (job->out == NULL || job->index == NULL) {
        job_fail(job, "sin memoria para la cola de bloques");
        return -1;
    }

    pthread_mutex_lock(&job->progress.mutex);
    job->progress.in_size     = job->in_size;
    job->progress.nchunks     = job->nchunks;
    job->progress.total_units = (uint64_t)job->nchunks * 2;  /* contar + codificar */
    snprintf(job->progress.phase, sizeof(job->progress.phase), "contando frecuencias");
    pthread_mutex_unlock(&job->progress.mutex);

    /* Se escribe en un archivo temporal y solo al final se renombra: si el proceso
     * muere a medio camino, el usuario nunca ve un .huff invalido. */
    job->fd_out = open(job->tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (job->fd_out < 0) {
        job_fail(job, "no se pudo crear '%s': %s", job->tmp_path, strerror(errno));
        return -1;
    }

    if (pthread_barrier_init(&job->barrier, NULL, job->nthreads) != 0) {
        job_fail(job, "no se pudo inicializar la barrera de sincronizacion");
        return -1;
    }

    unsigned permits = job->nthreads * 2;
    if (permits < 4) permits = 4;
    if (sem_init(&job->slots, 0, permits) != 0) {
        job_fail(job, "no se pudo inicializar el semaforo: %s", strerror(errno));
        pthread_barrier_destroy(&job->barrier);
        return -1;
    }
    job->slots_ok = 1;

    off_t data_start = (off_t)HUFF_HEADER_BYTES +
                       (off_t)job->nchunks * HUFF_INDEX_ENTRY;
    uint64_t data_bytes = 0;

    int rc = run_pool(job, worker_compress_main, 1, data_start, &data_bytes);
    pthread_barrier_destroy(&job->barrier);
    if (rc != 0) {
        unlink(job->tmp_path);
        return -1;
    }

    /* Cabecera e indice al final: ya se conocen las frecuencias y los desplazamientos */
    uint8_t hdr[HUFF_HEADER_BYTES];
    header_pack(job, hdr);
    if (pwrite_all(job->fd_out, hdr, sizeof(hdr), 0) != 0) {
        job_fail(job, "no se pudo escribir la cabecera: %s", strerror(errno));
        unlink(job->tmp_path);
        return -1;
    }

    if (job->nchunks > 0) {
        size_t idx_bytes = (size_t)job->nchunks * HUFF_INDEX_ENTRY;
        uint8_t *idx = malloc(idx_bytes);
        if (idx == NULL) {
            job_fail(job, "sin memoria para el indice");
            unlink(job->tmp_path);
            return -1;
        }
        index_pack(job->index, idx, job->nchunks);
        int ok = pwrite_all(job->fd_out, idx, idx_bytes, HUFF_HEADER_BYTES);
        free(idx);
        if (ok != 0) {
            job_fail(job, "no se pudo escribir el indice: %s", strerror(errno));
            unlink(job->tmp_path);
            return -1;
        }
    }

    /* Volcado a disco y publicacion atomica del resultado */
    if (fsync(job->fd_out) != 0) {
        job_fail(job, "fsync fallo: %s", strerror(errno));
        unlink(job->tmp_path);
        return -1;
    }
    if (close(job->fd_out) != 0) {
        job->fd_out = -1;
        job_fail(job, "close fallo: %s", strerror(errno));
        unlink(job->tmp_path);
        return -1;
    }
    job->fd_out = -1;

    if (rename(job->tmp_path, job->out_path) != 0) {
        job_fail(job, "rename fallo: %s", strerror(errno));
        unlink(job->tmp_path);
        return -1;
    }

    pthread_mutex_lock(&job->progress.mutex);
    job->progress.out_size = (uint64_t)data_start + data_bytes;
    pthread_mutex_unlock(&job->progress.mutex);
    return 0;
}

/**
 * ====================================================================================
 * MODO DESCOMPRESION
 * ====================================================================================
 */
static int run_decompress(HuffJob *job) {
    struct stat st;

    job->fd_in = open(job->in_path, O_RDONLY);
    if (job->fd_in < 0) {
        job_fail(job, "no se pudo abrir '%s': %s", job->in_path, strerror(errno));
        return -1;
    }
    if (fstat(job->fd_in, &st) != 0) {
        job_fail(job, "fstat fallo en '%s': %s", job->in_path, strerror(errno));
        return -1;
    }
    job->in_size = (uint64_t)st.st_size;

    if (job->in_size < HUFF_HEADER_BYTES) {
        job_fail(job, "'%s' es demasiado pequeno para ser un archivo .huff", job->in_path);
        return -1;
    }

    uint8_t hdr[HUFF_HEADER_BYTES];
    if (pread_all(job->fd_in, hdr, sizeof(hdr), 0) != 0) {
        job_fail(job, "no se pudo leer la cabecera: %s", strerror(errno));
        return -1;
    }
    if (header_unpack(job, hdr) != 0) return -1;

    size_t idx_bytes = (size_t)job->nchunks * HUFF_INDEX_ENTRY;
    if (job->in_size < (uint64_t)HUFF_HEADER_BYTES + idx_bytes) {
        job_fail(job, "archivo truncado: falta parte del indice de bloques");
        return -1;
    }

    job->index = calloc(job->nchunks > 0 ? job->nchunks : 1, sizeof(IndexEntry));
    if (job->index == NULL) {
        job_fail(job, "sin memoria para el indice");
        return -1;
    }
    if (job->nchunks > 0) {
        uint8_t *idx = malloc(idx_bytes);
        if (idx == NULL) {
            job_fail(job, "sin memoria para leer el indice");
            return -1;
        }
        if (pread_all(job->fd_in, idx, idx_bytes, HUFF_HEADER_BYTES) != 0) {
            job_fail(job, "no se pudo leer el indice: %s", strerror(errno));
            free(idx);
            return -1;
        }
        index_unpack(job->index, idx, job->nchunks);
        free(idx);
    }

    /* Validacion del indice ANTES de decodificar: un archivo corrupto o manipulado no
     * debe poder hacer que los hilos lean fuera de los limites del buffer. */
    uint64_t suma_raw = 0;
    for (uint32_t i = 0; i < job->nchunks; i++) {
        const IndexEntry *e = &job->index[i];
        uint64_t need = e->offset + (e->nbits + 7) / 8;
        if (e->raw_len > job->chunk_size || need > job->in_size || e->offset > job->in_size) {
            job_fail(job, "indice corrupto en el bloque %u", i);
            return -1;
        }
        suma_raw += e->raw_len;
    }
    if (suma_raw != job->orig_size) {
        job_fail(job, "cabecera inconsistente: el indice suma %llu bytes y declara %llu",
                 (unsigned long long)suma_raw, (unsigned long long)job->orig_size);
        return -1;
    }

    /* Los datos comprimidos se cargan enteros: los hilos solo leen de este buffer, por
     * lo que no necesita ninguna proteccion. */
    job->in_buf = malloc(job->in_size);
    if (job->in_buf == NULL) {
        job_fail(job, "sin memoria para leer '%s'", job->in_path);
        return -1;
    }
    if (pread_all(job->fd_in, job->in_buf, job->in_size, 0) != 0) {
        job_fail(job, "no se pudo leer '%s': %s", job->in_path, strerror(errno));
        return -1;
    }

    /* Reconstruccion del arbol identico al usado al comprimir */
    progress_set_phase(job, "reconstruyendo arbol");
    if (build_tree(job) != 0) return -1;
    if (job->nchunks > 0 && job->root == NULL) {
        job_fail(job, "tabla de frecuencias vacia pero el archivo declara %u bloques",
                 job->nchunks);
        return -1;
    }

    job->fd_out = open(job->tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (job->fd_out < 0) {
        job_fail(job, "no se pudo crear '%s': %s", job->tmp_path, strerror(errno));
        return -1;
    }
    /* Se fija el tamano final de antemano para que los hilos puedan escribir en
     * paralelo, cada uno en su propio rango, con pwrite(2). */
    if (ftruncate(job->fd_out, (off_t)job->orig_size) != 0) {
        job_fail(job, "ftruncate fallo: %s", strerror(errno));
        unlink(job->tmp_path);
        return -1;
    }

    pthread_mutex_lock(&job->progress.mutex);
    job->progress.in_size     = job->in_size;
    job->progress.nchunks     = job->nchunks;
    job->progress.total_units = job->nchunks;
    snprintf(job->progress.phase, sizeof(job->progress.phase), "decodificando");
    pthread_mutex_unlock(&job->progress.mutex);

    if (run_pool(job, worker_decompress_main, 0, 0, NULL) != 0) {
        unlink(job->tmp_path);
        return -1;
    }

    if (fsync(job->fd_out) != 0 || close(job->fd_out) != 0) {
        job_fail(job, "no se pudo cerrar la salida: %s", strerror(errno));
        job->fd_out = -1;
        unlink(job->tmp_path);
        return -1;
    }
    job->fd_out = -1;

    if (rename(job->tmp_path, job->out_path) != 0) {
        job_fail(job, "rename fallo: %s", strerror(errno));
        unlink(job->tmp_path);
        return -1;
    }

    pthread_mutex_lock(&job->progress.mutex);
    job->progress.out_size = job->orig_size;
    pthread_mutex_unlock(&job->progress.mutex);
    return 0;
}

/**
 * ====================================================================================
 * PUNTO DE ENTRADA DEL TRABAJO
 * ====================================================================================
 */
int huff_job_run(HuffJob *job) {
    if (job == NULL) return -1;

    int rc = (job->mode == HUFF_MODE_COMPRESS) ? run_compress(job) : run_decompress(job);

    /* Los descriptores se cierran aqui y no en cada rama de error, de modo que ninguna
     * salida temprana pueda dejarlos abiertos. */
    if (job->fd_in >= 0)  { close(job->fd_in);  job->fd_in = -1; }
    if (job->fd_out >= 0) { close(job->fd_out); job->fd_out = -1; }

    pthread_mutex_lock(&job->progress.mutex);
    if (rc == 0) {
        job->progress.state = HUFF_STATE_DONE;
        job->progress.done_units = job->progress.total_units;
        snprintf(job->progress.phase, sizeof(job->progress.phase), "completado");
    } else if (job->progress.state != HUFF_STATE_ERROR) {
        job->progress.state = HUFF_STATE_ERROR;
        if (job->progress.error[0] == '\0') {
            snprintf(job->progress.error, sizeof(job->progress.error), "error desconocido");
        }
    }
    pthread_mutex_unlock(&job->progress.mutex);
    return rc;
}
