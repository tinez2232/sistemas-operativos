// SPDX-License-Identifier: GPL-2.0
/*
 * mmapguard.c - Plano de datos: mapeo protegido con descifrado bajo demanda.
 *
 * Parcial 25% - Seguridad de Archivos mediante Intervencion en la Capa del
 *               Kernel y Mapeo de Memoria (mmap).
 *
 * ---------------------------------------------------------------------------
 * QUE HACE Y POR QUE
 * ---------------------------------------------------------------------------
 * Expone un archivo cifrado en disco como un mapeo de SOLO LECTURA, descifrando
 * UNA pagina de 4 KB cada vez, y solo cuando el proceso realmente la toca.
 *
 * La diferencia con remap_pfn_range() (paso 4 del repositorio de referencia) es
 * de seguridad, no de rendimiento: remap_pfn_range() programa todas las PTE de
 * golpe, lo que obliga a mantener el archivo COMPLETO descifrado en RAM durante
 * toda la vida del mapeo. Con vm_operations_struct->fault la ventana de
 * exposicion se reduce a las paginas efectivamente accedidas, y ademas se
 * obtiene un punto de control por pagina en lugar de uno solo en el mmap().
 *
 * ---------------------------------------------------------------------------
 * DECISION DE DISENO: SOLO LECTURA
 * ---------------------------------------------------------------------------
 * El cifrado es AES en modo CTR con el contador derivado del indice de pagina,
 * lo que permite descifrar cada pagina de forma independiente. CTR es un cifrado
 * de flujo: reutilizar clave y nonce para dos textos planos distintos revela su
 * XOR. Permitir escritura obligaria a re-cifrar cada pagina sucia con el mismo
 * nonce en cada msync(), incurriendo sistematicamente en esa falla.
 *
 * Por eso el mapeo es de solo lectura. El soporte de escritura exige un modo
 * AEAD (ChaCha20-Poly1305 o AES-GCM) con contador de version por pagina, y se
 * documenta como trabajo futuro.
 *
 * ---------------------------------------------------------------------------
 * ADVERTENCIA SOBRE LA GESTION DE CLAVES
 * ---------------------------------------------------------------------------
 * La clave se entrega como parametro del modulo con permisos 0 (no aparece en
 * sysfs). Esto es suficiente para una demostracion academica, pero NO es
 * practica aceptable en produccion: una solucion real derivaria la clave del
 * llavero del kernel (keyctl / fscrypt), nunca de la linea de insmod.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/cred.h>
#include <linux/file.h>
#include <linux/scatterlist.h>
#include <linux/highmem.h>
#include <linux/unaligned.h>
#include <crypto/skcipher.h>

#include "mmapguard_uapi.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Parcial SO 2026-2");
MODULE_DESCRIPTION("Mapeo protegido con descifrado por pagina bajo demanda");
MODULE_VERSION("0.1");

/* --------------------------------------------------------------------------
 * Parametros del modulo
 * -------------------------------------------------------------------------- */

/*
 * UID autorizado a asociar y mapear archivos protegidos.
 * -1 (valor por defecto) significa "solo root".
 */
static int authorized_uid = -1;
module_param(authorized_uid, int, 0444);
MODULE_PARM_DESC(authorized_uid, "UID autorizado; -1 = solo root");

/*
 * Clave AES-128 en hexadecimal (32 caracteres).
 * Permiso 0: deliberadamente NO visible en /sys/module/.../parameters.
 */
static char *key_hex =
	"000102030405060708090a0b0c0d0e0f";
module_param(key_hex, charp, 0);
MODULE_PARM_DESC(key_hex, "Clave AES-128 en hex (32 caracteres)");

#define MG_KEY_BYTES    16
#define MG_IV_BYTES     16
#define MG_AES_BLOCK    16

/* --------------------------------------------------------------------------
 * Estado global del driver
 * -------------------------------------------------------------------------- */

static dev_t            mg_devno;
static struct cdev      mg_cdev;
static struct class    *mg_class;
static u8               mg_key[MG_KEY_BYTES];

/*
 * Contexto por descriptor abierto. Su tiempo de vida esta ligado al
 * struct file: un VMA mantiene una referencia a vma->vm_file, de modo que la
 * sesion sobrevive aunque el proceso cierre el fd tras mapear.
 */
struct mg_session {
	struct file        *backing;      /* archivo cifrado en disco      */
	loff_t              size;         /* tamano en bytes               */
	unsigned long       npages;       /* paginas que cubre             */
	struct page       **pages;        /* cache de paginas descifradas  */
	struct crypto_sync_skcipher *tfm; /* transformacion AES-CTR        */
	struct mutex        lock;         /* protege pages[]               */
	struct mg_stats     stats;        /* contadores de auditoria       */
};

/* --------------------------------------------------------------------------
 * Utilidades
 * -------------------------------------------------------------------------- */

/* Convierte una cadena hexadecimal de 2*len caracteres en bytes. */
static int mg_parse_hex_key(const char *hex, u8 *out, size_t len)
{
	size_t i;

	if (!hex || strlen(hex) != len * 2)
		return -EINVAL;

	for (i = 0; i < len; i++) {
		int hi = hex_to_bin(hex[i * 2]);
		int lo = hex_to_bin(hex[i * 2 + 1]);

		if (hi < 0 || lo < 0)
			return -EINVAL;
		out[i] = (u8)((hi << 4) | lo);
	}
	return 0;
}

/* Comprueba si el proceso actual puede operar sobre archivos protegidos. */
static bool mg_caller_is_authorized(void)
{
	kuid_t euid = current_euid();

	if (uid_eq(euid, GLOBAL_ROOT_UID))
		return true;

	if (authorized_uid >= 0 &&
	    uid_eq(euid, make_kuid(&init_user_ns, (uid_t)authorized_uid)))
		return true;

	return false;
}

/*
 * Construye el bloque contador de AES-CTR para una pagina concreta.
 *
 * Los 8 bytes altos quedan a cero (nonce fijo) y los 8 bajos contienen, en
 * big-endian, el indice del primer bloque de 16 bytes de la pagina. Asi cada
 * pagina consume una region distinta del flujo de clave y puede descifrarse
 * de forma independiente, que es lo que hace viable la paginacion por demanda.
 */
static void mg_build_iv(u8 iv[MG_IV_BYTES], pgoff_t pgoff)
{
	u64 block = (u64)pgoff * (PAGE_SIZE / MG_AES_BLOCK);

	memset(iv, 0, MG_IV_BYTES);
	put_unaligned_be64(block, iv + 8);
}

/* Descifra en el sitio los primeros len bytes de page. */
static int mg_decrypt_page(struct mg_session *s, struct page *page,
			   unsigned int len, pgoff_t pgoff)
{
	SYNC_SKCIPHER_REQUEST_ON_STACK(req, s->tfm);
	struct scatterlist sg;
	u8 iv[MG_IV_BYTES];
	int ret;

	mg_build_iv(iv, pgoff);

	sg_init_table(&sg, 1);
	sg_set_page(&sg, page, len, 0);

	skcipher_request_set_sync_tfm(req, s->tfm);
	skcipher_request_set_callback(req, 0, NULL, NULL);
	skcipher_request_set_crypt(req, &sg, &sg, len, iv);

	/* En modo CTR cifrar y descifrar son la misma operacion. */
	ret = crypto_skcipher_decrypt(req);
	skcipher_request_zero(req);

	memzero_explicit(iv, sizeof(iv));
	return ret;
}

/* Libera todas las paginas descifradas de una sesion. */
static void mg_release_pages(struct mg_session *s)
{
	unsigned long i;

	if (!s->pages)
		return;

	for (i = 0; i < s->npages; i++) {
		if (s->pages[i]) {
			/*
			 * Borrado explicito: no dejamos texto en claro en un
			 * marco que volvera al asignador de paginas.
			 */
			memzero_explicit(page_address(s->pages[i]), PAGE_SIZE);
			__free_page(s->pages[i]);
			s->pages[i] = NULL;
		}
	}
	kvfree(s->pages);
	s->pages = NULL;
}

/* --------------------------------------------------------------------------
 * Manejadores del VMA
 * -------------------------------------------------------------------------- */

/*
 * Materializa una pagina bajo demanda.
 *
 * Este es el corazon del diseno: se invoca UNA VEZ por pagina, la primera vez
 * que el proceso la toca. Lee 4 KB de texto cifrado del archivo, los descifra
 * en un marco propio del modulo y lo entrega al proceso.
 */
static vm_fault_t mg_vma_fault(struct vm_fault *vmf)
{
	struct mg_session *s = vmf->vma->vm_private_data;
	pgoff_t pgoff = vmf->pgoff;
	struct page *page;
	loff_t pos;
	unsigned int to_read;
	void *vaddr;
	ssize_t n;
	int ret;

	if (!s)
		return VM_FAULT_SIGBUS;

	if (pgoff >= s->npages) {
		pr_warn_ratelimited("mmapguard: fallo fuera de rango (pgoff=%lu, npages=%lu)\n",
				    (unsigned long)pgoff, s->npages);
		return VM_FAULT_SIGBUS;
	}

	mutex_lock(&s->lock);

	/* Si ya fue materializada, basta con tomar una referencia. */
	if (s->pages[pgoff]) {
		page = s->pages[pgoff];
		get_page(page);
		vmf->page = page;
		mutex_unlock(&s->lock);
		return 0;
	}

	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page) {
		mutex_unlock(&s->lock);
		return VM_FAULT_OOM;
	}

	/* Cuanto texto cifrado queda realmente en esta pagina. */
	pos = (loff_t)pgoff << PAGE_SHIFT;
	to_read = PAGE_SIZE;
	if (pos + to_read > s->size)
		to_read = (unsigned int)(s->size - pos);

	vaddr = kmap_local_page(page);
	n = kernel_read(s->backing, vaddr, to_read, &pos);
	kunmap_local(vaddr);

	if (n < 0) {
		pr_warn("mmapguard: error leyendo el respaldo: %zd\n", n);
		__free_page(page);
		mutex_unlock(&s->lock);
		return VM_FAULT_SIGBUS;
	}

	if (n > 0) {
		ret = mg_decrypt_page(s, page, (unsigned int)n, pgoff);
		if (ret) {
			pr_warn("mmapguard: fallo al descifrar pagina %lu: %d\n",
				(unsigned long)pgoff, ret);
			memzero_explicit(page_address(page), PAGE_SIZE);
			__free_page(page);
			mutex_unlock(&s->lock);
			return VM_FAULT_SIGBUS;
		}
	}

	s->pages[pgoff] = page;
	s->stats.faults++;
	s->stats.bytes_decrypted += (u64)n;

	get_page(page);
	vmf->page = page;

	mutex_unlock(&s->lock);

	pr_info_ratelimited("mmapguard: pagina %lu descifrada bajo demanda (%zd bytes) por PID %d\n",
			    (unsigned long)pgoff, n, current->pid);
	return 0;
}

/*
 * Defensa en profundidad contra escritura.
 *
 * El mapeo ya se crea sin VM_MAYWRITE, de modo que el propio kernel rechaza
 * tanto PROT_WRITE en mmap() como una escalada posterior con mprotect(). Este
 * callback no deberia ejecutarse nunca; si lo hace, significa que algo burlo
 * la primera barrera, y entonces se deniega y se audita.
 */
static vm_fault_t mg_vma_page_mkwrite(struct vm_fault *vmf)
{
	struct mg_session *s = vmf->vma->vm_private_data;

	if (s)
		s->stats.denied_write++;

	pr_warn_ratelimited("mmapguard: ESCRITURA DENEGADA sobre mapeo protegido (PID %d)\n",
			    current->pid);
	return VM_FAULT_SIGBUS;
}

static const struct vm_operations_struct mg_vm_ops = {
	.fault        = mg_vma_fault,
	.page_mkwrite = mg_vma_page_mkwrite,
};

/* --------------------------------------------------------------------------
 * Operaciones del dispositivo
 * -------------------------------------------------------------------------- */

static int mg_open(struct inode *inode, struct file *filp)
{
	struct mg_session *s;

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	mutex_init(&s->lock);
	filp->private_data = s;
	return 0;
}

static int mg_release(struct inode *inode, struct file *filp)
{
	struct mg_session *s = filp->private_data;

	if (!s)
		return 0;

	pr_info("mmapguard: cerrando sesion (fallos=%llu, descifrados=%llu bytes, mmap denegados=%llu, escrituras denegadas=%llu)\n",
		s->stats.faults, s->stats.bytes_decrypted,
		s->stats.denied_mmap, s->stats.denied_write);

	mg_release_pages(s);

	if (s->tfm)
		crypto_free_sync_skcipher(s->tfm);
	if (s->backing)
		filp_close(s->backing, NULL);

	mutex_destroy(&s->lock);
	kfree(s);
	filp->private_data = NULL;
	return 0;
}

/* Asocia la sesion a un archivo cifrado concreto. */
static long mg_do_attach(struct mg_session *s, void __user *arg)
{
	struct mg_attach_req req;
	struct file *f;
	loff_t size;
	int ret;

	if (s->backing)
		return -EBUSY;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;

	/* La ruta llega de espacio de usuario: hay que terminarla a la fuerza. */
	req.path[MG_PATH_MAX - 1] = '\0';

	if (!mg_caller_is_authorized()) {
		s->stats.denied_mmap++;
		pr_warn("mmapguard: ACCESO DENEGADO a '%s' para UID %u (PID %d)\n",
			req.path, from_kuid(&init_user_ns, current_euid()),
			current->pid);
		return -EPERM;
	}

	f = filp_open(req.path, O_RDONLY, 0);
	if (IS_ERR(f)) {
		ret = (int)PTR_ERR(f);
		pr_warn("mmapguard: no se pudo abrir '%s': %d\n", req.path, ret);
		return ret;
	}

	size = i_size_read(file_inode(f));
	if (size <= 0) {
		filp_close(f, NULL);
		return -EINVAL;
	}

	s->npages = (unsigned long)DIV_ROUND_UP((u64)size, PAGE_SIZE);
	s->pages = kvcalloc(s->npages, sizeof(struct page *), GFP_KERNEL);
	if (!s->pages) {
		filp_close(f, NULL);
		return -ENOMEM;
	}

	s->tfm = crypto_alloc_sync_skcipher("ctr(aes)", 0, 0);
	if (IS_ERR(s->tfm)) {
		ret = (int)PTR_ERR(s->tfm);
		s->tfm = NULL;
		kvfree(s->pages);
		s->pages = NULL;
		filp_close(f, NULL);
		pr_err("mmapguard: no hay soporte de ctr(aes): %d\n", ret);
		return ret;
	}

	ret = crypto_sync_skcipher_setkey(s->tfm, mg_key, MG_KEY_BYTES);
	if (ret) {
		crypto_free_sync_skcipher(s->tfm);
		s->tfm = NULL;
		kvfree(s->pages);
		s->pages = NULL;
		filp_close(f, NULL);
		return ret;
	}

	s->backing = f;
	s->size = size;
	s->stats.file_size = (u64)size;

	pr_info("mmapguard: asociado '%s' (%lld bytes, %lu paginas) por PID %d\n",
		req.path, size, s->npages, current->pid);
	return 0;
}

static long mg_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct mg_session *s = filp->private_data;

	if (!s)
		return -EINVAL;

	switch (cmd) {
	case MG_IOC_ATTACH:
		return mg_do_attach(s, (void __user *)arg);

	case MG_IOC_STATS:
		if (copy_to_user((void __user *)arg, &s->stats, sizeof(s->stats)))
			return -EFAULT;
		return 0;

	default:
		return -ENOTTY;
	}
}

/*
 * Alta del mapeo: aqui se aplica la politica, ANTES de que exista una sola PTE.
 */
static int mg_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct mg_session *s = filp->private_data;
	unsigned long len = vma->vm_end - vma->vm_start;

	if (!s || !s->backing)
		return -EINVAL;

	if (!mg_caller_is_authorized()) {
		s->stats.denied_mmap++;
		pr_warn("mmapguard: MMAP DENEGADO para UID %u (PID %d)\n",
			from_kuid(&init_user_ns, current_euid()), current->pid);
		return -EPERM;
	}

	/* Politica: el mapeo es estrictamente de solo lectura. */
	if (vma->vm_flags & VM_WRITE) {
		s->stats.denied_mmap++;
		pr_warn("mmapguard: MMAP DENEGADO: se solicito PROT_WRITE (PID %d)\n",
			current->pid);
		return -EACCES;
	}

	if (vma->vm_flags & VM_SHARED) {
		s->stats.denied_mmap++;
		pr_warn("mmapguard: MMAP DENEGADO: MAP_SHARED no permitido (PID %d)\n",
			current->pid);
		return -EACCES;
	}

	if (((u64)vma->vm_pgoff << PAGE_SHIFT) + len > (u64)s->npages << PAGE_SHIFT) {
		s->stats.denied_mmap++;
		return -EINVAL;
	}

	/*
	 * Retirar VM_MAYWRITE es lo que cierra la escalada con mprotect():
	 * sin esa bandera el propio kernel rechaza cualquier intento posterior
	 * de convertir el mapeo en escribible, sin que tengamos que
	 * interceptar mprotect() nosotros.
	 *
	 * En kernel 6.x las banderas se mutan con vm_flags_set/clear, nunca por
	 * asignacion directa: el campo vm_flags es const dentro de una union y
	 * los setters toman el bloqueo de escritura del VMA.
	 */
	vm_flags_clear(vma, VM_MAYWRITE);
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);

	vma->vm_ops = &mg_vm_ops;
	vma->vm_private_data = s;

	pr_info("mmapguard: mapeo concedido [0x%lx-0x%lx] %lu bytes, solo lectura (PID %d)\n",
		vma->vm_start, vma->vm_end, len, current->pid);
	return 0;
}

static const struct file_operations mg_fops = {
	.owner          = THIS_MODULE,
	.open           = mg_open,
	.release        = mg_release,
	.unlocked_ioctl = mg_ioctl,
	.compat_ioctl   = mg_ioctl,
	.mmap           = mg_mmap,
};

/* --------------------------------------------------------------------------
 * Carga y descarga
 * -------------------------------------------------------------------------- */

static int __init mmapguard_init(void)
{
	struct device *dev;
	int ret;

	ret = mg_parse_hex_key(key_hex, mg_key, MG_KEY_BYTES);
	if (ret) {
		pr_err("mmapguard: key_hex invalida (se esperan %d caracteres hex)\n",
		       MG_KEY_BYTES * 2);
		return ret;
	}

	ret = alloc_chrdev_region(&mg_devno, 0, 1, MG_DEVICE_NAME);
	if (ret)
		return ret;

	cdev_init(&mg_cdev, &mg_fops);
	mg_cdev.owner = THIS_MODULE;
	ret = cdev_add(&mg_cdev, mg_devno, 1);
	if (ret)
		goto err_region;

	/* Desde 6.4 class_create() recibe solo el nombre. */
	mg_class = class_create(MG_DEVICE_NAME);
	if (IS_ERR(mg_class)) {
		ret = (int)PTR_ERR(mg_class);
		goto err_cdev;
	}

	dev = device_create(mg_class, NULL, mg_devno, NULL, MG_DEVICE_NAME);
	if (IS_ERR(dev)) {
		ret = (int)PTR_ERR(dev);
		goto err_class;
	}

	pr_info("mmapguard: cargado. /dev/%s listo (authorized_uid=%d)\n",
		MG_DEVICE_NAME, authorized_uid);
	return 0;

err_class:
	class_destroy(mg_class);
err_cdev:
	cdev_del(&mg_cdev);
err_region:
	unregister_chrdev_region(mg_devno, 1);
	return ret;
}

static void __exit mmapguard_exit(void)
{
	device_destroy(mg_class, mg_devno);
	class_destroy(mg_class);
	cdev_del(&mg_cdev);
	unregister_chrdev_region(mg_devno, 1);
	memzero_explicit(mg_key, sizeof(mg_key));
	pr_info("mmapguard: descargado y material de clave borrado\n");
}

module_init(mmapguard_init);
module_exit(mmapguard_exit);
