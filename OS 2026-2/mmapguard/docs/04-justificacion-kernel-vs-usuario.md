# 4.2 (b) Justificación Teórica: por qué la intervención debe ocurrir en la capa del kernel

*Complemento de [`04-resultados.md`](04-resultados.md), que aporta las mediciones que aquí se
interpretan.*

---

## 1. El criterio correcto no es el rendimiento

Es tentador justificar la elección de Ring 0 por velocidad: menos cambios de contexto, menos copias,
`copy_to_user()` eliminado. Ese argumento es cierto para la comparación `read()` contra `mmap()`, y es
el que desarrolla el material de referencia del curso, pero **no es el argumento que sostiene esta
solución**. De hecho nuestra medición apunta en contra: `mmapguard` es 17,60x más lento que `mmap()`
ordinario en acceso secuencial. Si el criterio fuera el rendimiento, habríamos perdido.

El criterio correcto es el clásico del **monitor de referencia**: un mecanismo de seguridad solo vale
si es (a) *inviolable* —el adversario no puede modificarlo ni desactivarlo—, (b) *invocado siempre*
—no existe camino al recurso que lo esquive, lo que Saltzer y Schroeder llaman mediación completa— y
(c) *suficientemente pequeño para verificarse*.

La tesis de esta sección:

> Para el objetivo que pide el enunciado —controlar el acceso a las páginas de un archivo proyectado
> en memoria— las soluciones en espacio de usuario fallan la condición (b) por una razón
> **estructural, no de implementación**: el evento que hay que mediar no es una llamada al sistema.

---

## 2. El argumento decisivo: un fallo de página no es una syscall

Todo lo que el espacio de usuario puede interceptar es una **frontera de llamada al sistema**: una
biblioteca interpuesta sustituye el envoltorio de libc, `seccomp` filtra la entrada a la syscall,
`ptrace` se detiene en ella, un demonio FUSE recibe una petición cuando el VFS se la enruta.

El acceso a una página mapeada no cruza ninguna de esas fronteras. Es una **excepción de hardware**:
la MMU encuentra la PTE sin el bit *present*, la CPU genera un *page fault*, y el manejador del
kernel lo resuelve íntegramente en Ring 0. El proceso nunca ejecutó una instrucción que un monitor en
usuario pudiera observar.

Esto no es una deducción teórica: está medido. La sección 5.4 de `04-resultados.md` documenta que
`strace` sobre el cliente de `mmapguard` registra **30 llamadas a `mmap` y ninguna señal de las 4.097
páginas descifradas**. El trabajo de seguridad completo es invisible al instrumento que observa
syscalls. Y lo que es invisible a `strace` es invisible, por la misma razón, a cualquier monitor que
viva en espacio de usuario.

De ahí el corolario que ordena toda la comparación:

> El espacio de usuario puede mediar el **alta del mapeo** (`mmap()`), que es una syscall. No puede
> mediar el **acceso a la página**, que no lo es. Y mediar solo el alta es una comprobación única:
> después, el mapeo queda gobernado por bits de PTE que el monitor no ve ni puede consultar.

---

## 3. Examen de las alternativas en espacio de usuario

No se descartan por género, sino una por una y por la razón que las cierra.

### 3.1 Cifrado dentro de la propia aplicación

La aplicación abre el archivo cifrado y lo descifra con una biblioteca criptográfica en su propio
espacio de direcciones.

**Donde falla.** No media nada: protege el dato *en reposo* y nada más. Una vez descifrado, el texto
plano vive en memoria anónima del proceso, legible por cualquier otro proceso del mismo UID vía
`ptrace()` o `/proc/pid/mem`, volcable a un *core dump* y susceptible de ir a *swap*. Falla (a) y (b)
a la vez, y además exige modificar cada aplicación: la mediación se pierde por omisión en cuanto un
programa no colabora.

### 3.2 Interposición de biblioteca (`LD_PRELOAD`)

Se sustituyen `open`, `mmap`, `read` por versiones propias cargadas antes de libc.

**Donde falla.** De forma trivial y conocida: una syscall se puede emitir **directamente**, con
`syscall(SYS_mmap, ...)` o con la instrucción `syscall` en ensamblador, sin tocar libc. Además
`LD_PRELOAD` no existe para binarios enlazados estáticamente y el cargador dinámico lo ignora en
binarios *setuid*. Falla (b) por construcción.

Y falla (a) con igual claridad: el código interpuesto reside en el **mismo espacio de direcciones que
el adversario**, que puede resolver el símbolo original con `dlsym(RTLD_NEXT, ...)` o simplemente
desmapear al interpositor. Un monitor de referencia situado dentro del dominio que vigila no es un
monitor de referencia.

### 3.3 `seccomp-bpf` y `SECCOMP_RET_USER_NOTIF`

Es la alternativa más fuerte en el plano de las syscalls, y merece tratarse en serio porque **sí
puede denegar**: la decisión la aplica el kernel, aunque el filtro lo defina el usuario.

**Donde falla, primer límite.** Un filtro `seccomp` **no puede desreferenciar punteros**. La
documentación del propio árbol lo dice sin ambigüedad:

> *"BPF programs may not dereference pointers which constrains all filters to solely evaluating the
> system call arguments directly"* — `Documentation/userspace-api/seccomp_filter.rst:26-27`

Consecuencia práctica: el filtro ve los valores de los registros, no la memoria. Puede denegar *todo*
`mmap` con `PROT_WRITE`, pero **no puede resolver el descriptor al archivo concreto** que se está
mapeando. Es incapaz de expresar la política que necesitamos, que es por archivo, no por bandera.

**Donde falla, segundo límite.** `SECCOMP_RET_USER_NOTIF` delega la decisión a un supervisor en
usuario, que sí puede leer la memoria del objetivo. Pero el propio documento advierte del TOCTOU que
eso reintroduce:

> *"struct seccomp_data contains the values of register arguments to the syscall, but does not contain
> pointers to memory... all arguments being read from the tracee's memory should be read into the
> tracer's memory before any policy decisions are made"* — `seccomp_filter.rst:284-290`

Es decir: el objetivo puede alterar los argumentos entre la lectura del supervisor y la decisión. Se
puede mitigar, pero el supervisor sigue siendo un proceso de usuario —matable, `ptrace`-able,
planificable a conveniencia del adversario— y cada `mmap` cuesta dos cambios de contexto.

**Y el límite que lo cierra.** `seccomp` media **syscalls**. Puede mediar la entrada a `mmap()` y
jamás el acceso a la página, que es exactamente lo que este diseño necesita mediar. Es necesario pero
insuficiente, por la razón de la sección 2.

### 3.4 FUSE: sistema de archivos cifrado en espacio de usuario

Es la alternativa seria, la que usan `gocryptfs` y `EncFS`, y sería intelectualmente deshonesto
despacharla: **sí logra mediación completa para la E/S de archivos**, porque es el VFS del kernel el
que enruta las peticiones al demonio. No hay forma de esquivarla emitiendo syscalls directas.

Su fallo es específico de `mmap`, y se puede verificar en el árbol. Las operaciones de VMA que FUSE
instala son:

```c
static const struct vm_operations_struct fuse_file_vm_ops = {
        .close          = fuse_vma_close,
        .fault          = filemap_fault,
        .map_pages      = filemap_map_pages,
        .page_mkwrite   = fuse_page_mkwrite,
};
```
— `fs/fuse/file.c:2323-2328`

`.fault = filemap_fault`. **FUSE sirve los fallos de página desde la caché de páginas genérica, no
desde el demonio.** El demonio se consulta cuando la página *no está* en caché; una vez poblada, todo
acceso posterior a esa página se resuelve dentro del kernel sin ninguna llamada ascendente. De aquí
salen tres consecuencias:

1. **El demonio no puede observar ni denegar el segundo acceso a una página.** La mediación por
   acceso se pierde: lo que media es el poblado de la caché, no el uso.
2. **La residencia del texto plano la gobierna la política de reclamo de la caché de páginas, no la
   política de seguridad.** No se puede expresar "olvida esta página en cuanto se use": la página en
   claro permanece mientras el kernel no decida reclamarla.
3. **El texto plano pasa por el espacio de direcciones del demonio**, que es un proceso de usuario,
   con todo lo que eso implica (`/proc/pid/mem`, volcados, *swap*). Falla (a).

A esto se suma el costo: cada falta en caché son como mínimo dos cambios de contexto y dos copias
(kernel → demonio → kernel), frente a nuestro descifrado en el propio camino del fallo, en contexto
de kernel, sin conmutar de proceso.

### Resumen

| Alternativa | ¿Inviolable (a)? | ¿Mediación completa del **acceso** (b)? | Puede expresar la política por archivo | Texto plano en proceso de usuario |
| :--- | :---: | :---: | :---: | :---: |
| Cifrado en la aplicación | No | No media nada | Sí | **Sí** |
| `LD_PRELOAD` | No | No: syscall directa lo esquiva | Sí | **Sí** |
| `seccomp-bpf` | Sí (lo aplica el kernel) | No: un fallo no es syscall | **No**: no desreferencia punteros | n/a |
| `seccomp` + `USER_NOTIF` | No: supervisor en usuario | No: un fallo no es syscall | Sí, con TOCTOU | n/a |
| FUSE | No: demonio en usuario | **No**: la caché sirve los fallos | Sí | **Sí** |
| **LKM con `vm_ops->fault`** | Sí | **Sí: está en el camino del fallo** | Sí | **No** |

---

## 4. Tres propiedades que solo se obtienen en Ring 0

### 4.1 La política como invariante, no como intercepción

Un monitor en usuario tiene que **interceptar y decidir** en cada punto de entrada, lo que obliga a
haberlos enumerado todos: `mmap`, `mprotect`, `mremap`, `madvise`, `process_vm_writev`... Olvidar uno
es un agujero.

`mmapguard` no intercepta `mprotect` en ninguna parte. Hace algo distinto: limpia un bit del VMA con
`vm_flags_clear(vma, VM_MAYWRITE)`, de modo que es **el propio núcleo** quien rechaza cualquier
intento posterior de obtener permiso de escritura, por cualquier camino que lo pida. La política deja
de ser una lista de comprobaciones y pasa a ser un **invariante del estado del VMA**.

Esto está demostrado empíricamente y es el resultado más fino de las PoC: el contador `denied_mmap`
marcó **1 y no 2**. El `mprotect(PROT_WRITE)` devolvió `EACCES` sin que el módulo ejecutara una sola
instrucción. La corrección no depende de que hayamos previsto el ataque: depende de una invariante que
el kernel comprueba en todas las rutas relevantes, incluidas las que no conocemos.

### 4.2 El texto plano nunca reside en un proceso de usuario, ni la clave

La clave se entrega como parámetro de módulo con permiso `0`:

```c
module_param(key_hex, charp, 0);   /* src/mmapguard.c:85 */
```

Permiso `0` significa que **no se crea entrada en sysfs**: la clave no es legible desde
`/sys/module/mmapguard/parameters/`. Vive en memoria de kernel y ningún proceso de usuario la tiene
jamás. En cualquier esquema de los de la sección 3, la clave está necesariamente en el espacio de
direcciones de un proceso —la aplicación, el interpositor o el demonio FUSE—.

Y la exposición del texto plano está acotada por diseño, no por la política de reclamo del kernel: se
descifra una página de 4 KB cuando se toca, y solo esa. Medido: un cliente que lee 64 bytes de un
archivo de 2 páginas deja **4.096 bytes en claro en RAM, no 8.192**. Con `remap_pfn_range()` —la
técnica del material de referencia— estarían las dos desde el instante del `mmap()`.

### 4.3 Granularidad de página en el camino del fallo

Es la unión de las dos anteriores: el único lugar desde el cual se puede decidir *por página y en el
momento del acceso* es el camino del fallo, y ese camino está en Ring 0 por definición
arquitectónica. No es una preferencia de implementación; es dónde el hardware entrega el control.

---

## 5. El costo de la mediación completa, cuantificado

La sección 2 explica por qué hay que estar en el camino del fallo. La medición explica lo que eso
cuesta, y el dato más interesante es la **simetría con FUSE**.

Nótese que `fuse_file_vm_ops` incluye `.map_pages = filemap_map_pages`. FUSE obtiene *fault-around*
gratis —16 páginas por fallo, `fault_around_pages = 65536 >> PAGE_SHIFT` en `mm/memory.c:5591`—
precisamente **porque delega los fallos a la caché de páginas**. Y delegar los fallos a la caché es
exactamente lo que le cuesta la mediación por acceso.

Ahí está el intercambio, en una frase:

> FUSE compra *fault-around* a cambio de renunciar a mediar cada acceso. `mmapguard` compra la
> mediación de cada acceso a cambio de renunciar al *fault-around*.

Y eso es, literalmente, el 17,60x: `mmap()` ordinario resuelve 4.097 páginas con 257 fallos; nosotros
con 4.097. La razón 15,9x explica casi todo el sobrecosto. **No estamos pagando por AES: estamos
pagando por mediar.**

La confirmación es que el costo es proporcional a la mediación que realmente se consume, no un
impuesto fijo. Con acceso disperso de salto 64, donde ambos esquemas hacen 65 fallos, el sobrecosto
cae a **1,94x** y solo se descifra el 1,6% del archivo. El mecanismo cobra por página mediada.

Queda además margen sin perder nada: implementar `.map_pages` descifrando un lote de páginas por
fallo bajaría el secuencial a ~9x conservando la decisión en Ring 0 y la granularidad de página.

---

## 6. Lo que el espacio de usuario hace mejor

Una comparación en la que una de las partes no tiene ninguna ventaja no es una comparación, es un
argumento de venta. Estas son reales y pesan.

| Dimensión | Ventaja del espacio de usuario |
| :--- | :--- |
| **Aislamiento de fallos** | Un error en un demonio FUSE mata un proceso. Un error en `mmapguard.ko` es un *oops* o un *panic*: se lleva el sistema. Es la desventaja más grave y no tiene mitigación dentro del enfoque. |
| **Portabilidad** | El episodio completo del CRC de `MODVERSIONS` es la evidencia: el módulo está atado a una versión de kernel **y a un compilador** (GCC 13.2). Un binario FUSE cruza versiones sin recompilar. |
| **Tamaño de la base confiable** | Añadir ~600 líneas al TCB, en C, sin red de seguridad, es un costo de seguridad real que hay que descontar del beneficio. |
| **Costo de desarrollo** | Sin libc, sin coma flotante, pila limitada, contextos de asignación restringidos, depuración por `dmesg`. |
| **Gestión de claves** | HSM, PKCS#11, un KMS en red: todo eso es natural en usuario y penoso en un módulo. |

Esto es también lo que explica por qué el plano 2 de la arquitectura es **eBPF LSM** y no más código
de módulo. eBPF es la síntesis moderna de las dos columnas: mediación en Ring 0, en los ganchos LSM,
pero con un **verificador** que elimina el riesgo de *panic* y con política recargable sin tocar el
kernel. Es el enfoque correcto para la cobertura global, y quedó condicionado a un requisito de
arranque que este entorno no satisface (`CONFIG_LSM` no incluye `bpf`).

---

## 7. En qué sentido la capa del kernel es óptima

No en sentido absoluto. La afirmación defendible es condicional, y estas son sus condiciones:

1. **El evento a mediar es el acceso a una página, no la petición de mapeo.** Si bastara con mediar
   el alta del mapeo, `seccomp` o FUSE serían preferibles por aislamiento de fallos y portabilidad.
2. **El modelo de amenaza incluye procesos del mismo UID, con `ptrace` y `/proc/pid/mem`.** Si el
   adversario fuera externo al sistema, cifrar en reposo en la aplicación bastaría.
3. **La exposición del texto plano en RAM es parte de lo que hay que acotar.** Es el requisito que
   descarta a FUSE y a `remap_pfn_range()`, y el que convierte la paginación por demanda de
   optimización en control de seguridad.
4. **Se acepta pagar el sobrecosto proporcional a las páginas tocadas.** 17,60x secuencial, 1,94x
   disperso. Para un archivo de configuración con secretos, leído una vez al arrancar y por partes,
   es irrelevante. Para un flujo secuencial de gran volumen, es inaceptable y la respuesta correcta
   sería otra: cifrado a nivel de bloque, o `fscrypt`.

Bajo esas cuatro condiciones —que son las del enunciado— la intervención en la capa del kernel no es
la mejor de varias opciones viables: es **la única que satisface la mediación completa**. Las demás
no pierden por margen, pierden por construcción, porque median una frontera que el evento protegido
no cruza.
