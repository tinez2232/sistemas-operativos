# Parte I — Análisis Conceptual y Preguntas Diagnósticas (30%)

> Referencias al árbol `linux-msft-wsl-6.18.33.2`, que es el kernel en ejecución
> (`6.18.33.2-microsoft-standard-WSL2`, x86_64, `CONFIG_PGTABLE_LEVELS=5`). Cada afirmación
> verificable lleva archivo y línea.

## 1. Mapeo virtual y Page Faults: secuencia exacta

Son dos fases con actores distintos: establecer el mapeo es software puro en Ring 0; materializar
las páginas es una excepción de hardware.

**Fase A — alta del VMA, una sola vez.** `SYSCALL_DEFINE6(mmap, …)`
(`arch/x86/kernel/sys_x86_64.c:82`) → `ksys_mmap_pgoff()` (`mm/mmap.c:566`) → `do_mmap()`
(`mm/mmap.c:334`), que resuelve el `fd` a `struct file *`, valida longitud, alineación y `RLIMIT_AS`
e invoca el gancho LSM `security_mmap_file()` (`include/linux/security.h:479`).
`get_unmapped_area()` localiza un hueco, se asigna un `struct vm_area_struct` y se rellenan sus
campos, entre ellos `vm_page_prot` con `vma_set_page_prot()` (`mm/mmap.c:81`), que traduce las
banderas del VMA a los bits que acabarán en la PTE. Por último `vfs_mmap()`
(`include/linux/fs.h:2403`) despacha a `f_op->mmap()`, donde el driver instala su `vma->vm_ops`. **Al
retornar no existe ni una sola PTE ni se ha reservado un marco físico.**

**Fase B — primer acceso.** La MMU recorre los cinco niveles desde CR3 y encuentra
`_PAGE_BIT_PRESENT` en cero. La CPU lanza el vector 14 con la dirección en CR2 y un código de error
que distingue lectura de escritura y usuario de supervisor.
`DEFINE_IDTENTRY_RAW_ERRORCODE(exc_page_fault)` (`arch/x86/mm/fault.c:1483`) deriva en
`do_user_addr_fault()` (`:1207`), que intenta la vía rápida sin `mmap_lock` con
`lock_vma_under_rcu()` (`:1325`). Sigue `handle_mm_fault()` (`mm/memory.c:6471`) →
`handle_pte_fault()` (`:6152`) → `do_fault()` (`:5821`), que elige entre `do_read_fault()`,
`do_cow_fault()` y `do_shared_fault()` (`:5771`). Se invoca `vma->vm_ops->fault`, se instala la PTE,
se invalida el TLB y **la CPU reejecuta la instrucción**, que ahora se resuelve íntegramente por
hardware.

Desde ese punto, **todo acceso posterior a esa página ocurre sin intervención del kernel**. Es la
propiedad que da su rendimiento a `mmap()` y, a la vez, la que crea el problema de este parcial.

## 2. `MAP_SHARED` frente a `MAP_PRIVATE`

`MAP_SHARED` activa `VM_SHARED | VM_MAYSHARE`: los fallos de escritura se atienden con
`do_shared_fault()`, que trabaja sobre el folio de la Page Cache del inodo, de modo que lo escrito
es visible de inmediato para cualquier otro proceso y el writeback acabará persistiéndolo.
`MAP_PRIVATE` entra en `do_cow_fault()`, que crea una copia anónima: no hay persistencia ni
visibilidad cruzada.

Un detalle del código resultó decisivo para este trabajo. En la rama `MAP_SHARED` de `do_mmap()`
(`mm/mmap.c:462-464`):

```c
vm_flags |= VM_SHARED | VM_MAYSHARE;
if (!(file->f_mode & FMODE_WRITE))
        vm_flags &= ~(VM_MAYWRITE | VM_SHARED);
```

Si el descriptor no es escribible, el kernel **degrada el mapeo en silencio**: no falla, cambia la
semántica pedida. Un filtro que razone sobre las banderas que pidió el usuario, y no sobre las que
quedaron en el VMA, analiza algo que el kernel no construyó. Esto se verificó en la práctica (PoC
A2) y obligó a corregir la prueba, no el módulo.

**Seguridad.** Con `MAP_SHARED`, un mapeo escribible da persistencia y visibilidad cruzada **sin
ejecutar una sola syscall de escritura**, de modo que un auditor que vigile `write()` queda ciego.
Con `MAP_PRIVATE` no hay persistencia, pero sí una copia completa en claro en el espacio del
atacante: se pierde confidencialidad aunque se conserve la integridad.

## 3. Copy-On-Write: ciclo de vida y escalada

Tras `fork()`, o en un mapeo privado de archivo, no se duplica nada: ambas PTE se marcan solo
lectura limpiando `_PAGE_BIT_RW`, mientras el VMA conserva `VM_WRITE` y `VM_MAYWRITE`. Esa
discrepancia deliberada entre «el VMA permite escribir» y «la PTE no deja» es el disparador. Al
escribir, el #PF deriva en `do_wp_page()` (`mm/memory.c:4049`): si el folio es exclusivo del proceso
—`wp_can_reuse_anon_folio()` (`mm/memory.c:3986`)— basta reactivar `_PAGE_BIT_RW`; si no,
`wp_page_copy()` (`mm/memory.c:3658`) reserva un folio nuevo y reapunta la PTE.

**Dirty COW (CVE-2016-5195) no fue un fallo de permisos sino de atomicidad:** `PROT_WRITE`,
`VM_MAYWRITE` y los bits de la PTE eran correctos en todo momento. El atacante mapeaba `MAP_PRIVATE`
un archivo de root legible y escribía por `/proc/self/mem` mientras un hilo concurrente lanzaba
`madvise(MADV_DONTNEED)` sobre el rango, descartando la copia privada recién creada; al reintentar,
`__get_user_pages()` abandonaba la exigencia de escritura privada y terminaba escribiendo **sobre el
folio de la Page Cache del archivo original**.

La corrección no añadió un candado: separó semánticamente «escribir» de «romper el compartido».
`FAULT_FLAG_UNSHARE` (`include/linux/mm_types.h:1659`) hizo de la ruptura de COW una operación
explícita e idempotente, y combinarla con `FAULT_FLAG_WRITE` se declaró ilegal (`:1644`).

**Lección que gobierna este diseño:** un control que se evalúa una vez y cuyo resultado se reutiliza
es un objetivo de carrera.

## 4. Intercepción: hook de la syscall table frente a LSM

**Corrección de nomenclatura.** El enunciado menciona el gancho `file_mmap`. **No existe en los
kernels modernos.** El real es `mmap_file` (`include/linux/lsm_hook_defs.h:199`), invocado por
`security_mmap_file()` (`include/linux/security.h:479`). `file_mmap` es nomenclatura anterior a la
serie 3.x que sobrevive en tutoriales y en la salida de los asistentes de IA, y por eso es un buen
indicador de material desactualizado.

**LKM con hook en la syscall table.** Da control total y cobertura global, pero tiene cuatro
obstáculos y cada uno basta para descartarlo: (1) `sys_call_table` no está exportada y
`kallsyms_lookup_name()` tampoco desde 5.7; (2) escribir la tabla obliga a limpiar el bit WP de
`CR0`, desarmando una protección del procesador para todo el sistema; (3) la reescritura no es
serializable —otra CPU puede estar despachando esa misma syscall—; (4) en x86_64 el despacho pasa
por envoltorios `__x64_sys_*(struct pt_regs *)` y la técnica choca con CFI y con `lockdown`.

**LSM.** Es la interfaz diseñada para esto: estable, situada tras resolver el descriptor y antes de
crear el VMA, con denegación por valor de retorno. **Desventaja decisiva:** `security_add_hooks()`
no está exportada y la tabla se fija en compilación, así que **no se puede registrar un LSM desde un
módulo cargable**.

La vía practicable hoy es **BPF LSM** (pregunta 7). 

## 5. Cifrado transparente en memoria

La respuesta en árbol es **fscrypt**: el sistema de archivos cifra por bloques en el writeback y
descifra en la lectura con `fscrypt_decrypt_pagecache_blocks()` (`include/linux/fscrypt.h:353`),
con clave por inodo derivada de una maestra en el llavero del kernel. Para que `mmap()` sea legible
no hace falta nada especial: `filemap_fault` entrega el folio de la Page Cache, que **ya contiene
texto en claro** porque el descifrado ocurrió al poblarla.

Y ahí está el punto. **La Page Cache almacena texto en claro**, de modo que el aislamiento no lo
aporta la criptografía sino los permisos ordinarios del VFS: cualquier proceso autorizado a abrir el
archivo ve el contenido descifrado, y dos procesos que lo mapeen comparten los mismos marcos
físicos. fscrypt protege **datos en reposo** —un disco sustraído, un respaldo filtrado— y no frente a
un proceso local no privilegiado, que es justamente el atacante de nuestro modelo.

## 6. `msync`, `fsync` y páginas sucias

El kernel mantiene la suciedad en dos sitios, y confundirlos es la fuente habitual de error: el
**bit de hardware** `_PAGE_BIT_DIRTY` de la PTE (bit 6,
`arch/x86/include/asm/pgtable_types.h:16`), que activa la CPU sola al escribir, y el **bit de
software** `PG_dirty` del folio, que es el que consulta el writeback.

Para un `MAP_SHARED` de archivo el kernel **no concede la escritura directamente**: mapea de solo
lectura a propósito para que la primera escritura genere una trampa. `do_shared_fault()`
(`mm/memory.c:5771`) llama a `do_page_mkwrite()` (`mm/memory.c:3478`), que invoca
`vm_ops->page_mkwrite()` (`mm/memory.c:3489`); el sistema de archivos reserva bloques y marca el
folio sucio, y **solo entonces** se activa `_PAGE_BIT_RW`. A partir de ahí toda escritura es
hardware puro. `msync(MS_SYNC)` traslada el estado sucio a los folios e inicia el writeback.

**Cómo se evade un filtro.** `page_mkwrite` se dispara **una sola vez por página**. Un filtro que
inspeccione el contenido en ese instante y deje la página escribible implementa un TOCTOU de
manual: el atacante escribe contenido inocuo, supera la inspección y sobrescribe con la carga
maliciosa **sin que el kernel se entere**, porque ya no hay fallos que atender. Solo quedan dos
puntos de inspección sólidos: en el writeback, barato pero tardío, o manteniendo la página de solo
lectura y aceptando un fallo por escritura, inmediato pero anulando la ventaja de `mmap()`.

**`mmapguard` disuelve el dilema en vez de escoger un lado** (sección 3.3): el mapeo es de solo
lectura, así que no existe una primera escritura que inspeccionar y la decisión se toma en el alta
del VMA. El precio es la funcionalidad, y el día que se admitan modificaciones el dilema reaparece
intacto.

## 7. eBPF, kprobes y fexit

| Tipo | Punto de enganche | ¿Puede denegar? |
| :--- | :--- | :---: |
| `kprobe` / `kretprobe` | Cualquier símbolo del kernel | No |
| `fentry` / `fexit` | Entrada/salida de función, vía BTF | No |
| Tracepoints | Puntos estáticos de ABI estable | No |
| **BPF LSM** (`SEC("lsm/...")`) | Ganchos del framework LSM | **Sí**, por valor de retorno |

La distinción decide la arquitectura: `fentry` y `kprobe` **observan pero no alteran el flujo**, su
valor de retorno se ignora. Solo BPF LSM deniega, porque se inserta en una cadena cuyo contrato es
exactamente ése. No hace falta recompilar por **CO-RE** apoyado en BTF, disponible aquí
(`CONFIG_DEBUG_INFO_BTF=y`): el kernel publica su descripción de tipos y libbpf reubica los
desplazamientos en carga.

**Condición de arranque aquí:** `CONFIG_BPF_LSM=y`, pero `CONFIG_LSM` no incluye `bpf` y
`/proc/cmdline` no trae `lsm=`. Los ganchos están compilados pero **no son adjuntables**, que es por
lo que el plano 2 queda documentado y no implementado.

## 8. Evasión de permisos POSIX

**Sí, el kernel lo permite.** La asimetría entre las dos ramas de `do_mmap()` es explícita. Rama
`MAP_SHARED`:

```c
if (prot & PROT_WRITE) {
        if (!(file->f_mode & FMODE_WRITE))
                return -EACCES;
```

Rama `MAP_PRIVATE`, en su totalidad en lo que a permisos respecta:

```c
case MAP_PRIVATE:
        if (!(file->f_mode & FMODE_READ))
                return -EACCES;
```

**No hay chequeo de escritura: solo se exige poder leer.** Es coherente con la semántica de
`MAP_PRIVATE` —toda escritura se desvía por COW y el archivo nunca se modifica— y equivale a leer a
un búfer con `read()` y modificarlo, que nadie consideraría una violación de permisos.

**Riesgos.** El contenido queda en claro y modificable en el espacio del atacante, de modo que
cualquier control de integridad que inspeccione *el archivo* es irrelevante para lo que el proceso
realmente usa; las páginas privadas modificadas pueden acabar en swap o en un volcado, razón por la
que el plano 1 fija `VM_DONTDUMP`; y es exactamente la configuración que explotó Dirty COW.
Verificado en la PoC A4 (sección 4.2): **la política no puede asumir que `O_RDONLY` equivale a
inofensivo.**

## 9. Carreras entre `madvise(MADV_DONTNEED)` y escrituras

`madvise_dontneed_free()` (`mm/madvise.c:907`) termina en `zap_page_range_single()` (`:1187`), que
**desmonta las PTE del rango**: para una página privada bajo COW descarta la copia anónima y el
siguiente acceso vuelve a fallar. Permite al atacante **revertir el estado de una página a voluntad
desde espacio de usuario**, y eso es lo que la hace una primitiva de carrera tan útil.

El kernel actual serializa con cuatro mecanismos: el `mmap_lock`; el bloqueo por VMA
(`lock_vma_under_rcu()`, `arch/x86/mm/fault.c:1325`), cuya exclusión se obtiene con
`vma_start_write()` —que es lo que invoca `vm_flags_set()` internamente
(`include/linux/mm.h:808`), y **la razón de fondo por la que se prohibió asignar directamente a
`vma->vm_flags`**—; el spinlock de la tabla de páginas vía `pte_offset_map_lock()`; y la
invalidación de TLB con IPI antes de liberar el marco.

Ninguno faltaba en 2016: el fallo era de **lógica de reintento**, y la corrección cambió el modelo
en vez de añadir un candado (pregunta 3).

**Principio aplicable a nuestra defensa:** los controles deben ser idempotentes y reevaluarse en
cada fallo, nunca cachearse como «ya validado». Un veredicto almacenado es un objetivo de carrera, y
la PoC A5 lo comprueba: sin escritura posible no hay precondición de carrera.

## 10. Aislamiento: descriptores, tablas y bits de la MMU

**`struct page` / `struct folio`** es el descriptor por marco físico —referencias, `mapping`,
banderas como `PG_dirty`—, metadato del kernel jamás direccionable desde Ring 3. La contención
empieza ahí: el proceso manipula direcciones virtuales, nunca descriptores de marco. **Jerarquía de
tablas:** con `CONFIG_PGTABLE_LEVELS=5`, PGD → P4D → PUD → PMD → PTE, recorrida por hardware desde
CR3. Cada proceso tiene su propia raíz, y ahí reside el aislamiento fundamental: una dirección
virtual del proceso A **no tiene traducción** hacia los marcos del proceso B.

**Bits de protección** (`arch/x86/include/asm/pgtable_types.h`):

| Bit | Nombre | Pos. | Función en la contención |
| :--- | :--- | :---: | :--- |
| P | `_PAGE_BIT_PRESENT` | 0 | Si es 0, cualquier acceso genera #PF: base de la paginación por demanda |
| R/W | `_PAGE_BIT_RW` | 1 | Soporte de COW y de la protección contra escritura |
| U/S | `_PAGE_BIT_USER` | 2 | Separa Ring 3 de Ring 0 |
| D | `_PAGE_BIT_DIRTY` | 6 | Lo activa la CPU al escribir |
| NX | `_PAGE_BIT_NX` | 63 | Impide ejecutar la página |

**NX** materializa W^X y volvió inviable la inyección clásica de shellcode, desplazando la
explotación hacia ROP. **U/S**, con SMEP y SMAP, cierra el ataque de hacer que el kernel salte a una
carga en espacio de usuario, y SMAP es la razón de hardware por la que un driver usa
`copy_to_user()` y no `memcpy()`. **R/W** sostiene COW, y la pregunta 3 demuestra que los bits por sí
solos no bastan: deben evaluarse atómicamente respecto al acceso.

**El límite que justifica este trabajo.** Los bits de la MMU se aplican **por traducción, no por
contenido**: protegen rutas de acceso —quién puede leer, escribir o ejecutar— y **nada en la MMU
puede decir si los bytes dentro de una página legítimamente escribible son maliciosos**, ni si quien
escribe está autorizado por una política de aplicación. Esa brecha es la que cubren los planos 1 y 3
de la arquitectura propuesta.
