// SPDX-License-Identifier: GPL-2.0
/*
 * mg_bench - Medicion del sobrecosto introducido por mmapguard.
 *
 * Compara tres formas de acceder al MISMO contenido:
 *   A) read() en bloques de 4 KB sobre el archivo en claro
 *   B) mmap() ordinario sobre el archivo en claro
 *   C) mapeo protegido de mmapguard sobre el archivo cifrado
 *
 * Metricas por getrusage(): CPU en usuario, CPU en kernel, fallos de pagina
 * menores y mayores, mas tiempo de pared y latencia de la propia llamada mmap().
 *
 * NOTA METODOLOGICA IMPORTANTE
 * ----------------------------
 * El checksum de cada metodo SE IMPRIME. Esto no es decorativo: si el resultado
 * del bucle de lectura no se usa, GCC con -O2 elimina el bucle entero por
 * codigo muerto y el metodo parece infinitamente rapido. Ese defecto existe en
 * el benchmark del paso 5 del repositorio de referencia, cuyo bucle de mmap se
 * descarta en la compilacion y produce cifras de throughput imposibles.
 *
 * Compilar: gcc -Wall -Wextra -O2 -o mg_bench mg_bench.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>

#include "../src/mmapguard_uapi.h"

#define DEV      "/dev/mmapguard"
#define PAGE_SZ  4096UL

#define C_RESET "\033[0m"
#define C_BOLD  "\033[1m"
#define C_GREEN "\033[0;32m"
#define C_YELL  "\033[1;33m"
#define C_CYAN  "\033[0;36m"

struct result {
	const char *name;
	double wall;
	double user_cpu;
	double sys_cpu;
	long   minflt;
	long   majflt;
	double mmap_us;        /* latencia de la llamada mmap(), microsegundos */
	unsigned long long checksum;
	unsigned long pages_touched;
};

static double now_sec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static double tv2sec(struct timeval tv)
{
	return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

/* Recorre el buffer tocando una pagina de cada 'stride'. */
static unsigned long long touch_pages(const volatile unsigned char *p,
				      size_t len, unsigned long stride,
				      unsigned long *touched)
{
	unsigned long long sum = 0;
	size_t off;
	unsigned long n = 0;

	for (off = 0; off < len; off += PAGE_SZ * stride) {
		sum += p[off];
		n++;
	}
	*touched = n;
	return sum;
}

static void bench_read(const char *path, size_t len, unsigned long stride,
		       struct result *r)
{
	struct rusage r0, r1;
	unsigned char buf[PAGE_SZ];
	unsigned long long sum = 0;
	unsigned long n = 0;
	double t0, t1;
	off_t off;
	int fd;

	r->name = "read() 4 KB";
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("open");
		exit(EXIT_FAILURE);
	}

	getrusage(RUSAGE_SELF, &r0);
	t0 = now_sec();

	for (off = 0; (size_t)off < len; off += (off_t)(PAGE_SZ * stride)) {
		if (pread(fd, buf, PAGE_SZ, off) < 0)
			break;
		sum += buf[0];
		n++;
	}

	t1 = now_sec();
	getrusage(RUSAGE_SELF, &r1);
	close(fd);

	r->wall = t1 - t0;
	r->user_cpu = tv2sec(r1.ru_utime) - tv2sec(r0.ru_utime);
	r->sys_cpu  = tv2sec(r1.ru_stime) - tv2sec(r0.ru_stime);
	r->minflt = r1.ru_minflt - r0.ru_minflt;
	r->majflt = r1.ru_majflt - r0.ru_majflt;
	r->mmap_us = 0.0;
	r->checksum = sum;
	r->pages_touched = n;
}

static void bench_mmap_plain(const char *path, size_t len, unsigned long stride,
			     struct result *r)
{
	struct rusage r0, r1;
	double t0, t1, m0, m1;
	unsigned char *map;
	int fd;

	r->name = "mmap() normal";
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("open");
		exit(EXIT_FAILURE);
	}

	getrusage(RUSAGE_SELF, &r0);
	t0 = now_sec();

	m0 = now_sec();
	map = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
	m1 = now_sec();
	if (map == MAP_FAILED) {
		perror("mmap");
		exit(EXIT_FAILURE);
	}

	r->checksum = touch_pages(map, len, stride, &r->pages_touched);

	t1 = now_sec();
	getrusage(RUSAGE_SELF, &r1);
	munmap(map, len);
	close(fd);

	r->wall = t1 - t0;
	r->user_cpu = tv2sec(r1.ru_utime) - tv2sec(r0.ru_utime);
	r->sys_cpu  = tv2sec(r1.ru_stime) - tv2sec(r0.ru_stime);
	r->minflt = r1.ru_minflt - r0.ru_minflt;
	r->majflt = r1.ru_majflt - r0.ru_majflt;
	r->mmap_us = (m1 - m0) * 1e6;
}

static void bench_mmapguard(const char *encpath, size_t len,
			    unsigned long stride, struct result *r,
			    struct mg_stats *st_out)
{
	struct rusage r0, r1;
	struct mg_attach_req req;
	double t0, t1, m0, m1;
	unsigned char *map;
	int fd;

	r->name = "mmapguard";
	fd = open(DEV, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "No se pudo abrir %s: %s\n", DEV, strerror(errno));
		fprintf(stderr, "Cargue el modulo y de permisos al dispositivo.\n");
		exit(EXIT_FAILURE);
	}

	memset(&req, 0, sizeof(req));
	strncpy(req.path, encpath, MG_PATH_MAX - 1);
	if (ioctl(fd, MG_IOC_ATTACH, &req) != 0) {
		fprintf(stderr, "MG_IOC_ATTACH fallo: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	getrusage(RUSAGE_SELF, &r0);
	t0 = now_sec();

	m0 = now_sec();
	map = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
	m1 = now_sec();
	if (map == MAP_FAILED) {
		perror("mmap mmapguard");
		exit(EXIT_FAILURE);
	}

	r->checksum = touch_pages(map, len, stride, &r->pages_touched);

	t1 = now_sec();
	getrusage(RUSAGE_SELF, &r1);

	if (ioctl(fd, MG_IOC_STATS, st_out) != 0)
		memset(st_out, 0, sizeof(*st_out));

	munmap(map, len);
	close(fd);

	r->wall = t1 - t0;
	r->user_cpu = tv2sec(r1.ru_utime) - tv2sec(r0.ru_utime);
	r->sys_cpu  = tv2sec(r1.ru_stime) - tv2sec(r0.ru_stime);
	r->minflt = r1.ru_minflt - r0.ru_minflt;
	r->majflt = r1.ru_majflt - r0.ru_majflt;
	r->mmap_us = (m1 - m0) * 1e6;
}

static void warm_cache(const char *path)
{
	unsigned char buf[65536];
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return;
	while (read(fd, buf, sizeof(buf)) > 0)
		;
	close(fd);
}


static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x > y) - (x < y);
}

static double median(double *v, int n)
{
	qsort(v, (size_t)n, sizeof(double), cmp_double);
	return (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

static void print_row(const struct result *r)
{
	printf("| %-14s | %10.5f | %10.5f | %10.5f | %8ld | %7ld | %9.1f |\n",
	       r->name, r->wall, r->user_cpu, r->sys_cpu,
	       r->minflt, r->majflt, r->mmap_us);
}

int main(int argc, char **argv)
{
	struct result a, b, c;
	struct mg_stats st;
	struct stat sb;
	unsigned long stride = 1;
	size_t len;

	if (argc < 3 || argc > 4) {
		fprintf(stderr,
			"Uso: %s <archivo_claro> <archivo_cifrado> [stride_paginas]\n",
			argv[0]);
		return EXIT_FAILURE;
	}
	if (argc == 4)
		stride = strtoul(argv[3], NULL, 10);
	if (stride == 0)
		stride = 1;

	if (stat(argv[1], &sb) != 0) {
		perror("stat");
		return EXIT_FAILURE;
	}
	len = (size_t)sb.st_size;

	printf("%s%s=== Sobrecosto de mmapguard ===%s\n", C_BOLD, C_CYAN, C_RESET);
	printf("Archivo : %zu bytes (%zu paginas)\n", len, (len + PAGE_SZ - 1) / PAGE_SZ);
	printf("Patron  : 1 pagina de cada %lu%s\n\n", stride,
	       stride == 1 ? " (acceso secuencial completo)" : " (acceso disperso)");

	/* Cache caliente en los tres casos: medimos el mecanismo, no el disco. */
	warm_cache(argv[1]);
	warm_cache(argv[2]);

	{
		/*
		 * Repeticiones: una sola muestra de la latencia de mmap() esta
		 * dominada por el ruido del planificador. Se reporta la mediana.
		 */
		const int REPS = 11;
		double wa[16], wb[16], wc[16], ma[16], mb[16], mc[16];
		int i;

		for (i = 0; i < REPS; i++) {
			bench_read(argv[1], len, stride, &a);
			bench_mmap_plain(argv[1], len, stride, &b);
			bench_mmapguard(argv[2], len, stride, &c, &st);
			wa[i] = a.wall;  wb[i] = b.wall;  wc[i] = c.wall;
			ma[i] = a.mmap_us; mb[i] = b.mmap_us; mc[i] = c.mmap_us;
		}
		a.wall = median(wa, REPS); a.mmap_us = median(ma, REPS);
		b.wall = median(wb, REPS); b.mmap_us = median(mb, REPS);
		c.wall = median(wc, REPS); c.mmap_us = median(mc, REPS);
		printf("Muestras: %d por metodo; se reporta la MEDIANA.\n\n", REPS);
	}

	printf("| %-14s | %10s | %10s | %10s | %8s | %7s | %9s |\n",
	       "Metodo", "Pared (s)", "CPU usr", "CPU sys", "minflt", "majflt", "mmap(us)");
	printf("|----------------|------------|------------|------------|----------|---------|-----------|\n");
	print_row(&a);
	print_row(&b);
	print_row(&c);

	printf("\nPaginas tocadas por el cliente : %lu\n", c.pages_touched);
	printf("Paginas descifradas por el driver: %llu%s\n",
	       (unsigned long long)st.faults,
	       st.faults == c.pages_touched ? "  <- coincide: cero trabajo de mas" : "");
	printf("Bytes descifrados                : %llu de %zu totales (%.1f%%)\n",
	       (unsigned long long)st.bytes_decrypted, len,
	       100.0 * (double)st.bytes_decrypted / (double)len);

	/* Impresos a proposito: impiden que -O2 elimine los bucles de lectura. */
	printf("\nChecksums (read / mmap / mmapguard): %llu / %llu / %llu%s\n",
	       a.checksum, b.checksum, c.checksum,
	       (a.checksum == b.checksum && b.checksum == c.checksum)
		       ? "  [identicos: el descifrado es correcto]"
		       : "  [DIFIEREN]");

	if (b.wall > 0.0)
		printf("%sSobrecosto de mmapguard frente a mmap normal: %.2fx%s\n",
		       C_YELL, c.wall / b.wall, C_RESET);
	if (c.mmap_us > 0.0 && b.mmap_us > 0.0)
		printf("%sLatencia de la llamada mmap(): %.1f us vs %.1f us%s\n",
		       C_GREEN, c.mmap_us, b.mmap_us, C_RESET);

	return EXIT_SUCCESS;
}
