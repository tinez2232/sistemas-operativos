// SPDX-License-Identifier: GPL-2.0
/*
 * mg_encrypt - Cifra un archivo con el MISMO esquema que espera mmapguard.ko
 *
 * Esquema: AES-128-CTR, con el bloque contador derivado del indice de pagina:
 *   IV[0..7]  = 0
 *   IV[8..15] = big-endian( indice_de_pagina * (PAGE_SIZE / 16) )
 *
 * Cada pagina de 4 KB se cifra de forma independiente, que es lo que permite
 * al driver descifrar una sola pagina en el manejador de fallos sin tener que
 * procesar el archivo completo.
 *
 * Compilar: gcc -Wall -Wextra -O2 -o mg_encrypt mg_encrypt.c -lcrypto
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <openssl/evp.h>

#define MG_PAGE_SIZE 4096
#define MG_BLOCK     16

static void build_iv(unsigned char iv[16], unsigned long pgoff)
{
	unsigned long long block = (unsigned long long)pgoff * (MG_PAGE_SIZE / MG_BLOCK);
	int i;

	memset(iv, 0, 16);
	for (i = 0; i < 8; i++)
		iv[15 - i] = (unsigned char)((block >> (8 * i)) & 0xFF);
}

static int parse_key(const char *hex, unsigned char *key)
{
	int i;

	if (strlen(hex) != 32)
		return -1;
	for (i = 0; i < 16; i++) {
		unsigned int b;

		if (sscanf(hex + i * 2, "%2x", &b) != 1)
			return -1;
		key[i] = (unsigned char)b;
	}
	return 0;
}

int main(int argc, char **argv)
{
	unsigned char key[16], iv[16];
	unsigned char inbuf[MG_PAGE_SIZE], outbuf[MG_PAGE_SIZE];
	FILE *fin, *fout;
	unsigned long pgoff = 0;
	size_t n;
	int rc = EXIT_FAILURE;

	if (argc != 4) {
		fprintf(stderr,
			"Uso: %s <entrada_clara> <salida_cifrada> <clave_hex_32>\n",
			argv[0]);
		return EXIT_FAILURE;
	}

	if (parse_key(argv[3], key) != 0) {
		fprintf(stderr, "Error: la clave debe tener 32 caracteres hex.\n");
		return EXIT_FAILURE;
	}

	fin = fopen(argv[1], "rb");
	if (!fin) {
		perror("fopen entrada");
		return EXIT_FAILURE;
	}
	fout = fopen(argv[2], "wb");
	if (!fout) {
		perror("fopen salida");
		fclose(fin);
		return EXIT_FAILURE;
	}

	while ((n = fread(inbuf, 1, MG_PAGE_SIZE, fin)) > 0) {
		EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
		int outl = 0, finl = 0;

		if (!ctx)
			goto out;

		build_iv(iv, pgoff);

		if (EVP_EncryptInit_ex(ctx, EVP_aes_128_ctr(), NULL, key, iv) != 1 ||
		    EVP_EncryptUpdate(ctx, outbuf, &outl, inbuf, (int)n) != 1 ||
		    EVP_EncryptFinal_ex(ctx, outbuf + outl, &finl) != 1) {
			EVP_CIPHER_CTX_free(ctx);
			fprintf(stderr, "Error de cifrado en la pagina %lu\n", pgoff);
			goto out;
		}
		EVP_CIPHER_CTX_free(ctx);

		if (fwrite(outbuf, 1, (size_t)(outl + finl), fout) != (size_t)(outl + finl)) {
			perror("fwrite");
			goto out;
		}
		pgoff++;
	}

	printf("[+] Cifradas %lu paginas hacia '%s'\n", pgoff, argv[2]);
	rc = EXIT_SUCCESS;
out:
	fclose(fin);
	fclose(fout);
	return rc;
}
