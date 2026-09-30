#include "shell.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>

#define MAX_ARGS 64
#define MAX_LINE 2048

/**
 * ====================================================================================
 * TABLA GLOBAL DE REGISTRO DE COMANDOS
 * ====================================================================================
 * Almacena los punteros a funciones e información descriptiva de cada comando educativo.
 * Cada elemento de la tabla inicializa una estructura Command declarada en shell.h.
 */
Command commands[] = {
    /* --- Categoría: Datos --- */
    {
        "Open", "datos",
        "Open <archivo>",
        "Crea un archivo no existe.",
        "open(2), close(2)",
        cmd_o_open
    },
    {
        "Print", "datos",
        "Print <archivo>",
        "Lee e imprime el contenido de un archivo.",
        "open(2), read(2), close(2)",
        cmd_p_print
    },
    {
        "Add", "datos",
        "Add <archivo> \"<texto>\"",
        "Agrega texto en una nueva linea de un archivo sin borrar su contenido previo.",
        "open(2), write(2), close(2)",
        cmd_add_append
    },
    {
        "Delete", "datos",
        "Delete <archivo> [numero_de_linea]",
        "Sin numero de linea borra el archivo completo; con numero de linea, borra esa linea y desplaza el resto.",
        "unlink(2), o stat(2)+open(2)+read(2)+write(2)+close(2)",
        cmd_delete
    },
    {
        "d_info", "datos",
        "d_info <archivo>", 
        "Muestra metadatos detallados de un archivo.",
        "stat(2)", 
        cmd_d_info
    },
    {
        "d_copy", "datos", 
        "d_copy <origen> <destino>", 
        "Copia recursiva o lineal de bytes entre archivos.",
        "open(2), read(2), write(2), close(2)", 
        cmd_d_copy
    },

    /* --- Categoría: Memoria --- */
    {
        "m_sbrk", "memoria", 
        "m_sbrk <incremento_bytes>", 
        "Modifica el program break de la sección heap.",
        "sbrk(2) / brk(2)", 
        cmd_m_sbrk
    },
    {
        "m_mmap", "memoria", 
        "m_mmap <tamaño_bytes>", 
        "Mapea una zona de memoria anónima y escribe un patrón.",
        "mmap(2), munmap(2)", 
        cmd_m_mmap
    },
    {
        "m_info", "memoria", 
        "m_info", 
        "Muestra el estado del mapa de memoria del proceso actual.",
        "Lectura directa de /proc/self/status", 
        cmd_m_info
    },

    /* --- Categoría: Monitoreo/Procesos --- */
    {
        "p_fork", "monitoreo", 
        "p_fork", 
        "Crea un proceso hijo, demuestra sincronización y códigos de salida.",
        "fork(2), getpid(2), getppid(2), waitpid(2)", 
        cmd_p_fork
    },
    {
        "p_exec", "monitoreo", 
        "p_exec <comando> [argumentos...]", 
        "Crea un proceso hijo y ejecuta un comando externo del sistema.",
        "fork(2), execvp(3), waitpid(2)", 
        cmd_p_exec
    },
    {
        "p_kill", "monitoreo", 
        "p_kill <pid> <numero_señal>", 
        "Envía una señal específica a un proceso en ejecución.",
        "kill(2)", 
        cmd_p_kill
    },
    {
        "p_monitor", "monitoreo", 
        "p_monitor", 
        "Muestra el uso detallado de recursos de la CPU y memoria del shell.",
        "getrusage(2)", 
        cmd_p_monitor
    },

    /* --- Categoría: Utilidades --- */
    {
        "saludar", "utilidades",
        "saludar",
        "Muestra un saludo personalizado para el usuario actual.",
        "getuid(2)",
        cmd_saludar
    },
    {
        "hora", "utilidades",
        "hora",
        "Muestra la hora actual del sistema.",
        "time(2)",
        cmd_hora
    },
    {
        "fecha", "utilidades",
        "fecha",
        "Muestra la fecha actual del sistema.",
        "time(2)",
        cmd_fecha
    }
};

/* Número total de comandos en el shell */
const int num_commands = sizeof(commands) / sizeof(commands[0]);

/**
 * ====================================================================================
 * ANALIZADOR DE LÍNEA DE COMANDOS (TOKENIZADOR)
 * ====================================================================================
 * Esta función toma la línea de entrada introducida por el usuario y la divide en 
 * argumentos individuales. Soporta comillas dobles (") para permitir argumentos
 * que contienen espacios en blanco, como en: d_create archivo.txt "Este es el texto"
 *
 * Parámetros:
 * - line: La línea cruda leída del teclado. Modificada in-situ colocando caracteres nulos (\0).
 * - argv: Array de punteros que se llenará apuntando al inicio de cada argumento.
 *
 * Retorna:
 * - El número de argumentos (argc) detectados.
 */
int parse_line(char *line, char **argv) {
    int argc = 0;
    char *p = line;
    int in_quote = 0;
    char *arg_start = NULL;

    while (*p) {
        if (!in_quote && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
            /* Espacio en blanco fuera de comillas: Terminar el token actual */
            if (arg_start != NULL) {
                *p = '\0'; /* Inserta carácter de fin de cadena */
                argv[argc++] = arg_start;
                arg_start = NULL;
            }
        } else if (*p == '"') {
            /* Cambiar el estado de lectura de comillas */
            if (in_quote) {
                /* Fin del bloque entre comillas */
                *p = '\0';
                argv[argc++] = arg_start;
                arg_start = NULL;
                in_quote = 0;
            } else {
                /* Inicio del bloque entre comillas */
                in_quote = 1;
                arg_start = p + 1;
            }
        } else {
            /* Carácter ordinario del argumento */
            if (arg_start == NULL) {
                arg_start = p;
            }
        }
        p++;
    }
    /* Añadir el último argumento si quedó pendiente */
    if (arg_start != NULL) {
        argv[argc++] = arg_start;
    }
    argv[argc] = NULL; /* Convención POSIX de terminar argv con NULL */
    return argc;
}

/**
 * ====================================================================================
 * FUNCIÓN DE AYUDA INTERACTIVA (HELP)
 * ====================================================================================
 * Muestra información pedagógica general del shell, por categoría o por comando individual.
 */
void print_help(const char *arg) {
    if (arg == NULL) {
        /* Caso 1: Escribió 'help' solo: Mostrar categorías principales */
        printf(COLOR_TITLE "\n--- Shell de Aprendizaje de Syscalls (SO2026B) ---\n" COLOR_RESET);
        printf("Este shell te permite explorar cómo funcionan las llamadas al sistema en Linux.\n");
        printf("Los comandos están clasificados en categorías.\n\n");
        printf("Categorías disponibles:\n");
        printf("  " COLOR_CATEGORY "datos" COLOR_RESET "      - Comandos de archivos y datos (open, read, write, stat, ...)\n");
        printf("  " COLOR_CATEGORY "memoria" COLOR_RESET "    - Comandos de control de heap y memoria (sbrk, mmap, ...)\n");
        printf("  " COLOR_CATEGORY "monitoreo" COLOR_RESET "  - Comandos de procesos, señales y recursos (fork, exec, kill, getrusage)\n");
        printf("  " COLOR_CATEGORY "utilidades" COLOR_RESET " - Comandos útiles del sistema (saludar, hora, fecha)\n\n");
        printf("Uso general:\n");
        printf("  " COLOR_PROMPT "help <categoria>" COLOR_RESET "  - Muestra comandos específicos de una categoría.\n");
        printf("  " COLOR_PROMPT "help <comando>" COLOR_RESET "    - Explica el uso y las syscalls de un comando específico.\n");
        printf("  " COLOR_PROMPT "clear" COLOR_RESET "             - Limpia la pantalla.\n");
        printf("  " COLOR_PROMPT "exit" COLOR_RESET "              - Cierra el shell.\n\n");
        return;
    }

    /* Caso 2: El usuario escribió 'help <categoria>': Mostrar comandos del grupo */
    if (strcmp(arg, "datos") == 0 || strcmp(arg, "memoria") == 0 || 
        strcmp(arg, "monitoreo") == 0 || strcmp(arg, "utilidades") == 0) {
        printf(COLOR_TITLE "\n--- Categoría: %s ---\n" COLOR_RESET, arg);
        for (int i = 0; i < num_commands; i++) {
            if (strcmp(commands[i].category, arg) == 0) {
                printf("  " COLOR_PROMPT "%-10s" COLOR_RESET " -> %s\n", commands[i].name, commands[i].description);
                printf("                " COLOR_INFO "Llamada(s): %s" COLOR_RESET "\n\n", commands[i].syscalls);
            }
        }
        return;
    }

    /* Caso 3: El usuario escribió 'help <comando>': Explicar syscalls individuales */
    for (int i = 0; i < num_commands; i++) {
        if (strcmp(commands[i].name, arg) == 0) {
            printf(COLOR_TITLE "\nDetalles de comando: %s\n" COLOR_RESET, commands[i].name);
            printf("  Descripción:  %s\n", commands[i].description);
            printf("  Uso:          " COLOR_PARAM "%s" COLOR_RESET "\n", commands[i].usage);
            printf("  Syscalls:     " COLOR_SYSCALL "%s" COLOR_RESET "\n\n", commands[i].syscalls);
            return;
        }
    }

    printf(COLOR_ERROR "Categoría o comando '%s' no reconocido. Escribe 'help' para ver la ayuda.\n" COLOR_RESET, arg);
}

/**
 * ====================================================================================
 * LECTOR DE LÍNEA CON RESALTADO EN VIVO (Syntax Highlighting)
 * ====================================================================================
 * Pone la terminal en modo "raw" (sin buffer de línea ni eco automático) para poder
 * leer la entrada carácter por carácter y repintar el prompt en cada tecla:
 *   - El primer token (nombre del comando) se pinta en VERDE.
 *   - El resto de la línea (argumentos) se pinta con FONDO AZUL.
 *
 * Nota: Por simplicidad pedagógica solo soporta escritura/backspace al final de la
 * línea (no hay movimiento de cursor con flechas izquierda/derecha).
 */
static struct termios g_orig_termios;

static void disable_raw_mode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
}

static void enable_raw_mode(void) {
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

/* Repinta la línea completa: limpia, imprime el prompt y colorea comando/argumentos */
static void redraw_line(const char *prompt, const char *buf, int len) {
    printf("\r\033[K"); /* Vuelve al inicio de la línea y borra hasta el final */
    printf("%s", prompt);

    /* Encuentra el final del primer token (nombre del comando) */
    int cmd_end = 0;
    while (cmd_end < len && buf[cmd_end] != ' ') {
        cmd_end++;
    }

    printf(COLOR_CMD_LIVE "%.*s" COLOR_RESET, cmd_end, buf);
    if (cmd_end < len) {
        /* A partir del primer espacio, los argumentos se imprimen sin color
         * (color por defecto de la terminal), en vez de heredar el verde del comando. */
        printf("%s", buf + cmd_end);
    }
    fflush(stdout);
}


/**
 * Lee una línea de stdin en modo raw, repintando con colores en cada tecla.
 * Retorna la longitud de la línea leída, o -1 si se recibió EOF (Ctrl+D) sin texto.
 */
static int read_line_colored(char *line, size_t size, const char *prompt) {
    int len = 0;
    line[0] = '\0';

    enable_raw_mode();
    redraw_line(prompt, line, len);

    while (1) {
        unsigned char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            continue;
        }

        if (c == '\n' || c == '\r') {
            line[len] = '\0';
            printf("\n");
            break;
        } else if (c == 127 || c == 8) {
            /* Backspace / Delete */
            if (len > 0) {
                len--;
                line[len] = '\0';
                redraw_line(prompt, line, len);
            }
        } else if (c == 3) {
            /* Ctrl+C: cancela la línea actual y empieza una nueva */
            len = 0;
            line[0] = '\0';
            printf("^C\n");
            disable_raw_mode();
            return 0;
        } else if (c == 4) {
            /* Ctrl+D: EOF solo si la línea está vacía (como en bash) */
            if (len == 0) {
                disable_raw_mode();
                printf("\n");
                return -1;
            }
        } else if (c == 27) {
            /* Secuencia de escape (flechas, etc.): la consumimos y la ignoramos */
            unsigned char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) <= 0) continue;
            if (seq[0] == '[' || seq[0] == 'O') {
                read(STDIN_FILENO, &seq[1], 1);
            }
        } else if (c >= 32 && c < 127 && (size_t)len < size - 1) {
            /* Carácter imprimible normal */
            line[len++] = (char)c;
            line[len] = '\0';
            redraw_line(prompt, line, len);
        }
    }

    disable_raw_mode();
    return len;
}

/**
 * ====================================================================================
 * BUCLE REPL PRINCIPAL (Read-Eval-Print Loop)
 * ====================================================================================
 * Controla el ciclo de vida del shell:
 * 1. Read: Lee la entrada del usuario usando fgets().
 * 2. Eval: Parse y busca si el comando coincide con built-ins o funciones registradas.
 * 3. Print: Imprime los resultados y las llamadas al sistema en consola.
 * 4. Loop: Repite el ciclo infinitamente hasta escribir 'exit' o presionar Ctrl+D.
 */
int main() {
    char line[MAX_LINE];
    char *argv[MAX_ARGS];

    /* Banner de bienvenida premium */
    printf(COLOR_TITLE "========================================================\n" COLOR_RESET);
    printf(COLOR_TITLE "    Shell Educativo de Llamadas al Sistema de Linux\n" COLOR_RESET);
    printf(COLOR_INFO "    Asignatura: SO2026B (Sistemas Operativos)\n" COLOR_RESET);
    printf(COLOR_INFO "    Escribe 'help' para iniciar. Desarrollado en C.\n" COLOR_RESET);
    printf(COLOR_TITLE "========================================================\n\n" COLOR_RESET);

    while (1) {
        /* Leer línea con resaltado en vivo: comando en verde, argumentos en fondo azul */
        int nread = read_line_colored(line, sizeof(line), COLOR_PROMPT "sys-shell> " COLOR_RESET);
        if (nread < 0) {
            /* EOF (Ctrl+D) con línea vacía */
            break;
        }

        /* Tokenizar línea leída */
        int argc = parse_line(line, argv);
        if (argc == 0) {
            continue; /* Ignorar comandos vacíos (incluye cancelación con Ctrl+C) */
        }

        /* Comandos Built-in generales */
        if (strcmp(argv[0], "exit") == 0) {
            printf(COLOR_INFO "Saliendo del shell educativo. ¡Hasta luego!\n" COLOR_RESET);
            break;
        } else if (strcmp(argv[0], "clear") == 0) {
            printf("\033[H\033[J"); /* Limpia la pantalla usando secuencias de escape ANSI */
            continue;
        } else if (strcmp(argv[0], "help") == 0) {
            if (argc > 1) {
                print_help(argv[1]);
            } else {
                print_help(NULL);
            }
            continue;
        }

        /* Enrutar la ejecución buscando en la lista de comandos registrados */
        int found = 0;
        for (int i = 0; i < num_commands; i++) {
            if (strcmp(argv[0], commands[i].name) == 0) {
                commands[i].handler(argc, argv);
                found = 1;
                break;
            }
        }

        /* Si no se encuentra en el registro educativo del shell */
        if (!found) {
            printf(COLOR_ERROR "Comando '%s' no encontrado en el shell educativo.\n" COLOR_RESET, argv[0]);
            printf(COLOR_INFO "Prueba usando 'p_exec %s' si quieres ejecutarlo como un binario de Linux externo, o escribe 'help'.\n" COLOR_RESET, argv[0]);
        }
        printf("\n");
    }

    return 0;
}
