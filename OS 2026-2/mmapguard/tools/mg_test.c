// SPDX-License-Identifier: GPL-2.0
/*
 * mg_test - Cliente de prueba y banco de ataques contra mmapguard.ko
 *
 * Ejecuta en un solo programa:
 *   A1  Lectura directa del archivo cifrado      -> debe devolver basura
 *   --  Lectura via mapeo protegido              -> debe devolver texto claro
 *   A2  mmap con PROT_WRITE                      -> debe ser denegado
 *   A3  mmap PROT_READ + mprotect(PROT_WRITE)    -> debe ser denegado
 *
 * Compilar: gcc -Wall -Wextra -O2 -o mg_test mg_test.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "../src/mmapguard_uapi.h"

#define DEV "/dev/mmapguard"

#define C_RESET "\033[0m"
#define C_BOLD  "\033[1m"
#define C_RED   "\033[0;31m"
#define C_GREEN "\033[0;32m"
#define C_YELL  "\033[1;33m"
#define C_CYAN  "\033[0;36m"

static void hexdump_ascii(const unsigned char *p, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		putchar((p[i] >= 32 && p[i] < 127) ? p[i] : '.');
	putchar('\n');
}

int main(int argc, char **argv)
{
	struct mg_attach_req req;
	struct mg_stats st;
	struct stat sb;
	unsigned char raw[64];
	char *map;
	int fd, ffd, rc;
	size_t maplen;

	if (argc != 2) {
		fprintf(stderr, "Uso: %s <ruta_archivo_cifrado>\n", argv[0]);
		return EXIT_FAILURE;
	}

	if (stat(argv[1], &sb) != 0) {
		perror("stat");
		return EXIT_FAILURE;
	}
	maplen = (size_t)sb.st_size;

	printf("%s%s=== mmapguard: prueba del plano de datos ===%s\n\n",
	       C_BOLD, C_CYAN, C_RESET);

	/* ---- A1: lectura directa del archivo cifrado ---- */
	printf("%s[A1] Lectura DIRECTA del archivo (sin el driver):%s\n",
	       C_YELL, C_RESET);
	ffd = open(argv[1], O_RDONLY);
	if (ffd < 0) {
		perror("open archivo");
		return EXIT_FAILURE;
	}
	if (read(ffd, raw, sizeof(raw)) < 0) {
		perror("read");
		close(ffd);
		return EXIT_FAILURE;
	}
	close(ffd);
	printf("     ");
	hexdump_ascii(raw, sizeof(raw));
	printf("     %s-> ilegible: el archivo esta cifrado en reposo.%s\n\n",
	       C_GREEN, C_RESET);

	/* ---- Asociacion con el driver ---- */
	fd = open(DEV, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "%sNo se pudo abrir %s: %s%s\n",
			C_RED, DEV, strerror(errno), C_RESET);
		fprintf(stderr, "Esta cargado el modulo? Tiene permisos el dispositivo?\n");
		return EXIT_FAILURE;
	}

	memset(&req, 0, sizeof(req));
	strncpy(req.path, argv[1], MG_PATH_MAX - 1);
	if (ioctl(fd, MG_IOC_ATTACH, &req) != 0) {
		fprintf(stderr, "%sMG_IOC_ATTACH fallo: %s%s\n",
			C_RED, strerror(errno), C_RESET);
		close(fd);
		return EXIT_FAILURE;
	}
	printf("%s[+] Archivo asociado al driver.%s\n\n", C_GREEN, C_RESET);

	/* ---- Lectura a traves del mapeo protegido ---- */
	printf("%s[  ] Lectura a traves del MAPEO PROTEGIDO:%s\n", C_YELL, C_RESET);
	map = mmap(NULL, maplen, PROT_READ, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED) {
		fprintf(stderr, "%smmap PROT_READ fallo: %s%s\n",
			C_RED, strerror(errno), C_RESET);
		close(fd);
		return EXIT_FAILURE;
	}
	printf("     ");
	hexdump_ascii((unsigned char *)map, sizeof(raw));
	printf("     %s-> legible: descifrado pagina a pagina en el fallo.%s\n\n",
	       C_GREEN, C_RESET);

	/* ---- A3: escalada con mprotect ---- */
	printf("%s[A3] Intento de escalada con mprotect(PROT_WRITE):%s\n",
	       C_YELL, C_RESET);
	rc = mprotect(map, maplen, PROT_READ | PROT_WRITE);
	if (rc == 0) {
		printf("     %sFALLO DE SEGURIDAD: la escalada fue permitida.%s\n\n",
		       C_RED, C_RESET);
	} else {
		printf("     %s-> DENEGADO (%s): VM_MAYWRITE fue retirado en el mmap.%s\n\n",
		       C_GREEN, strerror(errno), C_RESET);
	}

	munmap(map, maplen);

	/* ---- A2: mapeo escribible directo ---- */
	printf("%s[A2] Intento de mmap con PROT_WRITE:%s\n", C_YELL, C_RESET);
	map = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (map != MAP_FAILED) {
		printf("     %sFALLO DE SEGURIDAD: se concedio un mapeo escribible.%s\n\n",
		       C_RED, C_RESET);
		munmap(map, maplen);
	} else {
		printf("     %s-> DENEGADO (%s): politica de solo lectura.%s\n\n",
		       C_GREEN, strerror(errno), C_RESET);
	}

	/* ---- Contadores de auditoria ---- */
	if (ioctl(fd, MG_IOC_STATS, &st) == 0) {
		printf("%s%s--- Auditoria del driver ---%s\n", C_BOLD, C_CYAN, C_RESET);
		printf("  Tamano del archivo    : %llu bytes\n",
		       (unsigned long long)st.file_size);
		printf("  Paginas descifradas   : %llu\n",
		       (unsigned long long)st.faults);
		printf("  Bytes descifrados     : %llu\n",
		       (unsigned long long)st.bytes_decrypted);
		printf("  Mapeos denegados      : %llu\n",
		       (unsigned long long)st.denied_mmap);
		printf("  Escrituras denegadas  : %llu\n",
		       (unsigned long long)st.denied_write);
	}

	close(fd);
	return EXIT_SUCCESS;
}
