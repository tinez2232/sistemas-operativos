#include "shell.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>

/**
 * ====================================================================================
 * COMANDO: Open <archivo>
 * ====================================================================================
 * Demuestra cómo open(2) puede abrir un archivo existente o crearlo si no existe,
 * dependiendo únicamente de las banderas (flags) que se le pasan.
 *
 * Syscalls explicadas:
 * 1. open(2): Solicita al kernel abrir el archivo indicado.
 *    - O_RDWR: Abre el archivo para lectura y escritura.
 *    - O_CREAT: Si el archivo no existe, el kernel lo crea (sin esta bandera,
 *      open() fallaría con ENOENT cuando el archivo no existe).
 *    - 0644 (modo): Permisos en octal (-rw-r--r--) aplicados solo si el archivo se crea.
 *    Nótese que NO se usa O_TRUNC: si el archivo ya existía, su contenido se conserva intacto.
 * 2. close(2): Libera la entrada en la tabla de descriptores de archivos del proceso.
 */
int cmd_o_open(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, COLOR_ERROR "Uso: Open <archivo>\n" COLOR_RESET);
        return 1;
    }
    const char *filename = argv[1];

    /* Comprobación previa (no es una syscall trazada): solo sirve para informarle
     * al usuario si el archivo ya existía o si open() está a punto de crearlo. */
    struct stat st;
    int existia = (stat(filename, &st) == 0);

    /* 1. LLAMADA AL SISTEMA: open */
    LOG_SYSCALL("open", "\"%s\", O_RDWR|O_CREAT, 0644", filename);
    int fd = open(filename, O_RDWR | O_CREAT, 0644);
    if (fd == -1) {
        /* Si falla open, retorna -1 y asigna el código en errno */
        LOG_SYSCALL_ERROR(strerror(errno));
        return 1;
    }
    LOG_SYSCALL_RESULT(fd); /* El valor devuelto es un entero llamado descriptor de archivo (File Descriptor) */

    /* 2. LLAMADA AL SISTEMA: close */
    LOG_SYSCALL("close", "%d", fd);
    int res = close(fd);
    if (res == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        return 1;
    }
    LOG_SYSCALL_RESULT(res); /* Retorna 0 en caso de éxito */

    if (existia) {
        printf(COLOR_RESULT "Archivo '%s' abierto (ya existía).\n" COLOR_RESET, filename);
    } else {
        printf(COLOR_RESULT "Archivo '%s' no existía: creado exitosamente.\n" COLOR_RESET, filename);
    }
    return 0;
}

/**
 * ====================================================================================
 * COMANDO: Print <archivo>
 * ====================================================================================
 * Demuestra la lectura de archivos utilizando descriptores de archivo del sistema.
 *
 * Syscalls explicadas:
 * 1. open(2): Abre un archivo existente.
 *    - O_RDONLY: Abre el archivo en modo de solo lectura.
 * 2. read(2): Copia bytes desde el archivo apuntado por el descriptor al buffer en memoria RAM.
 *    - Retorna el número de bytes leídos. Si llega al final del archivo (EOF), retorna 0.
 * 3. close(2): Cierra la conexión al archivo.
 */
int cmd_p_print(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, COLOR_ERROR "Uso: Print <archivo>\n" COLOR_RESET);
        return 1;
    }
    const char *filename = argv[1];
    char buffer[1024];

    /* 1. LLAMADA AL SISTEMA: open */
    LOG_SYSCALL("open", "\"%s\", O_RDONLY", filename);
    int fd = open(filename, O_RDONLY);
    if (fd == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        return 1;
    }
    LOG_SYSCALL_RESULT(fd);

    /* 2. LLAMADA AL SISTEMA: read */
    LOG_SYSCALL("read", "%d, buffer, %zu", fd, sizeof(buffer) - 1);
    ssize_t bytes_read = read(fd, buffer, sizeof(buffer) - 1);
    if (bytes_read == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        close(fd);
        return 1;
    }
    LOG_SYSCALL_RESULT(bytes_read);
    buffer[bytes_read] = '\0'; /* Añadir carácter nulo para poder imprimirlo de forma segura como string */

    /* 3. LLAMADA AL SISTEMA: close */
    LOG_SYSCALL("close", "%d", fd);
    int res = close(fd);
    if (res == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        return 1;
    }
    LOG_SYSCALL_RESULT(res);

    printf(COLOR_TITLE "--- Contenido del Archivo ---\n" COLOR_RESET);
    printf("%s\n", buffer);
    printf(COLOR_TITLE "-----------------------------\n" COLOR_RESET);

    return 0;
}

/**
 * ====================================================================================
 * COMANDO: Add <archivo> <texto>
 * ====================================================================================
 * Demuestra cómo agregar contenido al final de un archivo sin borrar lo que ya tenía,
 * usando la bandera O_APPEND en lugar de O_TRUNC.
 *
 * Si el archivo ya tiene contenido y su último byte no es un salto de línea, primero
 * se escribe un '\n' antes del texto nuevo, para que este siempre comience en una
 * línea propia en lugar de quedar pegado al final de la línea anterior.
 *
 * Syscalls explicadas:
 * 1. stat(2): Se usa antes de abrir para conocer el tamaño actual del archivo.
 * 2. open(2) + lseek(2) + read(2): Si el archivo ya tiene contenido, se abren en modo
 *    lectura, se posiciona el cursor en el último byte (SEEK_END) y se lee ese byte
 *    para saber si ya es un '\n'.
 * 3. open(2) (O_WRONLY|O_CREAT|O_APPEND): Abre el archivo para escritura, creándolo
 *    si no existe. O_APPEND mueve el cursor al final antes de cada write(2) de forma
 *    atómica, garantizando que nada sobrescriba el contenido previo.
 * 4. write(2): Escribe primero el '\n' de separación (si hace falta) y luego el texto.
 * 5. close(2): Libera la entrada en la tabla de descriptores de archivos del proceso.
 */
int cmd_add_append(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, COLOR_ERROR "Uso: Add <archivo> \"<texto>\"\n" COLOR_RESET);
        return 1;
    }
    const char *filename = argv[1];
    const char *text = argv[2];
    size_t len = strlen(text);

    /* 1. LLAMADA AL SISTEMA: stat (para saber si el archivo ya tiene contenido) */
    struct stat st;
    LOG_SYSCALL("stat", "\"%s\", &st", filename);
    int existia = (stat(filename, &st) == 0);
    if (existia) LOG_SYSCALL_RESULT(0);
    else LOG_SYSCALL_ERROR(strerror(errno));

    /* 2. Si ya existe y tiene contenido, revisar su último byte para decidir si
     * hace falta anteponer un '\n' antes del texto nuevo. */
    int need_newline = 0;
    if (existia && st.st_size > 0) {
        LOG_SYSCALL("open", "\"%s\", O_RDONLY", filename);
        int fd_check = open(filename, O_RDONLY);
        if (fd_check == -1) {
            LOG_SYSCALL_ERROR(strerror(errno));
            return 1;
        }
        LOG_SYSCALL_RESULT(fd_check);

        LOG_SYSCALL("lseek", "%d, -1, SEEK_END", fd_check);
        off_t pos = lseek(fd_check, -1, SEEK_END);
        LOG_SYSCALL_RESULT(pos);

        char last_byte = '\n';
        LOG_SYSCALL("read", "%d, &last_byte, 1", fd_check);
        ssize_t r = read(fd_check, &last_byte, 1);
        LOG_SYSCALL_RESULT(r);
        if (r == 1 && last_byte != '\n') need_newline = 1;

        LOG_SYSCALL("close", "%d", fd_check);
        LOG_SYSCALL_RESULT(close(fd_check));
    }

    /* 3. LLAMADA AL SISTEMA: open (escritura, en modo append) */
    LOG_SYSCALL("open", "\"%s\", O_WRONLY|O_CREAT|O_APPEND, 0644", filename);
    int fd = open(filename, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        return 1;
    }
    LOG_SYSCALL_RESULT(fd);

    /* 4. LLAMADA AL SISTEMA: write del separador '\n', solo si hacía falta */
    if (need_newline) {
        LOG_SYSCALL("write", "%d, \"\\n\", 1", fd);
        ssize_t nl_written = write(fd, "\n", 1);
        if (nl_written == -1) {
            LOG_SYSCALL_ERROR(strerror(errno));
            close(fd);
            return 1;
        }
        LOG_SYSCALL_RESULT(nl_written);
    }

    /* 5. LLAMADA AL SISTEMA: write del texto nuevo */
    LOG_SYSCALL("write", "%d, \"%s\", %zu", fd, text, len);
    ssize_t bytes_written = write(fd, text, len);
    if (bytes_written == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        close(fd);
        return 1;
    }
    LOG_SYSCALL_RESULT(bytes_written);

    /* 6. LLAMADA AL SISTEMA: close */
    LOG_SYSCALL("close", "%d", fd);
    int res = close(fd);
    if (res == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        return 1;
    }
    LOG_SYSCALL_RESULT(res);

    printf(COLOR_RESULT "Se agregaron %zd bytes al final de '%s'.\n" COLOR_RESET, bytes_written, filename);
    return 0;
}

/**
 * ====================================================================================
 * COMANDO: Delete <archivo> [numero_de_linea]
 * ====================================================================================
 * Sin número de línea: elimina el archivo completo (la entrada de directorio).
 * Con número de línea: elimina solo esa línea y desplaza los bytes posteriores.
 *
 * Syscalls explicadas:
 * 1. unlink(2): Elimina el nombre (entrada de directorio) que apunta a un archivo.
 *    El kernel libera el inodo y el espacio en disco solo cuando el contador de
 *    enlaces llega a 0 y ningún proceso lo tiene abierto.
 *
 * Para borrar una línea específica, el kernel NO ofrece una syscall que "abra un
 * hueco" en medio de un archivo: read/write solo agregan o sobrescriben bytes en
 * un offset dado. Por eso la técnica clásica es: leer el archivo completo a memoria
 * (open+read+close), quitar en RAM los bytes de la línea indicada desplazando
 * (memmove) los bytes posteriores hacia atrás, y reescribir el archivo truncado
 * con el resultado (open con O_TRUNC + write + close).
 */
int cmd_delete(int argc, char **argv) {
    if (argc != 2 && argc != 3) {
        fprintf(stderr, COLOR_ERROR "Uso: Delete <archivo> [numero_de_linea]\n" COLOR_RESET);
        return 1;
    }
    const char *filename = argv[1];

    /* --- Caso 1: Sin número de línea -> eliminar el archivo completo --- */
    if (argc == 2) {
        LOG_SYSCALL("unlink", "\"%s\"", filename);
        int res = unlink(filename);
        if (res == -1) {
            LOG_SYSCALL_ERROR(strerror(errno));
            return 1;
        }
        LOG_SYSCALL_RESULT(res);
        printf(COLOR_RESULT "Archivo '%s' eliminado por completo.\n" COLOR_RESET, filename);
        return 0;
    }

    /* --- Caso 2: Con número de línea -> eliminar esa línea y desplazar el resto --- */
    char *endptr;
    long line_num = strtol(argv[2], &endptr, 10);
    if (*endptr != '\0' || line_num <= 0) {
        fprintf(stderr, COLOR_ERROR "El número de línea debe ser un entero positivo.\n" COLOR_RESET);
        return 1;
    }

    /* 1. stat: se usa para conocer el tamaño exacto y reservar un buffer suficiente */
    struct stat st;
    LOG_SYSCALL("stat", "\"%s\", &st", filename);
    if (stat(filename, &st) == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        return 1;
    }
    LOG_SYSCALL_RESULT(0);
    size_t filesize = (size_t)st.st_size;

    char *buffer = malloc(filesize + 1);
    if (buffer == NULL) {
        fprintf(stderr, COLOR_ERROR "No hay memoria suficiente para leer el archivo.\n" COLOR_RESET);
        return 1;
    }

    /* 2. LLAMADA AL SISTEMA: open (lectura) */
    LOG_SYSCALL("open", "\"%s\", O_RDONLY", filename);
    int fd = open(filename, O_RDONLY);
    if (fd == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        free(buffer);
        return 1;
    }
    LOG_SYSCALL_RESULT(fd);

    /* 3. LLAMADA AL SISTEMA: read (puede requerir varias llamadas hasta cubrir filesize) */
    size_t total_read = 0;
    while (total_read < filesize) {
        LOG_SYSCALL("read", "%d, buffer+%zu, %zu", fd, total_read, filesize - total_read);
        ssize_t n = read(fd, buffer + total_read, filesize - total_read);
        if (n < 0) {
            LOG_SYSCALL_ERROR(strerror(errno));
            close(fd);
            free(buffer);
            return 1;
        }
        LOG_SYSCALL_RESULT(n);
        if (n == 0) break; /* EOF antes de lo esperado */
        total_read += (size_t)n;
    }

    LOG_SYSCALL("close", "%d", fd);
    int res = close(fd);
    if (res == -1) LOG_SYSCALL_ERROR(strerror(errno));
    else LOG_SYSCALL_RESULT(res);

    /* 4. Localizar en memoria el rango de bytes [del_start, del_end) de la línea N,
     * incluyendo su carácter '\n' final si lo tiene. */
    size_t line_start = 0;
    long current_line = 1;
    size_t del_start = 0, del_end = 0;
    int found = 0;
    for (size_t i = 0; i <= total_read; i++) {
        if (i == total_read || buffer[i] == '\n') {
            if (current_line == line_num) {
                del_start = line_start;
                del_end = (i < total_read) ? i + 1 : i;
                found = 1;
                break;
            }
            current_line++;
            line_start = i + 1;
        }
    }

    if (!found) {
        fprintf(stderr, COLOR_ERROR "El archivo '%s' no tiene %ld líneas.\n" COLOR_RESET, filename, line_num);
        free(buffer);
        return 1;
    }

    /* 5. Desplazar en RAM los bytes posteriores a la línea eliminada, "tapando" el hueco */
    memmove(buffer + del_start, buffer + del_end, total_read - del_end);
    size_t new_size = total_read - (del_end - del_start);

    /* 6. LLAMADA AL SISTEMA: open (escritura, O_TRUNC vacía el archivo antes de reescribirlo) */
    LOG_SYSCALL("open", "\"%s\", O_WRONLY|O_TRUNC", filename);
    fd = open(filename, O_WRONLY | O_TRUNC);
    if (fd == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        free(buffer);
        return 1;
    }
    LOG_SYSCALL_RESULT(fd);

    /* 7. LLAMADA AL SISTEMA: write (el contenido restante, ya desplazado en memoria) */
    LOG_SYSCALL("write", "%d, buffer, %zu", fd, new_size);
    ssize_t bytes_written = write(fd, buffer, new_size);
    if (bytes_written == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        close(fd);
        free(buffer);
        return 1;
    }
    LOG_SYSCALL_RESULT(bytes_written);

    LOG_SYSCALL("close", "%d", fd);
    res = close(fd);
    if (res == -1) LOG_SYSCALL_ERROR(strerror(errno));
    else LOG_SYSCALL_RESULT(res);

    free(buffer);
    printf(COLOR_RESULT "Línea %ld eliminada de '%s'.\n" COLOR_RESET, line_num, filename);
    return 0;
}

/**
 * ====================================================================================
 * COMANDO: d_info <archivo>
 * ====================================================================================
 * Muestra información estructural del inodo de un archivo.
 * 
 * Syscalls explicadas:
 * 1. stat(2): Obtiene estadísticas de metadatos de un archivo sin necesidad de abrirlo.
 *    - Llena la estructura struct stat con metadatos del inodo del archivo en el sistema de ficheros.
 */
int cmd_d_info(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, COLOR_ERROR "Uso: d_info <archivo>\n" COLOR_RESET);
        return 1;
    }
    const char *filename = argv[1];
    struct stat st;

    /* 1. LLAMADA AL SISTEMA: stat */
    LOG_SYSCALL("stat", "\"%s\", &st", filename);
    int res = stat(filename, &st);
    if (res == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        return 1;
    }
    LOG_SYSCALL_RESULT(res); /* Retorna 0 en caso de éxito */

    printf(COLOR_TITLE "--- Metadatos del Archivo (stat) ---\n" COLOR_RESET);
    /* st_dev: ID del dispositivo físico que contiene el archivo */
    printf("  Dispositivo:    [%ld,%ld]\n", (long)major(st.st_dev), (long)minor(st.st_dev));
    /* st_ino: Número de inodo único del archivo dentro del dispositivo */
    printf("  Inodo:          " COLOR_PARAM "%ld" COLOR_RESET "\n", (long)st.st_ino);
    /* st_mode: Modos y permisos de acceso del archivo */
    printf("  Permisos (oct): " COLOR_PARAM "%o" COLOR_RESET "\n", st.st_mode & 0777);
    /* st_nlink: Número de enlaces físicos apuntando a este inodo */
    printf("  Enlaces (nlink):%ld\n", (long)st.st_nlink);
    /* st_uid y st_gid: IDs de dueño y grupo asignados */
    printf("  UID de Dueño:   %ld\n", (long)st.st_uid);
    printf("  GID de Grupo:   %ld\n", (long)st.st_gid);
    /* st_size: Tamaño real del archivo en bytes */
    printf("  Tamaño:         " COLOR_RESULT "%ld bytes" COLOR_RESET "\n", (long)st.st_size);
    /* st_blksize: Tamaño óptimo de bloque sugerido por el sistema de ficheros para E/S */
    printf("  Tamaño Bloque:  %ld bytes\n", (long)st.st_blksize);
    /* st_blocks: Número real de bloques de 512 bytes asignados en el disco */
    printf("  Bloques Ocup.:  %ld\n", (long)st.st_blocks);
    /* st_mtime: Fecha y hora de la última modificación del contenido */
    printf("  MTime (Modif):  %s", ctime(&st.st_mtime));
    printf(COLOR_TITLE "------------------------------------\n" COLOR_RESET);

    return 0;
}

/**
 * ====================================================================================
 * COMANDO: d_copy <origen> <destino>
 * ====================================================================================
 * Demuestra la copia progresiva de datos a nivel de bytes mediante búferes.
 * 
 * Este comando abre el archivo de origen en modo lectura (O_RDONLY), el archivo destino
 * en modo escritura (O_WRONLY | O_CREAT | O_TRUNC), y transfiere los bloques leyéndolos 
 * en memoria intermedia RAM e inmediatamente escribiéndolos en el destino.
 */
int cmd_d_copy(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, COLOR_ERROR "Uso: d_copy <origen> <destino>\n" COLOR_RESET);
        return 1;
    }
    const char *src_filename = argv[1];
    const char *dst_filename = argv[2];
    char buffer[512];
    ssize_t bytes_read, bytes_written;

    /* 1. Abrir archivo origen (Lectura) */
    LOG_SYSCALL("open", "\"%s\", O_RDONLY", src_filename);
    int fd_src = open(src_filename, O_RDONLY);
    if (fd_src == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        return 1;
    }
    LOG_SYSCALL_RESULT(fd_src);

    /* 2. Abrir/Crear archivo destino (Escritura) */
    LOG_SYSCALL("open", "\"%s\", O_WRONLY|O_CREAT|O_TRUNC, 0644", dst_filename);
    int fd_dst = open(dst_filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_dst == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        close(fd_src);
        return 1;
    }
    LOG_SYSCALL_RESULT(fd_dst);

    /* 3. Bucle de Copia: Lee del origen y escribe en el destino bloque a bloque */
    size_t total_copied = 0;
    while (1) {
        /* Leer del origen */
        LOG_SYSCALL("read", "%d, buffer, %zu", fd_src, sizeof(buffer));
        bytes_read = read(fd_src, buffer, sizeof(buffer));
        if (bytes_read < 0) {
            LOG_SYSCALL_ERROR(strerror(errno));
            break;
        }
        LOG_SYSCALL_RESULT(bytes_read);
        if (bytes_read == 0) break; /* Fin de Archivo (EOF) alcanzado */

        /* Escribir en el destino */
        LOG_SYSCALL("write", "%d, buffer, %zd", fd_dst, bytes_read);
        bytes_written = write(fd_dst, buffer, bytes_read);
        if (bytes_written < 0) {
            LOG_SYSCALL_ERROR(strerror(errno));
            break;
        }
        LOG_SYSCALL_RESULT(bytes_written);
        total_copied += bytes_written;
    }

    /* 4. Cerrar descriptores */
    LOG_SYSCALL("close", "%d", fd_src);
    int res = close(fd_src);
    if (res == -1) LOG_SYSCALL_ERROR(strerror(errno));
    else LOG_SYSCALL_RESULT(res);

    LOG_SYSCALL("close", "%d", fd_dst);
    res = close(fd_dst);
    if (res == -1) LOG_SYSCALL_ERROR(strerror(errno));
    else LOG_SYSCALL_RESULT(res);

    printf(COLOR_RESULT "Copiado finalizado. Total de bytes copiados: %zu\n" COLOR_RESET, total_copied);
    return 0;
}

/**
 * ====================================================================================
 * COMANDO: Zip <archivo>
 * ====================================================================================
 * Comprime un archivo existente generando un nuevo archivo con extensión .zip.
 *
 * Explicación teórica:
 * El kernel de Linux NO ofrece una llamada al sistema que comprima archivos: la
 * compresión es trabajo de un programa de espacio de usuario (en este caso el binario
 * 'zip'). Por eso el shell hace lo mismo que haría bash: se clona con fork(2) y, en el
 * proceso hijo, reemplaza su imagen de proceso con el compresor usando execvp(3).
 * Mientras tanto, el padre se bloquea en waitpid(2) hasta conocer el código de salida.
 *
 * Syscalls explicadas:
 * 1. stat(2): Verifica que el archivo de origen exista y guarda su tamaño original,
 *    para poder comparar después cuánto espacio se ahorró con la compresión.
 * 2. fork(2): Clona el shell. Retorna 0 en el hijo y el PID del hijo en el padre.
 * 3. execvp(3) (envoltorio de execve(2)): Carga el binario 'zip' sobre el hijo.
 *    Busca el ejecutable recorriendo los directorios de la variable de entorno PATH.
 *    Si tiene éxito nunca retorna; si retorna -1, el comando no pudo ejecutarse.
 * 4. waitpid(2): Suspende al padre hasta que el compresor termine y decodifica su
 *    estado con las macros WIFEXITED/WEXITSTATUS.
 * 5. stat(2): Se repite sobre el .zip resultante para mostrar el tamaño final.
 */
int cmd_comprimir(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, COLOR_ERROR "Uso: Zip <archivo>\n" COLOR_RESET);
        return 1;
    }
    const char *filename = argv[1];
    char zipname[1024];

    /* 1. LLAMADA AL SISTEMA: stat (comprobar que el origen existe y medir su tamaño) */
    struct stat st_src;
    LOG_SYSCALL("stat", "\"%s\", &st_src", filename);
    if (stat(filename, &st_src) == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        fprintf(stderr, COLOR_ERROR "No se puede comprimir '%s': el archivo no existe.\n" COLOR_RESET, filename);
        return 1;
    }
    LOG_SYSCALL_RESULT(0);

    /* 2. Construir el nombre de salida: se reemplaza la extensión del archivo por .zip
     * (por ejemplo notas.txt -> notas.zip). Si el nombre no tiene extensión, o ya es
     * un .zip, simplemente se le añade el sufijo (respaldo.zip -> respaldo.zip.zip),
     * para nunca sobrescribir el archivo de origen. */
    size_t len = strlen(filename);
    if (len + 5 >= sizeof(zipname)) {
        fprintf(stderr, COLOR_ERROR "El nombre '%s' es demasiado largo.\n" COLOR_RESET, filename);
        return 1;
    }
    strcpy(zipname, filename);

    char *slash = strrchr(zipname, '/');          /* Inicio del nombre, ignorando directorios */
    char *base = (slash != NULL) ? slash + 1 : zipname;
    char *dot = strrchr(base, '.');               /* Última extensión del nombre base */
    if (dot != NULL && dot != base && strcmp(dot, ".zip") != 0) {
        *dot = '\0';                              /* Recorta la extensión original */
    }
    strcat(zipname, ".zip");

    printf(COLOR_INFO "Comprimiendo '%s' en '%s' con el programa externo 'zip'...\n" COLOR_RESET, filename, zipname);

    /* 3. LLAMADA AL SISTEMA: fork
     * Antes de clonar se vacía el búfer de stdout: fork(2) duplica también el búfer
     * de la biblioteca estándar, y si quedara texto pendiente el hijo lo volvería a
     * imprimir al terminar, mostrando mensajes repetidos. */
    fflush(stdout);
    LOG_SYSCALL("fork", "");
    pid_t pid = fork();
    if (pid == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        return 1;
    }

    if (pid == 0) {
        /* PROCESO HIJO: se convierte en el compresor */
        char *exec_args[] = { "zip", zipname, (char *)filename, NULL };

        /* 4. LLAMADA AL SISTEMA: execvp */
        LOG_SYSCALL("execvp", "\"zip\", {\"zip\", \"%s\", \"%s\"}", zipname, filename);
        printf("\n");
        execvp(exec_args[0], exec_args);

        /* Solo se llega aquí si execvp falló (por ejemplo, 'zip' no está instalado) */
        LOG_SYSCALL_ERROR(strerror(errno));
        fprintf(stderr, COLOR_ERROR "Error: No se pudo ejecutar 'zip'. ¿Está instalado? (sudo apt install zip)\n" COLOR_RESET);
        exit(127); /* Código estándar de shell para "comando no encontrado" */
    }

    /* PROCESO PADRE: espera el resultado de la compresión */
    LOG_SYSCALL_RESULT(pid);

    int status;
    /* 5. LLAMADA AL SISTEMA: waitpid */
    LOG_SYSCALL("waitpid", "%d, &status, 0", pid);
    pid_t waited_pid = waitpid(pid, &status, 0);
    if (waited_pid == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        return 1;
    }
    LOG_SYSCALL_RESULT(waited_pid);

    if (WIFSIGNALED(status)) {
        fprintf(stderr, COLOR_ERROR "La compresión fue interrumpida por la señal %d.\n" COLOR_RESET, WTERMSIG(status));
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, COLOR_ERROR "El compresor terminó con error (código %d). No se generó '%s'.\n" COLOR_RESET,
                WIFEXITED(status) ? WEXITSTATUS(status) : -1, zipname);
        return 1;
    }

    /* 6. LLAMADA AL SISTEMA: stat sobre el .zip creado, para comparar tamaños */
    struct stat st_zip;
    LOG_SYSCALL("stat", "\"%s\", &st_zip", zipname);
    if (stat(zipname, &st_zip) == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        return 1;
    }
    LOG_SYSCALL_RESULT(0);

    printf(COLOR_TITLE "--- Resultado de la Compresión ---\n" COLOR_RESET);
    printf("  Archivo original: " COLOR_PARAM "%s" COLOR_RESET " (%ld bytes)\n", filename, (long)st_src.st_size);
    printf("  Archivo .zip:     " COLOR_RESULT "%s" COLOR_RESET " (%ld bytes)\n", zipname, (long)st_zip.st_size);
    if (st_src.st_size > 0) {
        double ahorro = 100.0 - ((double)st_zip.st_size * 100.0 / (double)st_src.st_size);
        printf("  Variación:        " COLOR_PARAM "%.1f%%" COLOR_RESET " respecto al original\n", ahorro);
    }
    printf(COLOR_TITLE "----------------------------------\n" COLOR_RESET);

    return 0;
}
