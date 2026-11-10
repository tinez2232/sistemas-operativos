/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mmapguard_uapi.h - Interfaz compartida entre el modulo de kernel y el
 *                    espacio de usuario.
 *
 * Parcial 25% - Seguridad de Archivos en Capa Kernel y mmap.
 */
#ifndef _MMAPGUARD_UAPI_H
#define _MMAPGUARD_UAPI_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define MG_DEVICE_NAME  "mmapguard"
#define MG_PATH_MAX     256

/*
 * Peticion de asociacion: indica al driver que archivo cifrado debe exponer
 * de forma protegida a traves del mapeo.
 */
struct mg_attach_req {
	char path[MG_PATH_MAX];   /* ruta absoluta del archivo cifrado */
	__u64 flags;              /* reservado para extensiones futuras */
};

/* Contadores de auditoria expuestos al espacio de usuario. */
struct mg_stats {
	__u64 file_size;          /* tamano del archivo asociado        */
	__u64 faults;             /* paginas materializadas por fallo   */
	__u64 bytes_decrypted;    /* bytes descifrados en total         */
	__u64 denied_mmap;        /* mapeos rechazados por politica     */
	__u64 denied_write;       /* escrituras rechazadas              */
};

#define MG_IOC_MAGIC    'G'
#define MG_IOC_ATTACH   _IOW(MG_IOC_MAGIC, 1, struct mg_attach_req)
#define MG_IOC_STATS    _IOR(MG_IOC_MAGIC, 2, struct mg_stats)

#endif /* _MMAPGUARD_UAPI_H */
