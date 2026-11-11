# 4.2 Medición de Impacto y Análisis de Resultados

## 1. Entorno y metodología

| Elemento | Valor |
| :--- | :--- |
| Kernel | `6.18.33.2-microsoft-standard-WSL2`, x86_64, 5 niveles de paginación |
| CPU / RAM | Intel Core i5-12450H, 12 hilos / 7,6 GiB |
| Compilador del módulo | GCC 13.4.0 (misma rama que el kernel, por CRC de MODVERSIONS) |
| Implementación de AES | `ctr-aes-vaes-avx2` (acelerada por hardware, prioridad 600) |
| Conjunto de datos | 16.777.285 bytes = 4.097 páginas |

El cifrado está acelerado por hardware, dato necesario para interpretar los resultados: el sobrecosto
que se mide **no** está dominado por el costo criptográfico.

Se comparan tres formas de acceder al mismo contenido: `read()` en bloques de 4 KB sobre el archivo
en claro, `mmap()` ordinario sobre el archivo en claro, y `mmapguard` sobre el archivo cifrado. Tres
decisiones metodológicas, cada una contra un defecto concreto:

1. **La caché de páginas se calienta antes de medir**, para medir el costo del mecanismo y no el del
   disco. Lo confirma que los fallos mayores (`majflt`) son 0 en todas las corridas.
2. **Once muestras por método, y se reporta la mediana.** Una sola muestra de la latencia de `mmap()`
   quedó dominada por el ruido del planificador: dos corridas consecutivas de la misma configuración
   dieron 43,2 µs y 3,2 µs. Sin repetición, ese número no significa nada.
3. **Los checksums se imprimen.** Si el resultado del bucle no se consume, GCC con `-O2` elimina el
   bucle por código muerto y el método parece infinitamente rápido. Es el defecto del benchmark del
   paso 5 del repositorio de referencia (sección 3.1 §6).

Los tres métodos producen **checksums idénticos** en todas las configuraciones, lo que verifica que el
descifrado por página es correcto.

## 2. Resultados funcionales

Banco completo (`./tests/run_pocs.sh --load`) sobre `secreto.enc`, 4.764 bytes = 2 páginas. **8 de 8
PASS**; la salida íntegra queda en `tests/last-run.log`.

| PoC | Ataque | Resultado medido | Mecanismo que lo bloquea | Preg. |
| :--- | :--- | :--- | :--- | :-: |
| A0 | `MG_IOC_ATTACH` desde el UID 65534 | `EPERM`, `denied_mmap = 1` | `mg_caller_is_authorized()` | 4 |
| A1 | Lectura directa con `read()` | Ilegible: fracción imprimible 0,41 | Cifrado en reposo | 5 |
| OK | Lectura por el mapeo protegido | Los 4.764 bytes coinciden con el claro | — (control positivo) | 1, 5 |
| A2 | `mmap(PROT_WRITE)`; `mmap(MAP_SHARED)` | `EACCES`; ver §2.2 | `mg_mmap()` y el kernel | 2 |
| A3 | `mprotect(PROT_WRITE)` tras mapeo legítimo | `EACCES`, `denied_mmap` sin cambio | **El kernel**, por `VM_MAYWRITE` retirado | 3 |
| A4 | `O_RDONLY` + `PROT_WRITE` + `MAP_PRIVATE` | Concedido sobre archivo ordinario; `EACCES` sobre el dispositivo | El kernel lo permite; `mg_mmap()` lo cierra | 8 |
| A5 | `MADV_DONTNEED` contra escritura | `SIGSEGV`; 0 discrepancias en 11.229 relecturas | La MMU, por el bit R/W de la PTE | 9 |

El control positivo no es decorativo: sin él, un módulo que denegara absolutamente todo pasaría el
resto del banco. Verifica el mapeo **completo** contra el texto en claro, no un prefijo.

### 2.1 La asimetría de los contadores

`denied_mmap` quedó en **1**, no en 2, en la sesión que ejecuta A2 y A3 juntos: el intento de
`mprotect` **nunca llegó al módulo**, lo rechazó el propio kernel. Es la confirmación empírica de que
retirar `VM_MAYWRITE` en el alta del VMA hace innecesario interceptar `mprotect()`. Y `denied_write`
quedó en **0**: `page_mkwrite` nunca llegó a ejecutarse, porque la primera barrera resultó suficiente
—la defensa en profundidad no se activó, que es justamente lo que debe ocurrir cuando la capa anterior
funciona—. Los contadores son **por sesión**, no globales: viven en el `struct mg_session` que cuelga
de cada descriptor abierto, de modo que las cifras no se acumulan entre pruebas.

### 2.2 A2: el kernel llega antes que el módulo

`mmap(MAP_SHARED)` sobre un descriptor abierto en `O_RDONLY` **es concedido**, y no es un fallo de la
política: `do_mmap()` retira `VM_SHARED` y `VM_MAYWRITE` cuando el archivo no tiene `FMODE_WRITE`
(`mm/mmap.c:462-464`), de modo que `mg_mmap()` nunca llega a ver la bandera que filtra. Medido sobre
el VMA resultante:

```
maps dice 'r--s'
smaps VmFlags: dd de me mr ms rd sd      <- sin 'sh', sin 'mw'
mprotect(PROT_WRITE) sobre el: DENEGADO con Permission denied
```

El mapeo concedido es privado y de solo lectura, tan restringido como el del caso `MAP_PRIVATE`. Con
el descriptor en `O_RDWR`, `VM_SHARED` sobrevive y entonces sí responde el módulo: `EACCES` con
`denied_mmap = 1`. Conviene no leer el `r--s` de `maps` como evidencia de nada: ese carácter depende
de `VM_MAYSHARE`, no de `VM_SHARED` (`fs/proc/task_mmu.c:454`). El episodio está documentado como A7 y
A8 en la sección 3.2.

### 2.3 A4: la evasión de `O_RDONLY` existe, y se mide

Sobre `secreto.txt` abierto en `O_RDONLY` el kernel **concede** `PROT_WRITE | MAP_PRIVATE`. La PoC
altera los primeros ocho bytes en memoria —`NOMINA C` → `AAAAAAAA`— y comprueba por `sha256` que el
archivo en disco no cambió: el escenario exacto de la pregunta 8. El mismo ataque sobre
`/dev/mmapguard` devuelve `EACCES`. El contraste con A2 lo explica una sola bandera: un mapeo
**privado** desde un descriptor de solo lectura conserva `mw` (`VM_MAYWRITE`), porque puede hacerse
escribible por copia en escritura; el **compartido** la pierde.

### 2.4 A5: la carrera no tiene precondición, y el descarte no redescifra

Fase 1: un proceso hijo intenta escribir en el mapeo protegido y **muere con `SIGSEGV`**, por el bit
R/W de la PTE y sin que el módulo ejecute una sola instrucción. Sin escritura posible no hay nada que
serializar contra el descarte, que es la precondición que describe la pregunta 9. Fase 2, cuatro hilos
martilleando `MADV_DONTNEED` durante 2 segundos contra un lector que reverifica el digest completo:

| Magnitud | Valor |
| :--- | ---: |
| Descartes (`madvise`) | 21.615 |
| Relecturas completas del mapeo | 11.229 |
| Discrepancias de contenido | **0** |
| Fallos de página menores del proceso (`ru_minflt`) | **+6.665** |
| `faults` y bytes descifrados por el driver | **+0** |

Los +6.665 fallos son la prueba de que el martilleo funcionaba: las PTE se descartaban y
rematerializaban de verdad. Sin esa cifra, un martilleo que no descartara nada habría «pasado» la
prueba sin demostrar nada.

Las dos últimas filas son el hallazgo: **el descarte no provoca un solo descifrado nuevo**. El fallo
rematerializado encuentra la página en `s->pages[pgoff]` y devuelve una referencia a la misma página
en claro (`src/mmapguard.c:252-259`), sin volver a descifrar y sin incrementar `faults`
(`src/mmapguard.c:297`). La consecuencia para el modelo de amenaza es concreta: **la ventana de
exposición del texto en claro es la vida de la sesión, no la de la PTE.** `MADV_DONTNEED` retira la
traducción del proceso, no la copia descifrada del módulo, que solo se borra en `mg_release()` con
`memzero_explicit()`. Estrechar esa ventana —descartar páginas en claro por presión de memoria o por
temporizador— queda como trabajo futuro.

## 3. Sobrecosto según el patrón de acceso

Mediana de 11 muestras; `minflt` = fallos de página menores. El archivo es el mismo en los tres
regímenes: solo cambia el patrón de acceso.

| Régimen | Método | Pared (s) | CPU kernel (s) | minflt | `mmap()` (µs) |
| :--- | :--- | ---: | ---: | ---: | ---: |
| **Secuencial** | `read()` 4 KB | 0,00200 | 0,00209 | 0 | — |
| 4.097 págs. | `mmap()` normal | 0,00038 | 0,00038 | 257 | 8,1 |
| | **`mmapguard`** | 0,00677 | 0,00677 | **4.097** | 3,1 |
| **Salto de 8** | `read()` 4 KB | 0,00022 | 0,00020 | 0 | — |
| 513 págs. | `mmap()` normal | 0,00026 | 0,00024 | 257 | 3,4 |
| | **`mmapguard`** | 0,00077 | 0,00069 | **513** | 2,4 |
| **Disperso (64)** | `read()` 4 KB | 0,00002 | 0,00002 | 0 | — |
| 65 págs. | `mmap()` normal | 0,00005 | 0,00005 | 65 | 1,2 |
| | **`mmapguard`** | 0,00010 | 0,00010 | 65 | 1,6 |

| Patrón | Páginas tocadas | Sobrecosto vs. `mmap()` | Archivo descifrado |
| :--- | ---: | ---: | ---: |
| Secuencial | 4.097 | **17,60x** | 100 % |
| Salto de 8 | 513 | **2,93x** | 12,5 % |
| Disperso (64) | 65 | **1,94x** | **1,6 %** |

## 4. Análisis: de dónde viene realmente el costo

**4.1 El sobrecosto escala con páginas tocadas, no con el tamaño del archivo.** De 17,60x a 1,94x
sobre el mismo archivo de 16 MB, variando únicamente el patrón de acceso. Es la propiedad que
justifica la arquitectura: el precio se paga por página efectivamente usada.

**4.2 El costo dominante NO es el cifrado, es la ausencia de *fault-around*.** La columna `minflt` lo
explica: en acceso secuencial, `mmap()` ordinario resuelve 4.097 páginas con solo **257 fallos**,
porque el kernel implementa `filemap_map_pages()` y puebla hasta 16 PTE por fallo. `mmapguard`
implementa únicamente `.fault`, de modo que paga **un fallo por página**. La razón entre ambos es
15,9x, prácticamente idéntica al sobrecosto observado de 17,60x. La prueba está en el régimen
disperso: con salto de 64 páginas el *fault-around* no aporta nada —ambos ejecutan 65 fallos— y el
sobrecosto cae a 1,94x. Ese residuo sí es el costo real de `kernel_read()` más AES por página. Un
modelo de dos puntos separa ambas componentes: leer y descifrar una página cuesta ≈ **0,8 µs** (del
régimen disperso, donde los fallos coinciden) y un fallo de página adicional cuesta ≈ **0,8 µs** (del
residuo en el secuencial). Es decir, en acceso secuencial **aproximadamente la mitad del sobrecosto es
evitable** y no tiene nada que ver con la seguridad.

**4.3 La latencia de la llamada `mmap()` está por debajo del ruido.** Los valores medidos —3,1 contra
8,1 µs, 2,4 contra 3,4 µs, 1,6 contra 1,2 µs— no mantienen un orden consistente: en dos de las tres
configuraciones `mmapguard` resulta *más rápido* que `mmap()` ordinario, lo cual es físicamente
imposible dado que hace trabajo adicional. La conclusión correcta no es que sea más rápido, sino que
**el sobrecosto de la política aplicada en el alta del VMA no es medible con esta instrumentación**:
queda sepultado bajo la varianza del planificador. Eso confirma cuantitativamente la predicción de la
matriz de compensaciones de la sección 3.3: una decisión tomada una sola vez tiene costo marginal
despreciable.

**4.4 `strace` es el instrumento equivocado, y eso mismo es el hallazgo.** Perfil de syscalls de una
corrida completa:

```
% time     seconds  usecs/call     calls    syscall
 97.46    0.484200          10     45069    pread64
  0.08    0.000381          12        30    mmap
```

Las 45.069 llamadas `pread64` (11 repeticiones × 4.097 páginas) provienen **exclusivamente del método
`read()`**; los métodos basados en memoria mapeada aportan 30 llamadas `mmap` y nada más. Las 4.097
operaciones de descifrado por repetición que ejecuta `mmapguard` **son invisibles para `strace`**,
porque un fallo de página no es una llamada al sistema. Es el «punto crítico de vulnerabilidad» del
enunciado visto desde la instrumentación: si la defensa no puede observarse con herramientas de
syscalls, el atacante tampoco. Medir mecanismos basados en `mmap` exige `getrusage()`, `ftrace` o
`perf`, nunca `strace`.

## 5. Optimización identificada y no implementada

Implementar `vm_operations_struct->map_pages` permitiría poblar varias PTE por fallo, igual que
`filemap_map_pages()`. Según el modelo del §4.2 eso reduciría el sobrecosto secuencial de 17,60x a
aproximadamente **9x**, sin debilitar ninguna propiedad de seguridad: las páginas seguirían
descifrándose individualmente y bajo demanda, solo que agrupando la instalación de entradas.

## 6. Limitaciones de estas mediciones

1. Se tomaron en WSL2, sobre un hipervisor: la varianza es mayor que en hardware nativo y los valores
   absolutos no son comparables con los de un servidor dedicado. Las razones entre métodos, medidas en
   las mismas condiciones, sí son significativas.
2. No se midió con `perf`, porque en WSL2 exige compilarlo desde `tools/perf` del propio árbol.
3. El modelo de dos puntos del §4.2 es una estimación de primer orden, no una descomposición exacta.
4. No se midió el caso de caché fría: todas las corridas tienen `majflt = 0`. Un escenario con lectura
   real de disco cambiaría el equilibrio **a favor** de `mmapguard`, porque el costo de E/S dominaría
   sobre el del descifrado.
