# Parte I — Análisis Conceptual y Preguntas Diagnósticas (30%)

> Todas las referencias a código corresponden al árbol `linux-msft-wsl-6.18.33.2`, que es
> exactamente el kernel en ejecución (`6.18.33.2-microsoft-standard-WSL2`, x86_64,
> `CONFIG_PGTABLE_LEVELS=5`). Cada afirmación verificable se acompaña de archivo y línea.

---

## 1. Mapeo virtual y Page Faults: secuencia exacta de eventos

El proceso se divide en dos fases que ocurren en momentos distintos y con actores distintos: el
establecimiento del mapeo es software puro en Ring 0; la materialización de las páginas es una
excepción de hardware.

### Fase A — Establecimiento del VMA (una sola vez)

1. **Entrada a la syscall.** `SYSCALL_DEFINE6(mmap, ...)` (`arch/x86/kernel/sys_x86_64.c:82`)
   deriva en `ksys_mmap_pgoff()` (`mm/mmap.c:566`).
2. **Resolución del descriptor.** Se traduce el `fd` a `struct file *` consultando
   `task_struct->files`, y se llama a `vm_mmap_pgoff()` → `do_mmap()` (`mm/mmap.c:334`).
3. **Validación.** `do_mmap()` comprueba longitud, alineación, `RLIMIT_AS`, el número máximo de
   mapeos, e invoca el gancho LSM `security_mmap_file()` (`include/linux/security.h:479`). Según
   las banderas aplica además los chequeos de permiso descritos en la Pregunta 8.
4. **Selección de dirección virtual.** `get_unmapped_area()` localiza un hueco libre en el árbol de
   VMAs del `mm_struct`.
5. **Construcción del descriptor.** Se asigna un `struct vm_area_struct` y se rellenan `vm_start`,
   `vm_end`, `vm_pgoff`, `vm_file`, `vm_flags` y `vm_page_prot` — este último mediante
   `vma_set_page_prot()` (`mm/mmap.c:81`), que traduce las banderas del VMA a los bits de
   protección que acabarán en la PTE.
6. **Callback del archivo o del driver.** `vfs_mmap()` (`include/linux/fs.h:2403`) despacha a
   `f_op->mmap()` o, en la API moderna, a `f_op->mmap_prepare()`. Aquí el driver instala su
   `vma->vm_ops`.
7. **Enlace y retorno.** El VMA se enlaza al `mm_struct` y la syscall devuelve la dirección
   virtual. **En este punto no existe ni una sola PTE ni se ha reservado un solo marco físico.**

### Fase B — Primer acceso: la excepción de hardware

8. **Recorrido de la MMU.** La instrucción toca la dirección. El hardware recorre, partiendo de
   CR3, los cinco niveles de este kernel: PGD → P4D → PUD → PMD → PTE. El bit `_PAGE_BIT_PRESENT`
   (bit 0) está en cero.
9. **Excepción #PF.** La CPU lanza el vector 14, deposita la dirección culpable en CR2 y apila un
   código de error que distingue lectura de escritura y usuario de supervisor.
10. **Entrada al kernel.** `DEFINE_IDTENTRY_RAW_ERRORCODE(exc_page_fault)`
    (`arch/x86/mm/fault.c:1483`) deriva en `do_user_addr_fault()` (`arch/x86/mm/fault.c:1207`).
11. **Localización del VMA.** En 6.x se intenta primero la vía rápida sin tomar `mmap_lock`,
    mediante bloqueo por VMA bajo RCU: `lock_vma_under_rcu()` (`arch/x86/mm/fault.c:1325`). Si
    falla, se recae al camino clásico con `mmap_lock`.
12. **Resolución genérica.** `handle_mm_fault()` (`mm/memory.c:6471`) → `__handle_mm_fault()`
    (`mm/memory.c:6246`), que crea los niveles intermedios que falten y llama a
    `handle_pte_fault()` (`mm/memory.c:6152`).
13. **Bifurcación por tipo de mapeo.** Para un mapeo de archivo sin PTE previa se entra en
    `do_fault()` (`mm/memory.c:5821`), que elige entre `do_read_fault()`, `do_cow_fault()` y
    `do_shared_fault()` (`mm/memory.c:5771`) según el acceso y las banderas del VMA.
14. **Obtención del marco físico.** Se invoca `vma->vm_ops->fault` (para un archivo ordinario,
    `filemap_fault`). Si la página ya está en la Page Cache el fallo es **menor**; si hay que leer
    del dispositivo, es **mayor**.
15. **Instalación de la PTE y reejecución.** Se escribe la PTE con los bits derivados de
    `vm_page_prot`, se invalida la entrada correspondiente del TLB y la CPU **reejecuta la
    instrucción que falló**, que esta vez se resuelve íntegramente por hardware.

A partir del paso 15, todo acceso posterior a esa página ocurre sin ninguna intervención del
kernel. Esa es la propiedad que da su rendimiento a `mmap()` y, simultáneamente, la que crea el
problema de seguridad del parcial.

---

## 2. `MAP_SHARED` vs. `MAP_PRIVATE` en `vm_area_struct`

La diferencia se codifica en `vm_flags` y determina qué manejador de fallo se usará.

**`MAP_SHARED`** activa `VM_SHARED | VM_MAYSHARE`. Los fallos de escritura se atienden con
`do_shared_fault()`, que trabaja **directamente sobre el folio de la Page Cache del inodo**. En
consecuencia: lo escrito es visible de inmediato para cualquier otro proceso que mapee o lea el
archivo, y el mecanismo de writeback acabará persistiéndolo en disco.

**`MAP_PRIVATE`** no activa `VM_SHARED`. El primer fallo de escritura entra en `do_cow_fault()`,
que crea una **copia anónima privada**. Las modificaciones nunca alcanzan el archivo ni son
visibles para otros procesos.

Un detalle del código merece atención particular. En la rama `MAP_SHARED` de `do_mmap()`:

```c
vm_flags |= VM_SHARED | VM_MAYSHARE;
if (!(file->f_mode & FMODE_WRITE))
        vm_flags &= ~(VM_MAYWRITE | VM_SHARED);
```

Si el descriptor no es escribible, el kernel **degrada silenciosamente el mapeo** retirando tanto
`VM_MAYWRITE` como `VM_SHARED`. No falla: cambia la semántica solicitada. Un filtro de seguridad
que razone únicamente sobre las banderas pedidas por el usuario, y no sobre las resultantes en el
VMA, estará analizando algo que no es lo que el kernel construyó.

`VM_MAYWRITE` es, además, la bandera que expresa "este mapeo podría llegar a ser escribible". Es la
que consulta `mprotect()` para decidir si una escalada de permisos es admisible, y por eso es el
punto de apoyo de toda la familia de vulnerabilidades tratada en las preguntas 3 y 8.

**Implicaciones de seguridad.** Con `MAP_SHARED`, un atacante que logre un mapeo escribible obtiene
persistencia y visibilidad cruzada, y lo hace **sin ejecutar una sola syscall de escritura**: todo
auditor que vigile `write()` queda ciego. Con `MAP_PRIVATE` no hay persistencia, pero sí una copia
completa en claro del contenido en el espacio de direcciones del atacante, de modo que se pierde la
confidencialidad aunque se conserve la integridad del archivo.

---

## 3. Mecanismo Copy-On-Write: ciclo de vida y condiciones de escalada

### Ciclo de vida

1. **Creación del estado compartido.** Tras `fork()`, o en un mapeo privado de archivo, las páginas
   no se duplican. Ambas PTE se marcan **solo lectura** limpiando `_PAGE_BIT_RW` (bit 1), mientras
   el VMA conserva `VM_WRITE` y `VM_MAYWRITE`. La discrepancia entre "el VMA permite escribir" y
   "la PTE no deja escribir" es deliberada: es el disparador del mecanismo.
2. **Intento de escritura.** La MMU encuentra la PTE presente pero no escribible y lanza un #PF con
   el bit de escritura activo en el código de error.
3. **Decisión.** `handle_pte_fault()` deriva en `do_wp_page()` (`mm/memory.c:4049`).
4. **Reuso sin copia.** Si el folio pertenece en exclusiva al proceso —`PageAnonExclusive()` o
   `wp_can_reuse_anon_folio()` (`mm/memory.c:3986`)— no se copia nada: basta con reactivar
   `_PAGE_BIT_RW`.
5. **Copia efectiva.** En caso contrario, `wp_page_copy()` (`mm/memory.c:3658`) reserva un folio
   nuevo, lo rellena con `__wp_page_copy_user()` (`mm/memory.c:3360`) y reapunta la PTE del proceso
   que falló hacia la copia privada.

### Condiciones de escalada: el caso Dirty COW (CVE-2016-5195)

El fallo no estuvo en los permisos. `PROT_WRITE`, `VM_MAYWRITE` y los bits de la PTE eran todos
correctos en cada instante. El fallo fue de **atomicidad**.

El atacante mapeaba `MAP_PRIVATE` un archivo de root legible (`/etc/passwd`) y escribía a través de
`/proc/self/mem`. El kernel iniciaba la ruptura de COW, pero un hilo concurrente lanzaba
`madvise(MADV_DONTNEED)` sobre el rango, descartando la copia privada recién creada. Al reintentar,
la lógica de `__get_user_pages()` abandonaba la exigencia de escritura y terminaba escribiendo
**sobre el folio de la Page Cache del archivo original** en lugar de sobre la copia privada:
escritura efectiva en un archivo del sistema partiendo de permiso de solo lectura.

La corrección estructural fue separar semánticamente "escribir" de "romper el compartido". Hoy
existe `FAULT_FLAG_UNSHARE` (`include/linux/mm_types.h:1659`), documentado como petición explícita
de desencadenar COW garantizando que quede mapeada una página anónima exclusiva, y la combinación
`FAULT_FLAG_WRITE | FAULT_FLAG_UNSHARE` está declarada **ilegal** (`include/linux/mm_types.h:1644`).
Romper COW dejó de ser un efecto colateral de un fallo de escritura para convertirse en una
operación explícita e idempotente, de modo que un reintento tras un `zap` no puede degradarse
silenciosamente.

**La lección que gobierna el diseño de este trabajo:** un control de seguridad que se evalúa una vez
y cuyo resultado se reutiliza es un objetivo de carrera. Por eso la arquitectura propuesta no
confía en un único punto de decisión, sino que combina una decisión en el alta del VMA con una
revalidación por página.

---

## 4. Intercepción en el kernel: LKM con hook de syscall table vs. LSM

### Corrección previa de nomenclatura

El enunciado menciona el gancho `file_mmap`. **Ese gancho no existe en los kernels modernos**: una
búsqueda sobre `include/linux/lsm_hook_defs.h` e `include/linux/security.h` no devuelve ninguna
coincidencia. El gancho real se llama `mmap_file` (`include/linux/lsm_hook_defs.h:199`) y lo invoca
`security_mmap_file()` (`include/linux/security.h:479`); existe además `security_mmap_addr()` para
la política de dirección mínima. El nombre `file_mmap` corresponde a la nomenclatura previa a la
serie 3.x y sobrevive en tutoriales y en la salida de los asistentes de IA, lo que lo convierte en
un buen indicador de material desactualizado.

### LKM con hook en la System Call Table

*Ventajas.* Control total sobre el punto de entrada, cobertura global de todos los procesos, y
sobrecosto bajo una vez instalado.

*Desventajas y viabilidad.* Cuatro obstáculos, cada uno suficiente para descartarla:

1. `sys_call_table` no está exportada, y `kallsyms_lookup_name()` tampoco desde 5.7, de modo que
   obtener la dirección exige colocar un `kprobe` sobre la propia `kallsyms_lookup_name` — un rodeo
   cuyo único fin es burlar una decisión deliberada de los mantenedores.
2. Escribir la tabla obliga a limpiar el bit WP de `CR0`, desarmando temporalmente una protección
   del procesador para todo el sistema.
3. La reescritura no es serializable: otra CPU puede estar despachando esa misma syscall.
4. En x86_64 el despacho pasa por envoltorios `__x64_sys_*(struct pt_regs *)`, y la técnica entra en
   conflicto con CFI y con `lockdown`.

### LSM

*Ventajas.* Es la interfaz diseñada para esto: estable, con el gancho situado después de la
resolución del descriptor y antes de la creación del VMA, y con semántica de denegación por valor
de retorno.

*Desventaja decisiva.* `security_add_hooks()` no está exportada y la inicialización del framework
es `__init` con la tabla fijada en compilación: **no se puede registrar un LSM desde un módulo
cargable**. Exigiría recompilar e instalar el kernel.

### La salida moderna, y un punto de intercepción nuevo

La vía practicable hoy es **BPF LSM**: programas eBPF adjuntos a los mismos ganchos, con capacidad
real de denegar y sin recompilar nada (ver Pregunta 7).

Conviene además señalar que 6.18 incorpora un punto de intercepción adicional del lado del driver.
`struct file_operations` declara ahora dos callbacks mutuamente excluyentes:
`int (*mmap)(struct file *, struct vm_area_struct *)` (`include/linux/fs.h:2285`) y
`int (*mmap_prepare)(struct vm_area_desc *)` (`include/linux/fs.h:2314`). El segundo opera sobre un
`struct vm_area_desc` (`include/linux/mm_types.h:783`) **antes de que el VMA exista**, lo que
permite rechazar o reconfigurar el mapeo sin haber construido estado. `can_mmap_file()`
(`include/linux/fs.h:2385`) emite `WARN_ON_ONCE` si un driver declara ambos.

---

## 5. Cifrado transparente en memoria

La respuesta en árbol es **fscrypt**. El sistema de archivos (ext4, f2fs, ubifs) cifra por bloques
en el camino de writeback y descifra en el de lectura mediante
`fscrypt_decrypt_pagecache_blocks()` (`include/linux/fscrypt.h:353`), con una clave por inodo
derivada de una clave maestra instalada en el llavero del kernel.

Para que la proyección vía `mmap()` sea legible no hace falta nada especial: `filemap_fault` entrega
el folio de la Page Cache, que **ya contiene texto en claro**, porque el descifrado ocurrió al
poblarla.

Y ahí está el punto que la pregunta persigue. **La Page Cache almacena texto en claro**, de modo que
la propiedad de aislamiento no la aporta la criptografía sino los chequeos de permisos ordinarios
del VFS: cualquier proceso autorizado a abrir el archivo verá el contenido descifrado, y dos
procesos distintos que lo mapeen compartirán exactamente los mismos marcos físicos. fscrypt protege
**datos en reposo** —un disco sustraído, un respaldo filtrado— y no a un proceso local no
privilegiado, que es justamente el atacante de nuestro modelo de amenaza. Lo mismo, y con menos
granularidad, vale para dm-crypt, que cifra por debajo de la capa de bloques y deja igualmente la
Page Cache en claro, con una única clave para todo el dispositivo.

De aquí se sigue directamente el diseño adoptado en este trabajo. Para defenderse de un proceso
local hay dos opciones, y la arquitectura propuesta usa ambas:

- **Autorización en el punto de mapeo**, de modo que el proceso no autorizado nunca llegue a
  obtener un VMA sobre el activo. Es el plano 2 (BPF LSM).
- **Una ruta de descifrado privada que no pueble la Page Cache compartida con texto en claro**: el
  módulo descifra hacia páginas de su propiedad y las entrega por fallo, una a una. Es el plano 1,
  y es la razón por la que se eligió `vm_ops->fault` en lugar de `remap_pfn_range()`.

---

## 6. Sincronización e integridad: `msync`, `fsync` y páginas sucias

### Dos bits de suciedad, no uno

El kernel mantiene la noción de "página sucia" en dos lugares distintos, y confundirlos es la
fuente habitual de error:

- El **bit de hardware** en la PTE, `_PAGE_BIT_DIRTY` (bit 6, `arch/x86/include/asm/pgtable_types.h:16`),
  que activa la CPU automáticamente al ejecutar una escritura, sin intervención de software.
- El **bit de software** `PG_dirty` en el folio, que es el que consulta el subsistema de writeback.

### Cómo se pasa de uno al otro

Para un mapeo `MAP_SHARED` de archivo, el kernel **no concede la escritura directamente**. Mapea la
página de solo lectura a propósito para que la primera escritura genere una trampa:

1. La escritura falla. `do_shared_fault()` (`mm/memory.c:5771`) llama a `do_page_mkwrite()`
   (`mm/memory.c:3478`), que invoca `vmf->vma->vm_ops->page_mkwrite(vmf)` (`mm/memory.c:3489`).
2. El sistema de archivos aprovecha ese punto para reservar bloques si hace falta y marcar el folio
   como sucio.
3. **Solo entonces** se activa `_PAGE_BIT_RW` en la PTE.
4. A partir de ese momento, todas las escrituras siguientes sobre esa página son hardware puro: la
   CPU activa el bit de suciedad de la PTE y **no se ejecuta ni una instrucción del kernel**.
5. `msync(MS_SYNC)` recorre las PTE, traslada el estado sucio a los folios e inicia el writeback.
   `fsync()` añade el vaciado de metadatos y de la caché del dispositivo.

### Cómo evade un atacante un filtro de seguridad

Aquí está el núcleo de la pregunta. `page_mkwrite` se dispara **una sola vez por página**, en la
primera escritura. Un filtro que inspeccione el contenido en ese instante y después deje la página
escribible implementa un TOCTOU de manual: el atacante escribe primero contenido inocuo, supera la
inspección, y a continuación sobrescribe con la carga maliciosa **sin que el kernel se entere**,
porque ya no hay fallos de página que atender.

De ahí se deducen los únicos dos puntos de inspección sólidos, y ambos cuestan caro:

- **En el writeback**, cuando el contenido ya es definitivo. Es barato pero tardío: la ventana entre
  la modificación y la detección puede ser de segundos.
- **Manteniendo la página de solo lectura y aceptando un fallo por escritura.** Es inmediato y
  exhaustivo, pero convierte cada escritura en una transición a Ring 0, anulando precisamente la
  ventaja de `mmap()`.

Este compromiso es el que cuantifica la matriz de la sección 3.3: la profundidad de inspección se
paga en fallos de página.

**Cómo lo resuelve `mmapguard`: eliminando el dilema en vez de escogiendo un lado.** El mapeo es
estrictamente de solo lectura, de modo que no existe una primera escritura que inspeccionar. La
decisión se toma en el alta del VMA —`mg_mmap()` rechaza `PROT_WRITE` y `MAP_SHARED`, y limpia
`VM_MAYWRITE` para que el núcleo rechace por sí mismo cualquier `mprotect(PROT_WRITE)` posterior—,
no en el momento del acceso. `page_mkwrite` sigue registrado, pero solo como defensa en profundidad:
devuelve `VM_FAULT_SIGBUS`. El contador `denied_write` se mantuvo en 0 durante las pruebas, lo cual
es el resultado correcto: no mide un fallo del mecanismo, mide que la ruta de escritura es
**inalcanzable** y que por tanto el TOCTOU descrito arriba no tiene dónde ocurrir.

El precio de esa solución es la funcionalidad: no se admiten modificaciones. El día que se admitan
—lo que exige un modo AEAD con contador de versión, especificado como trabajo futuro en la sección
3.3— el dilema reaparece intacto, y la salida razonable sería la primera de las dos: verificar en el
*writeback*, con una etiqueta de autenticación por página, aceptando detectar sin prevenir.

---

## 7. Control de accesos con eBPF, kprobes y fexit

### Tipos de enganche y cuál sirve para qué

| Tipo | Punto de enganche | ¿Puede denegar? |
| :--- | :--- | :---: |
| `kprobe` / `kretprobe` | Cualquier símbolo del kernel | No |
| `fentry` / `fexit` | Entrada/salida de función, vía BTF | No |
| Tracepoints | Puntos estáticos de ABI estable | No |
| **BPF LSM** (`SEC("lsm/...")`) | Ganchos del framework LSM | **Sí**, por valor de retorno |

La distinción es la que decide la arquitectura: `fentry` y `kprobe` **observan pero no pueden
alterar el flujo** — su valor de retorno se ignora. Solo los programas BPF LSM denegan, porque se
insertan en una cadena cuyo contrato es exactamente ése.

### Por qué no hace falta recompilar el núcleo

Por **CO-RE** (*Compile Once – Run Everywhere*) apoyado en BTF. El kernel publica su propia
descripción de tipos y libbpf reubica los desplazamientos de campo en tiempo de carga. En este
sistema está disponible: `CONFIG_DEBUG_INFO_BTF=y` y `/sys/kernel/btf/vmlinux` existe. Esto permite
que el mismo objeto compilado funcione en kernels con layouts de estructuras distintos, que era la
fragilidad histórica de los módulos.

### Aplicación concreta a la auditoría de `mmap`

- `SEC("lsm/mmap_file")` y `SEC("lsm/file_mprotect")` para la política de denegación.
- Tracepoints `syscalls:sys_enter_msync` y `syscalls:sys_enter_madvise` para detectar los patrones
  de las preguntas 6 y 9.
- `kprobe` sobre `handle_mm_fault` para medir tasa de fallos de página, que es la métrica de
  sobrecosto exigida en la sección 4.2.
- Transporte de eventos por `BPF_MAP_TYPE_RINGBUF`, sin una syscall por evento.

### Ventajas y límites

La ventaja operativa decisiva frente a un LKM es el **verificador**: garantiza terminación y
seguridad de memoria antes de cargar, de modo que un error de programación produce un rechazo en
tiempo de carga y no un kernel panic. A cambio hay límites reales: no se puede hacer bucles no
acotados, el tamaño del programa está limitado, y no se puede acceder a memoria arbitraria.

**Condición de arranque en este sistema.** `CONFIG_BPF_LSM=y`, pero la lista `CONFIG_LSM` no
incluye `bpf` y `/proc/cmdline` no trae parámetro `lsm=`. Los ganchos están compilados pero **no
son adjuntables** hasta añadir `bpf` a la lista de LSM activos en la línea de comandos del kernel.

---

## 8. Evasión de permisos POSIX: `O_RDONLY` mapeado con `PROT_WRITE | MAP_PRIVATE`

**Sí, el kernel lo permite**, y el código muestra exactamente por qué. La asimetría entre las dos
ramas de `do_mmap()` es explícita.

Rama `MAP_SHARED` / `MAP_SHARED_VALIDATE`:

```c
if (prot & PROT_WRITE) {
        if (!(file->f_mode & FMODE_WRITE))
                return -EACCES;
        if (IS_SWAPFILE(file->f_mapping->host))
                return -ETXTBSY;
}
```

Rama `MAP_PRIVATE`, en su totalidad en lo que a permisos respecta:

```c
case MAP_PRIVATE:
        if (!(file->f_mode & FMODE_READ))
                return -EACCES;
```

**No hay ningún chequeo de permiso de escritura.** Solo se exige poder leer.

### Por qué el kernel lo permite

Porque es coherente con la semántica de `MAP_PRIVATE`: toda escritura se desvía por COW hacia una
copia anónima y el archivo nunca se modifica. Es funcionalmente equivalente a leer el archivo a un
búfer privado con `read()` y modificar el búfer, operación que nadie consideraría una violación de
permisos. Exigir `FMODE_WRITE` sería tan incorrecto como exigirlo para `read()` seguido de `memcpy`.

### Riesgos que implica

1. **Copia completa en claro bajo control del atacante.** El contenido del archivo queda en su
   espacio de direcciones y es modificable a voluntad. La confidencialidad del archivo no empeora
   respecto a `read()`, pero cualquier control de integridad que inspeccione *el archivo* se vuelve
   irrelevante para lo que el proceso está realmente usando.
2. **Fuga por derivación.** Las páginas privadas modificadas pueden acabar en el área de
   intercambio o en un volcado de núcleo, exponiendo datos derivados del archivo original. Es la
   razón por la que el plano 1 fija `VM_DONTDUMP`.
3. **Precedente histórico.** Es exactamente la configuración que explotó Dirty COW: mapear
   `MAP_PRIVATE` un archivo de root legible y provocar una carrera para que la escritura aterrizara
   en el folio compartido en lugar de en la copia privada (Pregunta 3).

**Consecuencia de diseño:** la política no puede asumir que `O_RDONLY` equivale a inofensivo. Este
escenario es el PoC A4 del banco de pruebas.

---

## 9. Mitigación de carreras entre `madvise(MADV_DONTNEED)` y escrituras

### Qué hace exactamente `MADV_DONTNEED`

`madvise_dontneed_free()` (`mm/madvise.c:907`) termina llamando a `zap_page_range_single()`
(`mm/madvise.c:1187`), que **desmonta las PTE del rango**. Para una página privada bajo COW, eso
descarta la copia anónima: el siguiente acceso provoca un fallo nuevo que vuelve a leer del
archivo. Es decir, permite al atacante **revertir el estado de una página a voluntad y desde
espacio de usuario**, que es lo que la convierte en una primitiva de carrera tan útil.

### Los cuatro mecanismos de serialización actuales

1. **`mmap_lock`.** `madvise` lo toma, y el camino de fallo también cuando no puede usar la vía
   rápida. Serializa modificaciones estructurales del espacio de direcciones.
2. **Bloqueo por VMA (`CONFIG_PER_VMA_LOCK`).** `lock_vma_under_rcu()`
   (`arch/x86/mm/fault.c:1325`) permite resolver fallos bajo RCU sin contender el `mmap_lock`
   global. La exclusión mutua se obtiene con `vma_start_write()`, que es justamente lo que invoca
   `vm_flags_set()` internamente (`include/linux/mm.h:808`). **Esta es la razón de fondo por la que
   se prohibió la asignación directa a `vma->vm_flags`**: el campo se declaró `const` dentro de una
   unión para forzar el uso de setters que toman el bloqueo de escritura del VMA.
3. **El spinlock de la tabla de páginas.** `pte_offset_map_lock()` serializa la instalación y el
   desmontaje de la PTE concreta.
4. **Invalidación de TLB con IPI.** Antes de liberar el marco se fuerza el *shootdown* en todas las
   CPU, de modo que ninguna conserve una traducción obsoleta hacia memoria ya liberada.

### Por qué eso no bastó en Dirty COW, y qué cambió

Ninguno de esos cuatro mecanismos estaba ausente en 2016. El fallo era de **lógica de reintento**:
la decisión de permiso y la escritura efectiva no eran atómicas respecto al `zap`, y al reintentar
se descartaba la exigencia de escritura privada. La corrección no añadió un candado, sino que
cambió el modelo: `FAULT_FLAG_UNSHARE` (`include/linux/mm_types.h:1659`) convirtió "romper COW" en
una operación explícita e idempotente, y se declaró ilegal combinarla con `FAULT_FLAG_WRITE`
(`include/linux/mm_types.h:1644`).

**Principio aplicable a nuestra defensa:** los controles deben ser idempotentes y reevaluarse en
cada fallo, nunca cachearse como "ya validado". Un veredicto almacenado es un objetivo de carrera.

---

## 10. Aislamiento de memoria: descriptores, tablas de páginas y bits de la MMU

### Las tres capas de estructuras

**`struct page` / `struct folio`.** Es el descriptor que el kernel mantiene por cada marco físico:
contador de referencias, `mapping` al que pertenece, y banderas de estado entre las que está
`PG_dirty` (Pregunta 6). Es metadato del kernel, jamás direccionable desde Ring 3. La contención
empieza aquí: el proceso manipula direcciones virtuales, nunca descriptores de marco.

**Jerarquía de tablas.** Este kernel usa `CONFIG_PGTABLE_LEVELS=5`, es decir PGD → P4D → PUD → PMD →
PTE. Cada nivel es a su vez un marco físico de 512 entradas, y el recorrido lo ejecuta el hardware
partiendo de CR3. Cada proceso tiene su propia raíz, y ahí reside el aislamiento fundamental: una
dirección virtual del proceso A simplemente **no tiene traducción** hacia los marcos del proceso B.

**Bits de protección** (`arch/x86/include/asm/pgtable_types.h`):

| Bit | Nombre | Posición | Función en la contención |
| :--- | :--- | :---: | :--- |
| P | `_PAGE_BIT_PRESENT` | 0 | Si es 0, cualquier acceso genera #PF. Base de la paginación por demanda |
| R/W | `_PAGE_BIT_RW` | 1 | Soporte de COW y de la protección contra escritura |
| U/S | `_PAGE_BIT_USER` | 2 | Separa Ring 3 de Ring 0 |
| D | `_PAGE_BIT_DIRTY` | 6 | Lo activa la CPU al escribir |
| NX | `_PAGE_BIT_NX` | 63 | Impide ejecutar la página |

### Contención de inyección de código

- **NX (bit 63)** materializa W^X: una página de datos que el atacante logre escribir no puede
  ejecutarse. Es lo que volvió inviable la inyección clásica de shellcode y desplazó la explotación
  hacia ROP, que reutiliza código ya ejecutable en lugar de inyectarlo.
- **U/S (bit 2)**, combinado con **SMEP** (el kernel no puede ejecutar páginas de usuario) y
  **SMAP** (el kernel no puede leer ni escribir memoria de usuario sin `stac`/`clac`), cierra el
  ataque clásico de hacer que el kernel salte a una carga preparada en espacio de usuario. SMAP es,
  además, la razón de hardware por la que un driver debe usar `copy_to_user()` y no `memcpy()`.
- **R/W (bit 1)** es el mecanismo sobre el que cabalga COW, y la Pregunta 3 demuestra que los bits
  por sí solos no bastan: deben evaluarse atómicamente respecto al acceso.
- **KPTI** separa por completo los juegos de tablas de usuario y kernel como mitigación de Meltdown.

### El límite que justifica este trabajo

Los bits de la MMU se aplican **por traducción**, no por contenido. Protegen *rutas de acceso*:
quién puede leer, escribir o ejecutar una página. **Nada en la MMU puede decir si los bytes dentro
de una página legítimamente escribible son maliciosos**, ni si quien escribe está autorizado por
una política de aplicación.

Esa es exactamente la brecha que cubren los planos 1 y 3 de la arquitectura propuesta: el hardware
garantiza el aislamiento entre espacios de direcciones, pero la semántica de "este proceso puede
modificar este contenido" solo puede imponerse desde la capa de kernel, interceptando el alta del
VMA y los fallos de página.
