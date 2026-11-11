# 4.2 (b) Justificación Teórica: por qué la intervención debe ocurrir en la capa del kernel

## 1. El criterio correcto no es el rendimiento

Es tentador justificar Ring 0 por velocidad: menos cambios de contexto, menos copias,
`copy_to_user()` eliminado. El argumento es cierto para `read()` contra `mmap()`, y es el que
desarrolla el material de referencia del curso, pero **no es el que sostiene esta solución**. De
hecho la medición apunta en contra: `mmapguard` es 17,60x más lento que `mmap()` ordinario en acceso
secuencial. Si el criterio fuera el rendimiento, habríamos perdido.

El criterio correcto es el clásico del **monitor de referencia**: un mecanismo de seguridad vale si es
(a) *inviolable* —el adversario no puede modificarlo ni desactivarlo—, (b) *invocado siempre* —no
existe camino al recurso que lo esquive, la mediación completa de Saltzer y Schroeder— y (c)
*suficientemente pequeño para verificarse*. La tesis:

> Para el objetivo que pide el enunciado —controlar el acceso a las páginas de un archivo proyectado
> en memoria— las soluciones en espacio de usuario fallan la condición (b) por una razón
> **estructural, no de implementación**: el evento que hay que mediar no es una llamada al sistema.

## 2. El argumento decisivo: un fallo de página no es una syscall

Todo lo que el espacio de usuario puede interceptar es una **frontera de llamada al sistema**: una
biblioteca interpuesta sustituye el envoltorio de libc, `seccomp` filtra la entrada a la syscall,
`ptrace` se detiene en ella, un demonio FUSE recibe una petición cuando el VFS se la enruta. El acceso
a una página mapeada no cruza ninguna de esas fronteras: es una **excepción de hardware**. La MMU
encuentra la PTE sin el bit *present*, la CPU genera un *page fault*, y el manejador lo resuelve
íntegramente en Ring 0. El proceso nunca ejecutó una instrucción que un monitor en usuario pudiera
observar.

No es una deducción teórica: está medido. `strace` sobre el cliente de `mmapguard` registra **30
llamadas a `mmap` y ninguna señal de las 4.097 páginas descifradas** (§4.4 de la sección anterior). El
trabajo de seguridad completo es invisible al instrumento que observa syscalls, y lo que es invisible
a `strace` es invisible, por la misma razón, a cualquier monitor que viva en espacio de usuario. De
ahí el corolario que ordena toda la comparación:

> El espacio de usuario puede mediar el **alta del mapeo** (`mmap()`), que es una syscall. No puede
> mediar el **acceso a la página**, que no lo es. Y mediar solo el alta es una comprobación única:
> después, el mapeo queda gobernado por bits de PTE que el monitor no ve ni puede consultar.

## 3. Examen de las alternativas en espacio de usuario

**3.1 Cifrado dentro de la propia aplicación.** No media nada: protege el dato *en reposo* y nada más.
Una vez descifrado, el texto plano vive en memoria anónima del proceso, legible por cualquier otro
proceso del mismo UID vía `ptrace()` o `/proc/pid/mem`, volcable a un *core dump* y susceptible de ir
a *swap*. Falla (a) y (b) a la vez, y exige modificar cada aplicación: la mediación se pierde por
omisión en cuanto un programa no colabora.

**3.2 Interposición de biblioteca (`LD_PRELOAD`).** Falla de forma trivial y conocida: una syscall se
puede emitir **directamente**, con `syscall(SYS_mmap, …)` o con la instrucción `syscall` en
ensamblador, sin tocar libc; además `LD_PRELOAD` no existe para binarios estáticos y el cargador lo
ignora en binarios *setuid*. Falla (b) por construcción, y (a) con igual claridad: el código
interpuesto reside en el **mismo espacio de direcciones que el adversario**, que puede resolver el
símbolo original con `dlsym(RTLD_NEXT, …)` o simplemente desmapear al interpositor. Un monitor de
referencia situado dentro del dominio que vigila no es un monitor de referencia.

**3.3 `seccomp-bpf` y `SECCOMP_RET_USER_NOTIF`.** Es la alternativa más fuerte en el plano de las
syscalls y merece tratarse en serio, porque **sí puede denegar**: la decisión la aplica el kernel,
aunque el filtro lo defina el usuario. Primer límite: un filtro `seccomp` **no puede desreferenciar
punteros**, y la documentación del árbol lo dice sin ambigüedad —*«BPF programs may not dereference
pointers which constrains all filters to solely evaluating the system call arguments directly»*,
`Documentation/userspace-api/seccomp_filter.rst:26-27`—. El filtro ve los registros, no la memoria:
puede denegar *todo* `mmap` con `PROT_WRITE`, pero **no puede resolver el descriptor al archivo
concreto**, de modo que es incapaz de expresar una política por archivo. Segundo límite:
`SECCOMP_RET_USER_NOTIF` delega en un supervisor de usuario que sí puede leer la memoria del objetivo,
pero el mismo documento advierte del TOCTOU que eso reintroduce (`:284-290`); y el supervisor sigue
siendo un proceso de usuario —matable, `ptrace`-able, planificable a conveniencia del adversario—. Y
el límite que lo cierra: `seccomp` media **syscalls**, de modo que puede mediar la entrada a `mmap()`
y jamás el acceso a la página.

**3.4 FUSE: sistema de archivos cifrado en espacio de usuario.** Es la alternativa seria, la que usan
`gocryptfs` y `EncFS`, y sería deshonesto despacharla: **sí logra mediación completa para la E/S de
archivos**, porque es el VFS del kernel el que enruta las peticiones al demonio, y no hay forma de
esquivarla emitiendo syscalls directas. Su fallo es específico de `mmap`, y se verifica en el árbol
(`fs/fuse/file.c:2323-2328`):

```c
static const struct vm_operations_struct fuse_file_vm_ops = {
        .close          = fuse_vma_close,
        .fault          = filemap_fault,
        .map_pages      = filemap_map_pages,
        .page_mkwrite   = fuse_page_mkwrite,
};
```

`.fault = filemap_fault`. **FUSE sirve los fallos de página desde la caché de páginas genérica, no
desde el demonio.** El demonio se consulta cuando la página *no está* en caché; una vez poblada, todo
acceso posterior se resuelve dentro del kernel sin ninguna llamada ascendente. De ahí tres
consecuencias: el demonio **no puede observar ni denegar el segundo acceso a una página**, de modo que
lo que media es el poblado de la caché y no el uso; la residencia del texto plano la gobierna la
política de reclamo de la caché y no la de seguridad, así que no se puede expresar «olvida esta página
en cuanto se use»; y el texto plano **pasa por el espacio de direcciones del demonio**, que es un
proceso de usuario. A esto se suma el costo: cada falta en caché son como mínimo dos cambios de
contexto y dos copias, frente a nuestro descifrado en el propio camino del fallo.

| Alternativa | ¿Inviolable (a)? | ¿Mediación completa del **acceso** (b)? | Política por archivo | Claro en proceso de usuario |
| :--- | :---: | :---: | :---: | :---: |
| Cifrado en la aplicación | No | No media nada | Sí | **Sí** |
| `LD_PRELOAD` | No | No: syscall directa lo esquiva | Sí | **Sí** |
| `seccomp-bpf` | Sí (lo aplica el kernel) | No: un fallo no es syscall | **No**: no desreferencia punteros | n/a |
| `seccomp` + `USER_NOTIF` | No: supervisor en usuario | No: un fallo no es syscall | Sí, con TOCTOU | n/a |
| FUSE | No: demonio en usuario | **No**: la caché sirve los fallos | Sí | **Sí** |
| **LKM con `vm_ops->fault`** | Sí | **Sí: está en el camino del fallo** | Sí | **No** |

## 4. Tres propiedades que solo se obtienen en Ring 0

**4.1 La política como invariante, no como intercepción.** Un monitor en usuario tiene que
**interceptar y decidir** en cada punto de entrada, lo que obliga a haberlos enumerado todos: `mmap`,
`mprotect`, `mremap`, `madvise`, `process_vm_writev`… Olvidar uno es un agujero. `mmapguard` no
intercepta `mprotect` en ninguna parte: limpia `VM_MAYWRITE` del VMA, de modo que es **el propio
núcleo** quien rechaza cualquier intento posterior de obtener permiso de escritura, por cualquier
camino que lo pida. Está demostrado empíricamente —`denied_mmap` marcó **1 y no 2**—, y la corrección
no depende de que hayamos previsto el ataque: depende de un invariante que el kernel comprueba en
todas las rutas relevantes, **incluidas las que no conocemos**.

**4.2 El texto plano nunca reside en un proceso de usuario, ni la clave.** La clave se entrega como
parámetro de módulo con permiso `0` —`module_param(key_hex, charp, 0)`, `src/mmapguard.c:85`—, y
permiso `0` significa que **no se crea entrada en sysfs**: no es legible desde
`/sys/module/mmapguard/parameters/`, vive en memoria de kernel y ningún proceso de usuario la tiene
jamás. En cualquier esquema de la sección 3 la clave está necesariamente en el espacio de direcciones
de un proceso —la aplicación, el interpositor o el demonio FUSE—. Y la exposición del texto plano está
acotada por diseño, no por la política de reclamo del kernel (sección 3.3 §3.2).

**4.3 Granularidad de página en el camino del fallo.** El único lugar desde el cual se puede decidir
*por página y en el momento del acceso* es el camino del fallo, y ese camino está en Ring 0 por
definición arquitectónica. No es una preferencia de implementación: es dónde el hardware entrega el
control.

## 5. El costo de la mediación completa, cuantificado

La sección 2 explica por qué hay que estar en el camino del fallo; la medición explica lo que eso
cuesta, y el dato más interesante es la **simetría con FUSE**. Nótese que `fuse_file_vm_ops` incluye
`.map_pages = filemap_map_pages`: FUSE obtiene *fault-around* gratis —16 páginas por fallo,
`fault_around_pages = 65536 >> PAGE_SHIFT` en `mm/memory.c:5591`— precisamente **porque delega los
fallos a la caché de páginas**, que es exactamente lo que le cuesta la mediación por acceso. Ahí está
el intercambio, en una frase:

> FUSE compra *fault-around* a cambio de renunciar a mediar cada acceso. `mmapguard` compra la
> mediación de cada acceso a cambio de renunciar al *fault-around*.

Y eso es, literalmente, el 17,60x. **No estamos pagando por AES: estamos pagando por mediar.** La
confirmación es que el costo es proporcional a la mediación que se consume y no un impuesto fijo: con
salto 64, donde ambos esquemas hacen 65 fallos, cae a **1,94x** y solo se descifra el 1,6 % del
archivo. El mecanismo cobra por página mediada.

## 6. Lo que el espacio de usuario hace mejor

Una comparación en la que una de las partes no tiene ninguna ventaja no es una comparación, es un
argumento de venta. Estas son reales y pesan.

| Dimensión | Ventaja del espacio de usuario |
| :--- | :--- |
| **Aislamiento de fallos** | Un error en un demonio FUSE mata un proceso. Un error en `mmapguard.ko` es un *oops* o un *panic*: se lleva el sistema. Es la desventaja más grave y no tiene mitigación dentro del enfoque. |
| **Portabilidad** | El episodio del CRC de `MODVERSIONS` es la evidencia: el módulo está atado a una versión de kernel **y a un compilador**. Un binario FUSE cruza versiones sin recompilar. |
| **Tamaño de la base confiable** | Añadir ~600 líneas al TCB, en C, sin red de seguridad, es un costo de seguridad real que hay que descontar del beneficio. |
| **Costo de desarrollo y claves** | Sin libc, sin coma flotante, pila limitada, depuración por `dmesg`; y un HSM, PKCS#11 o un KMS en red son naturales en usuario y penosos en un módulo. |

Esto explica también por qué el plano 2 de la arquitectura es **eBPF LSM** y no más código de módulo:
eBPF es la síntesis moderna de las dos columnas —mediación en Ring 0, en los ganchos LSM, pero con un
**verificador** que elimina el riesgo de *panic* y con política recargable sin tocar el kernel—.

## 7. En qué sentido la capa del kernel es óptima

No en sentido absoluto. La afirmación defendible es condicional, y estas son sus cuatro condiciones:

1. **El evento a mediar es el acceso a una página, no la petición de mapeo.** Si bastara con mediar el
   alta, `seccomp` o FUSE serían preferibles por aislamiento de fallos y portabilidad.
2. **El modelo de amenaza incluye procesos del mismo UID, con `ptrace` y `/proc/pid/mem`.** Si el
   adversario fuera externo, cifrar en reposo en la aplicación bastaría.
3. **La exposición del texto plano en RAM es parte de lo que hay que acotar.** Es el requisito que
   descarta a FUSE y a `remap_pfn_range()`, y el que convierte la paginación por demanda de
   optimización en control de seguridad.
4. **Se acepta pagar el sobrecosto proporcional a las páginas tocadas.** Para un archivo de secretos
   leído por partes al arrancar, 17,60x secuencial es irrelevante; para un flujo de gran volumen es
   inaceptable, y la respuesta correcta sería cifrado a nivel de bloque, o `fscrypt`.

Bajo esas condiciones —que son las del enunciado— la intervención en la capa del kernel no es la mejor
de varias opciones viables: es **la única que satisface la mediación completa**. Las demás no pierden
por margen, pierden por construcción, porque median una frontera que el evento protegido no cruza.
