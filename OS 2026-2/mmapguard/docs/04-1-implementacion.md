# 4.1 Implementación: código, compilación y pruebas de concepto

*El código completo está en `src/`, `tools/` y el `Makefile` del repositorio. Esta sección explica
las decisiones de implementación; las instrucciones de uso están en el `README.md`.*

---

## 1. El contrato con el espacio de usuario

Toda la interfaz cabe en una cabecera compartida, `src/mmapguard_uapi.h`, compilada tanto por el
módulo como por las herramientas:

```c
struct mg_attach_req {
        char  path[MG_PATH_MAX];   /* ruta absoluta del archivo cifrado */
        __u64 flags;               /* reservado */
};

struct mg_stats {
        __u64 file_size, faults, bytes_decrypted, denied_mmap, denied_write;
};

#define MG_IOC_ATTACH   _IOW('G', 1, struct mg_attach_req)
#define MG_IOC_STATS    _IOR('G', 2, struct mg_stats)
```

El flujo del cliente son tres pasos: `open("/dev/mmapguard")`, `ioctl(MG_IOC_ATTACH)` indicando qué
archivo cifrado exponer, y `mmap()` sobre ese descriptor. La auditoría se recupera en cualquier
momento con `ioctl(MG_IOC_STATS)`.

La separación importa por una razón de diseño: **el dispositivo no decide qué archivo se protege; lo
decide el cliente autorizado.** El módulo no lleva una lista de rutas protegidas codificada, de modo
que la misma instancia sirve para cualquier archivo cifrado con la clave cargada.

---

## 2. Estado por sesión y su tiempo de vida

```c
struct mg_session {
        struct file                 *backing;   /* archivo cifrado en disco     */
        loff_t                       size;
        unsigned long                npages;
        struct page                **pages;     /* cache de paginas descifradas */
        struct crypto_sync_skcipher *tfm;       /* transformacion AES-CTR       */
        struct mutex                 lock;      /* protege pages[]              */
        struct mg_stats              stats;
};
```
— `src/mmapguard.c:105-114`

La sesión se asigna en `mg_open()` y se cuelga de `filp->private_data`. Hay un detalle de corrección
que no es evidente y que conviene justificar:

> El VMA conserva una referencia a `vma->vm_file`, de modo que **la sesión sobrevive aunque el
> proceso cierre el descriptor después de mapear**.

Es el comportamiento correcto y además el esperado por el patrón habitual de uso
(`fd = open(); mmap(fd); close(fd);`). Si la sesión se hubiera ligado al proceso en lugar de al
`struct file`, ese cierre habría dejado el VMA con un puntero colgante y el primer fallo de página
posterior habría sido un *oops*. La liberación real ocurre en `mg_release()`, cuando desaparece la
última referencia al archivo.

`mg_release()` imprime además el resumen de auditoría de la sesión a `dmesg` antes de liberar, de
modo que queda rastro aunque el cliente nunca consulte `MG_IOC_STATS`.

---

## 3. Derivación del IV: lo que hace viable la paginación por demanda

Esta es la pieza que permite todo lo demás:

```c
static void mg_build_iv(u8 iv[MG_IV_BYTES], pgoff_t pgoff)
{
        u64 block = (u64)pgoff * (PAGE_SIZE / MG_AES_BLOCK);

        memset(iv, 0, MG_IV_BYTES);
        put_unaligned_be64(block, iv + 8);
}
```
— `src/mmapguard.c:162-168`

AES-CTR cifra generando un flujo de bloques a partir de un contador. Si el contador de la página *n*
se puede **calcular** en lugar de derivar del estado anterior, cada página se descifra de forma
independiente y en cualquier orden. Como una página de 4.096 bytes son exactamente 256 bloques AES de
16 bytes, el contador de la página `pgoff` es `pgoff * 256`, escrito en los 8 bytes bajos del IV en
orden de red.

De ahí se sigue todo lo demás: sin esta propiedad habría que descifrar secuencialmente desde el
principio del archivo para llegar a una página cualquiera, y el modelo de acceso aleatorio de `mmap()`
—su razón de ser— sería imposible. La herramienta `tools/mg_encrypt.c` replica exactamente esta
derivación con OpenSSL, de modo que el cifrado en usuario y el descifrado en kernel coinciden por
construcción.

**Nota de incluido:** `put_unaligned_be64` requiere `<linux/unaligned.h>`. La cabecera
`<asm/unaligned.h>`, que es la que aparece en casi todo el material publicado, ya no existe.

---

## 4. El manejador de fallos: el corazón del diseño

`mg_vma_fault()` (`src/mmapguard.c:230-305`) se invoca una vez por página, la primera vez que el
proceso la toca. La secuencia:

| Paso | Qué hace | Por qué así |
| :-: | :--- | :--- |
| 1 | Valida `pgoff < npages` | Un fallo fuera de rango devuelve `VM_FAULT_SIGBUS`, no accede a memoria ajena |
| 2 | Toma `s->lock` | `pages[]` es compartido entre hilos del mismo mapeo |
| 3 | Si la página ya existe, `get_page()` y listo | Evita descifrar dos veces; el coste de un segundo acceso es el de un fallo menor normal |
| 4 | `alloc_page(GFP_KERNEL \| __GFP_ZERO)` | Marco propio del módulo, inicializado a cero |
| 5 | Calcula `to_read`, truncando en la última página | Un archivo que no es múltiplo de 4 KB no debe leer basura más allá del final |
| 6 | `kmap_local_page()` + `kernel_read()` | Lee el texto cifrado directamente en el marco |
| 7 | `mg_decrypt_page()` descifra **en el sitio** | Sin copia intermedia: no hay un segundo buffer con texto plano |
| 8 | Registra en `stats` y entrega con `vmf->page` | El contador de auditoría se actualiza bajo el mismo `lock` |

Dos decisiones del camino de error merecen mención. Si la lectura o el descifrado fallan, la página
se borra con `memzero_explicit()` **antes** de devolverla al asignador: un marco con texto plano
parcial no vuelve al sistema. Y se devuelve `VM_FAULT_SIGBUS`, no `VM_FAULT_OOM`, para que el proceso
reciba una señal y no se interprete como presión de memoria.

El descifrado usa `SYNC_SKCIPHER_REQUEST_ON_STACK` y una `scatterlist` de un solo elemento que apunta
a la propia página, con el mismo origen y destino (`skcipher_request_set_crypt(req, &sg, &sg, ...)`).
La transformación se pide por nombre genérico, `crypto_alloc_sync_skcipher("ctr(aes)", 0, 0)`, lo que
deja que la API de criptografía del kernel elija la implementación acelerada disponible en el
hardware.

---

## 5. La política: aplicada antes de que exista una sola PTE

`mg_mmap()` (`src/mmapguard.c:475-529`) es el único punto donde se decide, y se ejecuta en el alta
del VMA:

```c
if (!mg_caller_is_authorized()) {          /* UID efectivo */
        s->stats.denied_mmap++;
        pr_warn("mmapguard: MMAP DENEGADO para UID %u (PID %d)\n", ...);
        return -EPERM;
}
if (vma->vm_flags & VM_WRITE)  { s->stats.denied_mmap++; return -EACCES; }
if (vma->vm_flags & VM_SHARED) { s->stats.denied_mmap++; return -EACCES; }

vm_flags_clear(vma, VM_MAYWRITE);
vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);

vma->vm_ops          = &mg_vm_ops;
vma->vm_private_data = s;
```

**`vm_flags_clear(vma, VM_MAYWRITE)` es la línea más importante del módulo.** No rechaza nada por sí
misma: retira del VMA la *capacidad* de llegar a ser escribible. A partir de ahí es el propio núcleo
quien rechaza cualquier `mprotect(PROT_WRITE)` posterior, por cualquier camino que lo solicite, sin
que el módulo intercepte nada. La política deja de ser una lista de comprobaciones —que obliga a
haber enumerado todos los puntos de entrada— y pasa a ser un invariante del estado del VMA.

Las otras dos banderas cierran huecos concretos: `VM_DONTEXPAND` impide crecer el mapeo con
`mremap()`, y `VM_DONTDUMP` lo excluye de los volcados de memoria, que de otro modo escribirían el
texto plano a disco.

**Sobre la mutación de banderas:** `vma->vm_flags` es `const` dentro de una unión en los kernels 6.x.
Hay que usar `vm_flags_set()` y `vm_flags_clear()`, que además toman el bloqueo de escritura del VMA
(`vma_start_write()`). La asignación directa `vma->vm_flags |= ...`, que es lo que aparece en todo el
material anterior a 2023, no compila.

---

## 6. Gestión de memoria y residuos

`mg_release_pages()` (`src/mmapguard.c:197-216`) borra explícitamente cada marco antes de liberarlo:

```c
memzero_explicit(page_address(s->pages[i]), PAGE_SIZE);
__free_page(s->pages[i]);
```

`memzero_explicit()` y no `memset()`, porque el compilador puede eliminar un `memset` cuyo resultado
no se lee: es exactamente el mismo modo de fallo que invalidó el banco de pruebas del repositorio de
referencia, aplicado a un borrado de seguridad en lugar de a una medición.

**Limitación declarada.** La caché `pages[]` no tiene mecanismo de reclamo: una página descifrada
permanece en memoria hasta que se cierra la sesión. Para un archivo recorrido por completo eso
significa que el consumo llega al tamaño del archivo —16 MB en nuestras mediciones—, y la exposición
de texto plano crece de forma monótona con las páginas tocadas. Es aceptable para el caso de uso que
motiva el diseño (archivos de secretos, leídos por partes) y sería inaceptable para archivos grandes
recorridos íntegramente. La corrección natural es registrar un *shrinker* o acotar la caché con una
política de expulsión; no está implementado.

---

## 7. Compilación sin advertencias

```make
obj-m          += mmapguard.o
mmapguard-objs := src/mmapguard.o
KDIR           ?= /lib/modules/$(shell uname -r)/build
ccflags-y      := -I$(src)/src -Wall -Wextra
```

El enunciado exige compilación libre de advertencias, y `-Wall -Wextra` está en `ccflags-y`, no como
recomendación externa. El módulo y las tres herramientas compilan limpios desde cero.

El `Makefile` ofrece además `make tools`, `make demo` (que regenera los datos de prueba de forma
determinista), `make clean`, `make distclean` y `make help`.

**La restricción que no es del Makefile sino del entorno:** hay que compilar con `CC=gcc-13`. El
kernel de WSL2 lo compiló Microsoft con GCC 13.2, y `CONFIG_MODVERSIONS=y` exige que los CRC de los
símbolos coincidan. Compilar con el GCC 15 del sistema hace que `make olddefconfig` autodetecte
capacidades distintas del compilador —`CONFIG_CC_HAS_COUNTED_BY` en particular—, lo que cambia el
texto de las cabeceras que `genksyms` convierte en CRC. El síntoma es
`insmod: Invalid module format` y, en `dmesg`,
*"disagrees about version of symbol module_layout"*. El CRC de `module_layout` pasó de `0x5bc3c700`
a `0xadc86883` al cambiar de compilador, y con eso el módulo cargó.

---

## 8. Herramientas de espacio de usuario

| Herramienta | Qué hace | Nota de implementación |
| :--- | :--- | :--- |
| `mg_encrypt.c` | Cifra un archivo con el esquema que espera el driver | OpenSSL `EVP_aes_128_ctr()`, un IV por página con la misma derivación del §3 |
| `mg_test.c` | Ejecuta las tres PoC y muestra la auditoría | Imprime el `errno` de cada intento, no solo éxito o fallo |
| `mg_bench.c` | Comparativa a tres bandas con `getrusage()` | 11 repeticiones y mediana; imprime todos los checksums |
| `tests/run_pocs.sh` | Orquesta el banco de PoC y resume el resultado | Verifica el entorno, correlaciona con `dmesg` y agrega códigos de salida |
| `tests/mg_poc.py` | Las PoC A0–A5, contra libc por `ctypes` | Sin compilar nada: banderas y `errno` exactos de `mmap`/`mprotect`/`madvise` |

El banco de pruebas está en dos piezas y en dos lenguajes, y la división no es arbitraria. El
enunciado pide scripts en Bash o Python, y además hay trabajo que en C habría costado mucho más:
provocar un `SIGSEGV` deliberado en un proceso hijo aislado, martillear `madvise` desde varios hilos
y leer el `errno` exacto de cada combinación de banderas. `ctypes` da acceso directo a libc sin
pasar por el `Makefile`, de modo que el banco de ataques se modifica y se reejecuta sin recompilar;
`mg_test.c` queda como implementación de referencia en C, que es lo que el enunciado pide para el
código del módulo y sus clientes.

Que `mg_bench` imprima los checksums no es cosmético: es lo que impide que `-O2` elimine los bucles
de medición por código muerto. Sin eso, el programa reporta cifras imposibles, que es precisamente el
defecto encontrado en el banco de pruebas del material de referencia.

---

## 9. Pruebas de concepto

El banco completo se ejecuta con `./tests/run_pocs.sh` (o `make test TESTFLAGS=--load`, que carga
el módulo y lo descarga al terminar). `mg_test` cubre tres ataques en una sola sesión:

| PoC | Ataque | Resultado | Quién lo bloquea | Pregunta |
| :-- | :--- | :--- | :--- | :-: |
| A1 | `read()` directo del archivo | Ilegible | El cifrado en reposo | 5 |
| A2 | `mmap(PROT_WRITE)` | `EACCES` | `mg_mmap()` | 2 |
| A3 | `mmap(PROT_READ)` + `mprotect(PROT_WRITE)` | `EACCES` | **El núcleo**, vía `VM_MAYWRITE` | 3 |

El resultado más informativo no es que los tres fallen, sino el estado de los contadores:
**`denied_mmap = 1` y no 2**, porque el `mprotect` nunca llegó al módulo; y
**`denied_write = 0`**, porque `page_mkwrite` no tuvo que actuar ni una vez. Lo primero demuestra que
el invariante del §5 funciona; lo segundo, que la ruta de escritura es inalcanzable y no simplemente
vigilada.

El banco en `tests/` añade las tres que faltaban para cerrar el cuarto entregable del 4.1. Las
cifras son las de la ejecución registrada en `tests/last-run.log`, **8 de 8 PASS**:

| PoC | Ataque | Resultado medido | Pregunta |
| :-- | :--- | :--- | :-: |
| A0 | Asociar un archivo bajo el UID 65534 | `EPERM`, `denied_mmap = 1` | 4 |
| A4 | `fd` en `O_RDONLY` mapeado `PROT_WRITE \| MAP_PRIVATE` | Concedido sobre un archivo ordinario (`NOMINA C` → `AAAAAAAA`, `sha256` del disco intacto); `EACCES` sobre el dispositivo | 8 |
| A5 | `MADV_DONTNEED` martilleado contra una escritura | `SIGSEGV`; 21.615 descartes, +6.665 fallos de página, **0** discrepancias y **0** bytes redescifrados | 9 |

A6 (`msync`, pregunta 6) sigue sin aplicar a un mapeo de solo lectura: exige el modo AEAD que la
sección de trabajo futuro documenta.

A5 obligó a corregir la sección 3.3, y ahora está medido, no razonado: 21.615 descartes provocaron
**+6.665 fallos de página** en el proceso y **+0** descifrados en el driver. Tras `MADV_DONTNEED` el
nuevo fallo **no vuelve a descifrar**: encuentra la página en `s->pages[pgoff]` y
devuelve una referencia a la misma página en claro (§4), sin incrementar `faults`. La ventana de
exposición del texto en claro es por tanto la vida de la **sesión**, no la de la PTE: `MADV_DONTNEED`
retira la traducción del proceso, no la copia que el módulo guarda hasta `mg_release()`.
