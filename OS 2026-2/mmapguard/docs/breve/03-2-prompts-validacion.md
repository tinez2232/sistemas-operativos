# 3.2 Metodología de Ingeniería de Prompts y Verificación Crítica

## 1. Naturaleza del registro

La solución se desarrolló en una sesión asistida por un modelo con acceso a terminal (Claude Code),
de forma **conversacional y no mediante prompts pre-redactados**. Los prompts de la sección 2 son
literales; la descomposición en rol y restricción es un análisis posterior. Se aclara porque la
alternativa —redactar a posteriori prompts elaborados que nunca se usaron— produciría una tabla más
vistosa y completamente falsa.

## 2. Tabla de prompts estructurados

| # | Prompt (literal) | Rol asignado | Restricción | Resultado esperado |
| :-: | :--- | :--- | :--- | :--- |
| P1 | *«clona este repo … con lo que contiene lo puedes correr para verificar la función mmap … pero si algo sale mal avísame»* | Verificador autónomo | Reportar fallos en vez de ocultarlos | Diagnóstico del estado real del repositorio |
| P2 | *«en este pdf está toda la información … por ahora revisa todo el reporte»* | Analista de requisitos | No actuar todavía, solo analizar | Desglose de entregables, pesos y bloqueos |
| P3 | *«hagamos la arquitectura como recomiendas»* | Arquitecto de sistemas | Justificar cada descarte, no solo la elección | Documento 3.3 con matriz de compensaciones |
| P4 | *«arranca con las 10 preguntas»* | Experto en kernel Linux | Fundamentar contra el árbol real, citando archivo y línea | 10 respuestas verificables |
| P5 | *«opción A, empieza con el mmapguard.ko»* | Desarrollador de kernel | Solo lectura; sin advertencias de compilación | Módulo funcional y cargable |
| P6 | *«sigue con la medición del plano 3 que vale el 40 %»* | Ingeniero de rendimiento | Medición rigurosa, no cifras vistosas | Benchmark con metodología defendible |

## 3. Alucinaciones técnicas detectadas

Las cinco primeras se detectaron **antes de compilar o antes de cargar**, por inspección del árbol o
por contradicción empírica. Las tres últimas —A6, A7 y A8— las encontró el banco de pruebas al
ejecutarse, y por eso se desarrollan aparte: son las que más enseñan.

| # | Lo que el modelo afirmó | Lo que es cierto en 6.18.33.2 | Cómo se detectó e impacto evitado |
| :-: | :--- | :--- | :--- |
| A1 | `.llseek = no_llseek` en `file_operations` | Eliminado del kernel en 6.12; no hay coincidencias en el árbol | Inspección antes de compilar. Error de compilación. Representativo: aparece en casi todo ejemplo de driver de caracteres anterior a 2024 |
| A2 | `touch .scmversion` corrige el sufijo `+` de versión local | `scripts/setlocalversion` de 6.18 ya no consulta ese archivo; hay que desactivar la detección de git | Empíricamente: se aplicó y la versión no cambió. **Más peligrosa que A1**: la recomendación no produce ningún error, simplemente no hace nada, y el fallo aparece al cargar el módulo con un mensaje que no apunta a su causa |
| A3 | El árbol de fuentes no corresponde al kernel en ejecución, porque `GCC_ASM_GOTO_OUTPUT_BROKEN` exige `GCC_VERSION < 110500` | El bloque tiene **tres** cláusulas `default`, y la tercera (`>= 130000 && < 130300`) sí incluye a GCC 13.2 | Imprimiendo el bloque completo. **Grave**: habría llevado a abandonar la ruta del módulo cargable, el 40 % de la nota, por un problema inexistente. No fue un error de conocimiento sino de **evidencia insuficiente** |
| A4 | «gcc-13 sí soporta el atributo `counted_by`», según una prueba encadenada con `&&` | GCC 13 emite *warning: attribute directive ignored* y **termina con código 0**; el test real del Kconfig usa `-Werror=attributes` | Al leer el test real. Habría descartado gcc-13, que era la solución correcta. **No basta con ejecutar una prueba: hay que replicar las condiciones exactas** |
| A5 | `CONFIG_CC_HAS_COUNTED_BY` es la causa del desajuste de CRC, presentado como hecho | La hipótesis **resultó correcta**, pero se afirmó antes de verificar que `genksyms` procesa atributos en su gramática | No la detectó nada: se acertó. Se registra porque es el modo de fallo más difícil de ver —una afirmación causal confiada y no verificada es indistinguible de una verificada hasta que resulta falsa |

### A6 — El `errno` de `mprotect`: `EPERM` escrito de memoria

Al documentar la PoC A3 se afirmó que `mprotect(PROT_WRITE)` sobre un VMA sin `VM_MAYWRITE` devuelve
`EPERM`. Es el error que dice «no tienes permiso» en la intuición de cualquiera, y encajaba con la
narrativa. La afirmación se propagó a **seis documentos**, incluido el bloque de «salida esperada»
del `README`, que llegó a mostrar `Operation not permitted` como si fuera una captura real.
`mm/mprotect.c:959-963`:

```c
/* newflags >> 4 shift VM_MAY% in place of VM_% */
if ((newflags & ~(newflags >> 4)) & VM_ACCESS_FLAGS) {
        error = -EACCES;
        break;
}
```

El núcleo contrasta cada permiso pedido contra su `VM_MAY*` y devuelve **`EACCES`**. No lo detectó
una relectura: lo detectó la PoC, porque `mg_poc.py` compara el `errno` real contra el esperado en
vez de limitarse a comprobar que la llamada falló, y marcó A3 como `INCONCLUSIVE`. El cliente en C
nunca lo habría delatado, porque imprime `strerror(errno)` sin compararlo con nada: mostraba el error
correcto y el documento que lo acompañaba decía otra cosa. **Una PoC que solo verifica que el ataque
falla no distingue entre fallar por el motivo previsto y fallar por otro.**

### A7 — Suponer que el módulo ve las banderas que pidió el usuario

La PoC A2 daba por hecho que `mmap(MAP_SHARED)` sobre `/dev/mmapguard` tenía que ser rechazado por
`mg_mmap()`, que comprueba `vma->vm_flags & VM_SHARED`. La primera ejecución marcó **FAIL**: el mapeo
fue concedido. `mm/mmap.c:462-464`, dentro de `do_mmap()`:

```c
vm_flags |= VM_SHARED | VM_MAYSHARE;
if (!(file->f_mode & FMODE_WRITE))
        vm_flags &= ~(VM_MAYWRITE | VM_SHARED);
```

El dispositivo se abre en `O_RDONLY`, así que **el núcleo retira `VM_SHARED` y `VM_MAYWRITE` antes de
llamar a `f_op->mmap`**. El módulo nunca ve la bandera que creía filtrar, y lo que concede es un
mapeo privado de solo lectura, tan restringido como el del caso `MAP_PRIVATE`. No era un fallo de
seguridad, pero deja ver algo que ningún repaso del módulo habría mostrado: **la comprobación de
`VM_SHARED` de `mg_mmap()` es inalcanzable con un descriptor de solo lectura.** Para ejercitarla hay
que abrir en `O_RDWR`, que es lo que hace ahora el tercer caso de A2. **El conjunto de banderas que
recibe un `f_op->mmap` no es el que pidió el proceso: es el que el núcleo dejó después de aplicar sus
propias reglas.**

### A8 — El instrumento mintió: `r--s` no significa `VM_SHARED`

La corrección de A7 se apoyó en `/proc/self/maps`: si el núcleo retiraba `VM_SHARED`, el mapeo debía
aparecer como `r--p`. La PoC corregida volvió a marcar **FAIL**, ahora con «el VMA quedó compartido
(`r--s`)». Dos intentos, dos veredictos equivocados sobre un módulo que nunca estuvo mal.
`fs/proc/task_mmu.c:454`:

```c
seq_putc(m, flags & VM_MAYSHARE ? 's' : 'p');
```

El cuarto carácter depende de **`VM_MAYSHARE`**, no de `VM_SHARED`, y `do_mmap()` retira
`VM_MAYWRITE | VM_SHARED` dejando `VM_MAYSHARE` intacto. Un mapeo ya degradado a privado sigue
mostrándose como `s`. El instrumento correcto es la línea `VmFlags:` de `/proc/self/smaps`
(`fs/proc/task_mmu.c:1152-1159`), con un mnemónico por bandera: `rd`, `wr`, `sh`=`VM_SHARED`,
`mw`=`VM_MAYWRITE`, `ms`=`VM_MAYSHARE`. Comprobado sobre un archivo ordinario, sin el módulo:

| Mapeo | `maps` | `VmFlags` de `smaps` |
| :--- | :---: | :--- |
| `O_RDONLY` + `MAP_SHARED` | `r--s` | `me mr ms rd sd` — **sin `sh`, sin `mw`** |
| `O_RDWR` + `MAP_SHARED` | `r--s` | `me mr ms mw rd sd` **`sh`** |
| `O_RDONLY` + `MAP_PRIVATE` | `r--p` | `me mr mw rd sd` |

Las dos primeras filas son indistinguibles en `maps` y describen VMA con permisos distintos. La
tercera añade el matiz que sostiene la pregunta 8: un mapeo **privado** desde un `fd` de solo lectura
conserva `mw`, y por eso admite `PROT_WRITE` por copia en escritura, mientras que el compartido lo
pierde.

**Las tres últimas comparten estructura:** una abstracción de usuario (`errno`, las banderas de
`mmap`, la columna de `maps`) se dio por equivalente a la estructura de kernel subyacente, y no lo
era. La verificación no termina al mirar la salida de una herramienta: termina al comprobar **qué
imprime esa herramienta exactamente**, que también es código del kernel y tiene sus propias reglas.

### Sobre el ejemplo de `CR0` que cita el enunciado

El enunciado menciona, como alucinación típica, deshabilitar el bit de protección contra escritura de
`CR0` sin control de concurrencia. **No se produjo en esta sesión**, porque la evaluación
arquitectónica descartó el hook en la *syscall table* antes de solicitar código para ella. Se deja
constancia de que no ocurrió en lugar de atribuirse una detección que no tuvo lugar.

## 4. Hallazgos de verificación proactiva

No son alucinaciones: son hechos comprobados contra el árbol *antes* de escribir código, de modo que
nunca llegaron a convertirse en errores. Son la evidencia de que el procedimiento se aplicó.

| # | Hecho verificado | Archivo y línea | Por qué importaba |
| :-: | :--- | :--- | :--- |
| V1 | `class_create()` recibe un solo argumento desde 6.4 | `include/linux/device/class.h:226` | El ejemplo habitual pasa `THIS_MODULE` |
| V2 | `vm_flags` es `const` en una unión; hay que usar `vm_flags_set/clear` | `include/linux/mm.h:808,815` | `vma->vm_flags \|= …` no compila en 6.x |
| V3 | `security_add_hooks()` no está exportada | `security/` | Descarta un LSM desde un módulo cargable |
| V4 | `kallsyms_lookup_name` y `sys_call_table` no exportadas | `kernel/kallsyms.c` | Sustenta el descarte del hook de syscalls |
| V5 | El gancho LSM `file_mmap` **no existe**; el real es `mmap_file` | `include/linux/lsm_hook_defs.h:199` | El propio enunciado usa el nombre antiguo |
| V6 | `put_unaligned_be64()` vive en `<linux/unaligned.h>` | `include/linux/unaligned.h` | `<asm/unaligned.h>` ya no está en el árbol |
| V7 | `.page_mkwrite` disponible en `vm_operations_struct` | `include/linux/mm.h:638` | Habilita la defensa en profundidad del plano 1 |
| V8 | 6.18 añade `f_op->mmap_prepare`, excluyente con `.mmap` | `include/linux/fs.h:2285,2314,2385` | Punto de intercepción posterior al corte de conocimiento habitual |
| V9 | `do_mmap()` retira `VM_SHARED` y `VM_MAYWRITE` sin `FMODE_WRITE` | `mm/mmap.c:462-464` | **A posteriori**, por el FAIL de A2 (ver A7) |
| V10 | `mprotect` devuelve `-EACCES`, no `-EPERM` | `mm/mprotect.c:959-963` | **A posteriori**, por el INCONCLUSIVE de A3 (ver A6) |
| V11 | La `s`/`p` de `maps` depende de `VM_MAYSHARE` | `fs/proc/task_mmu.c:454` | **A posteriori**, por el segundo FAIL de A2 (ver A8) |
| V12 | `smaps` sí separa `sh`, `ms` y `mw` | `fs/proc/task_mmu.c:1152-1159` | Es el instrumento con que la PoC afirma el estado del VMA |

### Por qué la distinción entre las secciones 3 y 4 es el hallazgo central

Las alucinaciones de la sección 3 se detectaron **porque alguien fue a mirar**. Ninguna se delató
sola: `no_llseek` habría fallado en compilación, pero `.scmversion` no produjo error alguno y la
conclusión errónea sobre el árbol era internamente coherente y plausible.

A6, A7 y A8 son la excepción que confirma la regla, y por eso V9–V12 están marcadas como
verificaciones *a posteriori*. No las encontró nadie leyendo: las encontró el banco al ejecutarse,
porque afirma valores concretos —el `errno` exacto, las banderas reales del VMA— en lugar de
comprobar que el ataque fracasa. Una prueba que solo exigiera «esto debe fallar» habría dado PASS en
los tres casos y habría dejado intactos los seis documentos equivocados. **El oráculo no estaba en el
árbol del kernel: estaba en exigirle a la prueba que fuera específica.** Y conviene no suavizarlo: el
banco marcó FAIL **dos veces seguidas** sobre un módulo que siempre estuvo bien. Un veredicto rojo no
prueba un defecto en el código: prueba un **desacuerdo** entre el código y lo que alguien creía del
código, y el defecto puede estar en cualquiera de los dos lados.

El riesgo real de usar asistentes de IA en programación de sistemas no son las afirmaciones
obviamente falsas, que el compilador filtra, sino las **plausibles y silenciosas**: APIs que
existieron, soluciones que funcionaron hace tres versiones, y razonamientos correctos sobre datos
incompletos.

## 5. Procedimiento de validación manual y compilación

**Verificación contra el árbol.** Se clonó `linux-msft-wsl-6.18.33.2`, que corresponde exactamente al
kernel en ejecución, y **ninguna afirmación sobre una API se dio por válida de memoria**.
**Compilación sin advertencias:** exigido por el punto 4.1, módulo y herramientas compilan con
`-Wall -Wextra` sin una sola. **Validación empírica:** ningún componente se dio por bueno hasta
ejecutarlo —carga con `insmod`, verificación en `dmesg`, banco completo y contadores—. **Validación
cruzada de las mediciones:** tres salvaguardas detalladas en 4.2, el consumo obligatorio del
checksum, once muestras con mediana y caché caliente verificada con `majflt = 0`.

**Compilación contra los headers de la distribución utilizada.** Requisito explícito del enunciado y
el punto que más esfuerzo costó. El kernel de WSL2 lo compiló Microsoft con GCC 13.2; al regenerar la
configuración con el GCC 15.2 del sistema, `make olddefconfig` autodetectó capacidades distintas
—entre ellas `CONFIG_CC_HAS_COUNTED_BY`—, lo que altera el texto de las cabeceras que `genksyms`
convierte en CRC de símbolo. El módulo compilaba y su *vermagic* coincidía, pero `insmod` lo
rechazaba con `Invalid module format` y, en `dmesg`, *«disagrees about version of symbol
module_layout»*. El diagnóstico exigió comparar ambas configuraciones símbolo a símbolo (19
diferencias, de las que solo una era relevante), confirmar que los CRC se calculan con `genksyms`
sobre texto preprocesado y no con `gendwarfksyms` sobre DWARF, y verificar que la gramática de
`genksyms` procesa atributos. La solución fue instalar GCC 13 y recompilar: el CRC de `module_layout`
cambió de `0x5bc3c700` a `0xadc86883`. Quedó una diferencia residual —el trío de opciones
`asm goto`— que **se predijo inocua y se documentó como predicción, no como hecho**, razonando que
protegen cuerpos de función y macros, no declaraciones de tipos, que es lo único que `genksyms`
procesa. Se confirmó al cargar el módulo.
