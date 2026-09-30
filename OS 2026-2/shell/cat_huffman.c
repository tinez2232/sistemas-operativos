#include "shell.h"
#include "huffman.h"

#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

/**
 * ====================================================================================
 * CATEGORIA: COMPRESION CONCURRENTE (Comprimir / Descomprimir / Tareas)
 * ====================================================================================
 * Este archivo es el puente entre el motor Huffman (huffman.c, que no sabe nada del
 * shell) y la interfaz interactiva. Sus tres responsabilidades son:
 *
 *   1. Ejecutar la compresion en SEGUNDO PLANO, de forma que el prompt siga
 *      respondiendo mientras el trabajo avanza.
 *   2. Reportar el progreso en tiempo real sin destruir lo que el usuario esta
 *      escribiendo en ese momento.
 *   3. Impedir que el usuario edite un archivo que una tarea esta leyendo o escribiendo
 *      (control de condiciones de carrera entre la interfaz y los hilos de trabajo).
 */

#define MAX_TASKS   8
#define MAX_FLOCKS  32
#define PROGRESS_MS 200    /* Cada cuanto consulta el progreso el hilo monitor */
#define PROGRESS_STEP 5.0  /* Minimo avance (en %) para volver a notificar */

/**
 * ====================================================================================
 * REGISTRO DE BLOQUEOS POR ARCHIVO (lectores / escritor)
 * ====================================================================================
 * Implementa la politica clasica de lector-escritor sobre nombres de archivo:
 *   - Varios lectores pueden coexistir (dos Print, o un Print mientras se comprime).
 *   - Un escritor excluye a todos (Add/Delete frente a una descompresion en curso).
 *
 * Todas las funciones son de tipo "try": si el archivo esta ocupado FALLAN de
 * inmediato en lugar de esperar. Esta decision elimina por construccion cualquier
 * posibilidad de interbloqueo, porque ningun hilo se queda nunca dormido reteniendo un
 * bloqueo, y tampoco hay espera activa porque no existe ningun bucle de reintento.
 */
typedef struct {
    int  used;
    char path[1024];
    int  readers;
    int  writer;
} FileLock;

static FileLock       g_flocks[MAX_FLOCKS];
static pthread_mutex_t g_flock_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Busca la entrada de un archivo; si create es 1 y no existe, reserva una libre. */
static FileLock *flock_find(const char *path, int create) {
    FileLock *libre = NULL;
    for (int i = 0; i < MAX_FLOCKS; i++) {
        if (g_flocks[i].used && strcmp(g_flocks[i].path, path) == 0) {
            return &g_flocks[i];
        }
        if (!g_flocks[i].used && libre == NULL) {
            libre = &g_flocks[i];
        }
    }
    if (create && libre != NULL) {
        libre->used = 1;
        libre->readers = 0;
        libre->writer = 0;
        snprintf(libre->path, sizeof(libre->path), "%s", path);
        return libre;
    }
    return NULL;
}

/* Libera la entrada si ya no tiene ni lectores ni escritor */
static void flock_gc(FileLock *fl) {
    if (fl != NULL && fl->readers == 0 && fl->writer == 0) {
        fl->used = 0;
        fl->path[0] = '\0';
    }
}

int shell_file_try_read(const char *path) {
    pthread_mutex_lock(&g_flock_mutex);
    int ok = 0;
    FileLock *fl = flock_find(path, 1);
    if (fl == NULL) {
        ok = 1;   /* Tabla llena: se permite la operacion en vez de bloquear el shell */
    } else if (!fl->writer) {
        fl->readers++;
        ok = 1;
    }
    pthread_mutex_unlock(&g_flock_mutex);
    return ok;
}

int shell_file_try_write(const char *path) {
    pthread_mutex_lock(&g_flock_mutex);
    int ok = 0;
    FileLock *fl = flock_find(path, 1);
    if (fl == NULL) {
        ok = 1;
    } else if (!fl->writer && fl->readers == 0) {
        fl->writer = 1;
        ok = 1;
    }
    pthread_mutex_unlock(&g_flock_mutex);
    return ok;
}

void shell_file_unlock_read(const char *path) {
    pthread_mutex_lock(&g_flock_mutex);
    FileLock *fl = flock_find(path, 0);
    if (fl != NULL && fl->readers > 0) fl->readers--;
    flock_gc(fl);
    pthread_mutex_unlock(&g_flock_mutex);
}

void shell_file_unlock_write(const char *path) {
    pthread_mutex_lock(&g_flock_mutex);
    FileLock *fl = flock_find(path, 0);
    if (fl != NULL) fl->writer = 0;
    flock_gc(fl);
    pthread_mutex_unlock(&g_flock_mutex);
}

/**
 * ====================================================================================
 * REGISTRO DE TAREAS EN SEGUNDO PLANO
 * ====================================================================================
 */
typedef struct {
    int       used;
    unsigned  num;                 /* Numero visible de la tarea */
    HuffMode  mode;
    char      in_path[1024];
    char      out_path[1024];
    unsigned  nthreads;

    HuffJob  *job;                 /* Vivo mientras corre; NULL cuando ya termino */
    HuffState final_state;
    char      final_msg[256];
    double    seconds;

    /* Sincronizacion del hilo monitor de progreso */
    pthread_mutex_t mon_mutex;
    pthread_cond_t  mon_cond;
    int             mon_stop;
} Task;

static Task            g_tasks[MAX_TASKS];
static unsigned        g_task_seq = 0;
static int             g_active = 0;
static pthread_mutex_t g_task_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_task_cond  = PTHREAD_COND_INITIALIZER;

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Dibuja una barra de progreso de ancho fijo: [########------------] */
static void format_bar(char *dst, size_t dst_sz, double percent) {
    const int width = 20;
    int filled = (int)((percent / 100.0) * width + 0.5);
    if (filled < 0) filled = 0;
    if (filled > width) filled = width;

    char bar[32];
    for (int i = 0; i < width; i++) bar[i] = (i < filled) ? '#' : '-';
    bar[width] = '\0';
    snprintf(dst, dst_sz, "[%s] %3.0f%%", bar, percent);
}

/**
 * HILO MONITOR: publica el progreso en la terminal sin estorbar al usuario.
 *
 * Duerme con pthread_cond_timedwait, no con sleep() ni con un bucle de sondeo: eso
 * permite que el hilo coordinador lo despierte al instante cuando la tarea termina,
 * en lugar de tener que esperar a que se agote el intervalo.
 */
static void *monitor_main(void *arg) {
    Task *t = (Task *)arg;
    double last_pct = -100.0;
    char   last_phase[48] = "";

    while (1) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (long)PROGRESS_MS * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += ts.tv_nsec / 1000000000L;
            ts.tv_nsec %= 1000000000L;
        }

        pthread_mutex_lock(&t->mon_mutex);
        while (!t->mon_stop) {
            if (pthread_cond_timedwait(&t->mon_cond, &t->mon_mutex, &ts) == ETIMEDOUT) {
                break;
            }
        }
        int stop = t->mon_stop;
        pthread_mutex_unlock(&t->mon_mutex);

        if (stop) break;

        /* El job solo se consulta con el registro tomado: el coordinador lo pone en
         * NULL antes de destruirlo, de modo que nunca se lee memoria liberada. */
        double pct = 0.0;
        char phase[48] = "";
        HuffState st = HUFF_STATE_RUNNING;
        int vivo = 0;

        pthread_mutex_lock(&g_task_mutex);
        if (t->job != NULL) {
            huff_progress_snapshot(huff_job_progress(t->job), &st, &pct,
                                   phase, sizeof(phase), NULL, 0, NULL, NULL);
            vivo = 1;
        }
        pthread_mutex_unlock(&g_task_mutex);

        if (!vivo || st != HUFF_STATE_RUNNING) continue;

        /* Se notifica solo cuando hay un avance apreciable o un cambio de fase, para no
         * inundar la terminal con decenas de lineas por segundo. */
        if (pct >= last_pct + PROGRESS_STEP || strcmp(phase, last_phase) != 0) {
            char bar[64], msg[256];
            format_bar(bar, sizeof(bar), pct);
            snprintf(msg, sizeof(msg),
                     COLOR_INFO "[tarea %u]" COLOR_RESET " %s " COLOR_PARAM "%s" COLOR_RESET " (%s)",
                     t->num,
                     (t->mode == HUFF_MODE_COMPRESS) ? "comprimiendo" : "descomprimiendo",
                     bar, phase);
            shell_notify(msg);
            last_pct = pct;
            snprintf(last_phase, sizeof(last_phase), "%s", phase);
        }
    }
    return NULL;
}

/**
 * HILO COORDINADOR DE LA TAREA (se ejecuta en segundo plano).
 *
 * Arranca el monitor, ejecuta el trabajo completo (dentro de huff_job_run se crea y se
 * hace join del pool de trabajadores), detiene el monitor y publica el resultado.
 */
static void *task_main(void *arg) {
    Task *t = (Task *)arg;
    double t0 = now_seconds();

    pthread_t mon;
    int mon_ok = (pthread_create(&mon, NULL, monitor_main, t) == 0);

    int rc = huff_job_run(t->job);

    /* Detener el monitor y esperarlo: join explicito, sin dejar hilos sueltos */
    if (mon_ok) {
        pthread_mutex_lock(&t->mon_mutex);
        t->mon_stop = 1;
        pthread_cond_signal(&t->mon_cond);
        pthread_mutex_unlock(&t->mon_mutex);
        pthread_join(mon, NULL);
    }

    /* Copiar el resultado antes de destruir el trabajo */
    HuffState st;
    char err[192];
    uint64_t in_size = 0, out_size = 0;
    huff_progress_snapshot(huff_job_progress(t->job), &st, NULL, NULL, 0,
                           err, sizeof(err), &in_size, &out_size);

    double elapsed = now_seconds() - t0;
    /* Holgado a proposito: los dos nombres de archivo pueden ocupar 1024 bytes cada uno */
    char msg[2600];

    if (rc == 0 && t->mode == HUFF_MODE_COMPRESS) {
        double ahorro = (in_size > 0)
                      ? (100.0 - ((double)out_size * 100.0 / (double)in_size))
                      : 0.0;
        snprintf(msg, sizeof(msg),
                 COLOR_RESULT "[tarea %u] listo" COLOR_RESET
                 " %s -> %s | %llu -> %llu bytes (ahorro %.1f%%) | %.2f s | %u hilos",
                 t->num, t->in_path, t->out_path,
                 (unsigned long long)in_size, (unsigned long long)out_size,
                 ahorro, elapsed, t->nthreads);
    } else if (rc == 0) {
        /* Al descomprimir, hablar de "ahorro" no tiene sentido: el archivo crece */
        snprintf(msg, sizeof(msg),
                 COLOR_RESULT "[tarea %u] listo" COLOR_RESET
                 " %s -> %s | %llu bytes restaurados | %.2f s | %u hilos",
                 t->num, t->in_path, t->out_path,
                 (unsigned long long)out_size, elapsed, t->nthreads);
    } else {
        snprintf(msg, sizeof(msg),
                 COLOR_ERROR "[tarea %u] fallo" COLOR_RESET " %s: %s",
                 t->num, t->in_path, err);
    }

    /* El puntero se anula CON EL REGISTRO TOMADO y solo despues se libera, para que el
     * comando 'Tareas' y el monitor nunca puedan leer memoria ya liberada. */
    pthread_mutex_lock(&g_task_mutex);
    HuffJob *job = t->job;
    t->job = NULL;
    t->final_state = (rc == 0) ? HUFF_STATE_DONE : HUFF_STATE_ERROR;
    t->seconds = elapsed;
    snprintf(t->final_msg, sizeof(t->final_msg), "%s",
             (rc == 0) ? "completado" : err);
    pthread_mutex_unlock(&g_task_mutex);

    huff_job_destroy(job);

    /* Liberar los bloqueos de archivo tomados al lanzar la tarea */
    shell_file_unlock_read(t->in_path);
    shell_file_unlock_write(t->out_path);

    shell_notify(msg);

    /* Avisar a quien espere el vaciado de tareas (el shell al salir) */
    pthread_mutex_lock(&g_task_mutex);
    g_active--;
    pthread_cond_broadcast(&g_task_cond);
    pthread_mutex_unlock(&g_task_mutex);

    return NULL;
}

/* Reserva una ranura del registro. Devuelve NULL si ya no hay espacio. */
static Task *task_alloc(void) {
    pthread_mutex_lock(&g_task_mutex);
    Task *t = NULL;
    for (int i = 0; i < MAX_TASKS; i++) {
        /* Se reutilizan las ranuras de tareas ya terminadas */
        if (!g_tasks[i].used || g_tasks[i].job == NULL) {
            if (g_tasks[i].used) {
                pthread_mutex_destroy(&g_tasks[i].mon_mutex);
                pthread_cond_destroy(&g_tasks[i].mon_cond);
            }
            t = &g_tasks[i];
            memset(t, 0, sizeof(*t));
            t->used = 1;
            t->num = ++g_task_seq;
            pthread_mutex_init(&t->mon_mutex, NULL);
            pthread_cond_init(&t->mon_cond, NULL);
            break;
        }
    }
    pthread_mutex_unlock(&g_task_mutex);
    return t;
}

/**
 * Lanza una tarea en segundo plano. Retorna 0 si quedo encolada.
 * El hilo coordinador se crea DESACOPLADO (detached) porque nadie va a hacer join de
 * el: su finalizacion se sigue con el contador g_active y la variable de condicion
 * g_task_cond, que es lo que consulta huff_wait_all() cuando el shell va a cerrarse.
 */
static int launch_task(const char *in_path, const char *out_path,
                      HuffMode mode, unsigned nthreads) {
    /* Reservar los accesos al archivo ANTES de crear el hilo */
    if (!shell_file_try_read(in_path)) {
        fprintf(stderr, COLOR_ERROR "'%s' esta siendo modificado por otra operacion.\n" COLOR_RESET, in_path);
        return 1;
    }
    if (!shell_file_try_write(out_path)) {
        shell_file_unlock_read(in_path);
        fprintf(stderr, COLOR_ERROR "'%s' esta ocupado por otra tarea.\n" COLOR_RESET, out_path);
        return 1;
    }

    /* Se comprueba antes de lanzar el hilo, para que el aviso salga junto al mensaje
     * de arranque y no mezclado con la salida de otra tarea. */
    int sobrescribe = (access(out_path, F_OK) == 0);

    Task *t = task_alloc();
    if (t == NULL) {
        shell_file_unlock_read(in_path);
        shell_file_unlock_write(out_path);
        fprintf(stderr, COLOR_ERROR "Ya hay %d tareas registradas. Usa 'Tareas' para verlas.\n" COLOR_RESET, MAX_TASKS);
        return 1;
    }

    snprintf(t->in_path,  sizeof(t->in_path),  "%s", in_path);
    snprintf(t->out_path, sizeof(t->out_path), "%s", out_path);
    t->mode = mode;
    t->nthreads = nthreads;
    t->final_state = HUFF_STATE_RUNNING;

    t->job = huff_job_create(in_path, out_path, mode, nthreads);
    if (t->job == NULL) {
        shell_file_unlock_read(in_path);
        shell_file_unlock_write(out_path);
        t->used = 0;
        fprintf(stderr, COLOR_ERROR "No se pudo preparar la tarea (sin memoria).\n" COLOR_RESET);
        return 1;
    }

    pthread_mutex_lock(&g_task_mutex);
    g_active++;
    pthread_mutex_unlock(&g_task_mutex);

    pthread_t tid;
    if (pthread_create(&tid, NULL, task_main, t) != 0) {
        pthread_mutex_lock(&g_task_mutex);
        g_active--;
        pthread_mutex_unlock(&g_task_mutex);
        huff_job_destroy(t->job);
        t->job = NULL;
        t->used = 0;
        shell_file_unlock_read(in_path);
        shell_file_unlock_write(out_path);
        fprintf(stderr, COLOR_ERROR "No se pudo crear el hilo de la tarea: %s\n" COLOR_RESET, strerror(errno));
        return 1;
    }
    pthread_detach(tid);

    printf(COLOR_RESULT "Tarea %u lanzada en segundo plano" COLOR_RESET
           " (%s -> %s, %u hilos).\n", t->num, in_path, out_path, nthreads);
    if (sobrescribe) {
        printf(COLOR_PARAM "Aviso: '%s' ya existia y sera reemplazado al terminar.\n" COLOR_RESET,
               out_path);
    }
    printf(COLOR_INFO "El prompt sigue disponible; el avance se ira mostrando solo. Usa 'Tareas' para consultarlo.\n" COLOR_RESET);
    return 0;
}

/**
 * Espera a que todas las tareas en segundo plano terminen. La llama el shell antes de
 * cerrarse, para no matar hilos que estan a medio escribir un archivo.
 */
void huff_wait_all(void) {
    pthread_mutex_lock(&g_task_mutex);
    if (g_active > 0) {
        printf(COLOR_INFO "Esperando %d tarea(s) en segundo plano...\n" COLOR_RESET, g_active);
        fflush(stdout);
    }
    while (g_active > 0) {
        pthread_cond_wait(&g_task_cond, &g_task_mutex);
    }
    pthread_mutex_unlock(&g_task_mutex);
}

/* Construye el nombre de salida por omision anexando o quitando la extension .huff */
static int derive_out_path(char *dst, size_t dst_sz, const char *in, HuffMode mode) {
    size_t len = strlen(in);
    if (mode == HUFF_MODE_COMPRESS) {
        if (len + 6 >= dst_sz) return -1;
        snprintf(dst, dst_sz, "%s.huff", in);
        return 0;
    }
    /* Descompresion: notas.txt.huff -> notas.txt ; si no acaba en .huff, usa .out */
    if (len > 5 && strcmp(in + len - 5, ".huff") == 0) {
        if (len - 5 + 1 >= dst_sz) return -1;
        snprintf(dst, dst_sz, "%.*s", (int)(len - 5), in);
    } else {
        if (len + 5 >= dst_sz) return -1;
        snprintf(dst, dst_sz, "%s.out", in);
    }
    return 0;
}

/**
 * ====================================================================================
 * COMANDO: Comprimir <archivo> [num_hilos]
 * ====================================================================================
 * Comprime con Huffman concurrente en segundo plano. Genera <archivo>.huff.
 * El numero de hilos es opcional: por omision usa los nucleos disponibles, y poder
 * fijarlo permite comparar tiempos con 1, 2, 4 u 8 hilos sobre el mismo archivo.
 */
int cmd_comprimir_huffman(int argc, char **argv) {
    if (argc != 2 && argc != 3) {
        fprintf(stderr, COLOR_ERROR "Uso: Comprimir <archivo> [num_hilos]\n" COLOR_RESET);
        return 1;
    }

    unsigned nthreads = huff_default_threads();
    if (argc == 3) {
        char *end;
        long n = strtol(argv[2], &end, 10);
        if (*end != '\0' || n < 1 || n > 64) {
            fprintf(stderr, COLOR_ERROR "El numero de hilos debe estar entre 1 y 64.\n" COLOR_RESET);
            return 1;
        }
        nthreads = (unsigned)n;
    }

    char out_path[1024];
    if (derive_out_path(out_path, sizeof(out_path), argv[1], HUFF_MODE_COMPRESS) != 0) {
        fprintf(stderr, COLOR_ERROR "El nombre '%s' es demasiado largo.\n" COLOR_RESET, argv[1]);
        return 1;
    }

    /* Verificacion temprana: es mejor avisar aqui que dentro del hilo */
    if (access(argv[1], R_OK) != 0) {
        fprintf(stderr, COLOR_ERROR "No se puede leer '%s': %s\n" COLOR_RESET, argv[1], strerror(errno));
        return 1;
    }

    return launch_task(argv[1], out_path, HUFF_MODE_COMPRESS, nthreads);
}

/**
 * ====================================================================================
 * COMANDO: Descomprimir <archivo.huff> [num_hilos]
 * ====================================================================================
 * Restaura un archivo .huff. Tambien es concurrente: el indice de bloques del formato
 * permite que cada hilo decodifique un bloque distinto y lo escriba en su posicion.
 */
int cmd_descomprimir_huffman(int argc, char **argv) {
    if (argc != 2 && argc != 3) {
        fprintf(stderr, COLOR_ERROR "Uso: Descomprimir <archivo.huff> [num_hilos]\n" COLOR_RESET);
        return 1;
    }

    unsigned nthreads = huff_default_threads();
    if (argc == 3) {
        char *end;
        long n = strtol(argv[2], &end, 10);
        if (*end != '\0' || n < 1 || n > 64) {
            fprintf(stderr, COLOR_ERROR "El numero de hilos debe estar entre 1 y 64.\n" COLOR_RESET);
            return 1;
        }
        nthreads = (unsigned)n;
    }

    char out_path[1024];
    if (derive_out_path(out_path, sizeof(out_path), argv[1], HUFF_MODE_DECOMPRESS) != 0) {
        fprintf(stderr, COLOR_ERROR "El nombre '%s' es demasiado largo.\n" COLOR_RESET, argv[1]);
        return 1;
    }
    if (access(argv[1], R_OK) != 0) {
        fprintf(stderr, COLOR_ERROR "No se puede leer '%s': %s\n" COLOR_RESET, argv[1], strerror(errno));
        return 1;
    }

    return launch_task(argv[1], out_path, HUFF_MODE_DECOMPRESS, nthreads);
}

/**
 * ====================================================================================
 * COMANDO: Tareas
 * ====================================================================================
 * Muestra el estado de todas las tareas de compresion lanzadas en la sesion. Consultar
 * el progreso bajo demanda complementa las notificaciones automaticas: con archivos
 * pequenos la tarea termina antes de que el monitor alcance a publicar nada.
 */
int cmd_tareas(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf(COLOR_TITLE "--- Tareas de compresion de esta sesion ---\n" COLOR_RESET);

    pthread_mutex_lock(&g_task_mutex);
    int mostradas = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        Task *t = &g_tasks[i];
        if (!t->used) continue;
        mostradas++;

        const char *accion = (t->mode == HUFF_MODE_COMPRESS) ? "Comprimir" : "Descomprimir";

        if (t->job != NULL) {
            double pct = 0.0;
            char phase[48] = "";
            HuffState st;
            huff_progress_snapshot(huff_job_progress(t->job), &st, &pct,
                                   phase, sizeof(phase), NULL, 0, NULL, NULL);
            char bar[64];
            format_bar(bar, sizeof(bar), pct);
            printf("  " COLOR_PARAM "#%u" COLOR_RESET " %-12s %-24s " COLOR_PROMPT "en curso" COLOR_RESET
                   " %s (%s, %u hilos)\n",
                   t->num, accion, t->in_path, bar, phase, t->nthreads);
        } else if (t->final_state == HUFF_STATE_DONE) {
            printf("  " COLOR_PARAM "#%u" COLOR_RESET " %-12s %-24s " COLOR_RESULT "completada" COLOR_RESET
                   " en %.2f s (%u hilos) -> %s\n",
                   t->num, accion, t->in_path, t->seconds, t->nthreads, t->out_path);
        } else {
            printf("  " COLOR_PARAM "#%u" COLOR_RESET " %-12s %-24s " COLOR_ERROR "con error" COLOR_RESET
                   ": %s\n",
                   t->num, accion, t->in_path, t->final_msg);
        }
    }
    int activas = g_active;
    pthread_mutex_unlock(&g_task_mutex);

    if (mostradas == 0) {
        printf(COLOR_INFO "  (ninguna todavia: prueba 'Comprimir <archivo>')\n" COLOR_RESET);
    } else {
        printf(COLOR_INFO "  Tareas activas ahora mismo: %d\n" COLOR_RESET, activas);
    }
    printf(COLOR_TITLE "-------------------------------------------\n" COLOR_RESET);
    return 0;
}
