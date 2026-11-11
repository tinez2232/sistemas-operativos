# 3.2 Metodología de Ingeniería de Prompts y Verificación Crítica

---

## 1. Naturaleza del registro

La solución se desarrolló en una sesión de trabajo asistida por un modelo de lenguaje con acceso a
terminal (Claude Code), de forma **conversacional y no mediante prompts pre-redactados**. Los
prompts que se reproducen en la sección 2 son literales; la descomposición en contexto, rol,
restricción y resultado esperado es un análisis posterior de qué función cumplió cada uno.

Se hace explícita esta aclaración porque la alternativa —redactar a posteriori prompts elaborados
que nunca se usaron— produciría una tabla más vistosa y completamente falsa. El valor metodológico
de esta sección está en el procedimiento de verificación, no en la elegancia de las peticiones.

---

## 2. Tabla de prompts estructurados

| # | Prompt (literal) | Contexto | Rol asignado | Restricción | Resultado esperado |
| :-: | :--- | :--- | :--- | :--- | :--- |
| P1 | *"clona este repo ... creo que con lo que contiene lo puedes correr para verificar la función mmap si no me equivoco pero si algo sale mal avísame"* | Exploración inicial del material de referencia | Verificador autónomo | Reportar fallos en vez de ocultarlos | Diagnóstico del estado real del repositorio |
| P2 | *"en este pdf está toda la información de lo que hay que hacer ... por ahora revisa todo el reporte"* | Comprensión del enunciado | Analista de requisitos | No actuar todavía, solo analizar | Desglose de entregables, pesos y bloqueos |
| P3 | *"hagamos la arquitectura como recomiendas"* | Diseño de la solución | Arquitecto de sistemas | Justificar cada descarte, no solo la elección | Documento 3.3 con matriz de compensaciones |
| P4 | *"arranca con las 10 preguntas"* | Parte I del parcial | Experto en kernel Linux | Fundamentar contra el árbol real, citando archivo y línea | 10 respuestas verificables |
| P5 | *"opción A, empieza con el mmapguard.ko"* | Implementación | Desarrollador de kernel | Opción A = solo lectura; sin advertencias de compilación | Módulo funcional y cargable |
| P6 | *"sigue con la medición del plano 3 que vale el 40%"* | Sección 4.2 | Ingeniero de rendimiento | Medición rigurosa, no cifras vistosas | Benchmark con metodología defendible |

El patrón que emerge: los prompts son **cortos y de alto nivel**, y la restricción crítica aparece
en la conversación previa, no dentro del prompt. La restricción más productiva de toda la sesión
—"verificar contra el árbol del kernel antes de afirmar"— se estableció una vez y gobernó todo el
trabajo posterior.

---

## 3. Alucinaciones técnicas detectadas

Casos en los que el modelo produjo una afirmación o un código incorrecto que hubo que corregir.

### A1 — `no_llseek` en `struct file_operations` (función eliminada)

**Qué ocurrió.** El primer borrador de `mmapguard.c` incluía `.llseek = no_llseek,` en su
`file_operations`.

**Por qué es incorrecto.** `no_llseek` fue eliminado del kernel en 6.12. Una búsqueda sobre
`include/linux/fs.h` del árbol 6.18.33.2 no devuelve ninguna coincidencia.

**Cómo se detectó.** Por inspección del árbol antes de compilar, como parte del procedimiento de la
sección 5.

**Impacto evitado.** Error de compilación. Es un caso representativo: `no_llseek` aparece en
prácticamente todos los ejemplos de drivers de caracteres publicados antes de 2024, de modo que es
abrumadoramente frecuente en el material de entrenamiento.

### A2 — `touch .scmversion` como solución al sufijo de versión

**Qué ocurrió.** Al detectar que el kernel compilado producía `6.18.33.2-microsoft-standard-WSL2+`
en lugar de la versión sin el `+`, el modelo recomendó crear un archivo `.scmversion` vacío en la
raíz del árbol.

**Por qué es incorrecto.** Es la solución documentada en innumerables guías y fue correcta durante
años, pero `scripts/setlocalversion` de 6.18 ya no consulta ese archivo.

**Cómo se detectó.** Empíricamente: se aplicó y la versión no cambió.

**Solución real.** Desactivar la detección de git en el árbol de compilación, de modo que
`scm_version()` retorne vacío.

**Lección.** Esta alucinación es más peligrosa que A1 porque la recomendación **no produce ningún
error**: simplemente no hace nada, y el fallo aparece mucho después, al cargar el módulo, con un
mensaje que no apunta a su causa.

### A3 — Conclusión errónea por lectura truncada del Kconfig

**Qué ocurrió.** Al investigar por qué la configuración regenerada difería de la original, el modelo
imprimió solo tres líneas del bloque `GCC_ASM_GOTO_OUTPUT_BROKEN`, leyó
`default y if GCC_VERSION < 110500`, observó que GCC 13.2 no cumple esa condición, y **concluyó que
el árbol de fuentes no correspondía al kernel en ejecución**.

**Por qué es incorrecto.** El bloque tiene tres cláusulas `default`. La tercera,
`default y if GCC_VERSION >= 130000 && GCC_VERSION < 130300`, sí incluye a GCC 13.2.

**Cómo se detectó.** Imprimiendo el bloque completo.

**Impacto evitado.** Grave. La conclusión equivocada habría llevado a abandonar la ruta del módulo
cargable, que es el 40% de la nota, por un problema inexistente.

**Lección.** El error no fue de conocimiento sino de **evidencia insuficiente**: una ventana de
contexto recortada produjo una afirmación formalmente correcta sobre datos incompletos.

### A4 — Prueba de capacidad del compilador mal construida

**Qué ocurrió.** Para comprobar si GCC 13 soportaba el atributo `counted_by`, el modelo escribió una
prueba de shell encadenada con `&&`. GCC 13 emite *warning: 'counted_by' attribute directive
ignored* pero termina con código 0, de modo que la prueba reportó "gcc-13 SÍ lo soporta", que es lo
contrario de la verdad.

**Cómo se detectó.** Al leer el test real del Kconfig, que usa `-Werror=attributes`. Reproducido con
ese flag, gcc-13 falla y gcc-15 pasa, confirmando el comportamiento esperado.

**Impacto evitado.** Habría descartado gcc-13 como solución al problema de CRC, que era la solución
correcta.

**Lección.** No basta con ejecutar una prueba: hay que replicar **las condiciones exactas** del
sistema que se intenta reproducir.

### A5 — Confianza causal por delante de la evidencia

**Qué ocurrió.** El modelo afirmó que `CONFIG_CC_HAS_COUNTED_BY` era la causa del desajuste de CRC
y lo presentó como hecho establecido, antes de haber verificado que `genksyms` procesa atributos en
su gramática.

**Resultado.** La hipótesis resultó correcta, pero la afirmación precedió a la evidencia.

**Por qué se registra igualmente.** Es el modo de fallo más difícil de detectar, porque no hay nada
que corregir cuando se acierta. Una afirmación causal confiada y no verificada es indistinguible
—para quien lee— de una verificada, y solo se distingue cuando resulta falsa.

### A6 — El `errno` de `mprotect`: `EPERM` escrito de memoria

**Qué ocurrió.** Al documentar la PoC A3 se afirmó que `mprotect(PROT_WRITE)` sobre un VMA sin
`VM_MAYWRITE` devuelve `EPERM`. Es el error que devuelve «no tienes permiso» en la intuición de
cualquiera, y encajaba con la narrativa de la defensa. La afirmación se propagó a **seis
documentos**, incluido el bloque de «salida esperada» del `README`, que llegó a mostrar
`Operation not permitted` como si fuera una captura real.

**Lo que dice el árbol.** `mm/mprotect.c:959-963`:

```c
/* newflags >> 4 shift VM_MAY% in place of VM_% */
if ((newflags & ~(newflags >> 4)) & VM_ACCESS_FLAGS) {
        error = -EACCES;
        break;
}
```

El núcleo contrasta cada permiso pedido contra su `VM_MAY*` correspondiente y devuelve **`EACCES`**,
no `EPERM`.

**Cómo se detectó.** No lo detectó una relectura: lo detectó la PoC. `mg_poc.py` compara el `errno`
real contra el esperado en vez de limitarse a comprobar que la llamada falló, y marcó A3 como
`INCONCLUSIVE` con el mensaje «denegado, pero con Permission denied (errno 13/EACCES) en vez de
EPERM». El cliente en C nunca lo habría delatado, porque imprime `strerror(errno)` sin compararlo
con nada: mostraba el error correcto, y aun así el documento que lo acompañaba decía otra cosa.

**La lección operativa.** Una PoC que solo verifica que el ataque *falla* no distingue entre fallar
por el motivo previsto y fallar por otro. Afirmar el `errno` exacto convierte la prueba en un
oráculo capaz de contradecir a la documentación.

### A7 — Suponer que el módulo ve las banderas que pidió el usuario

**Qué ocurrió.** La PoC A2 daba por hecho que `mmap(MAP_SHARED)` sobre `/dev/mmapguard` tenía que
ser rechazado por `mg_mmap()`, porque el módulo comprueba `vma->vm_flags & VM_SHARED`. La primera
ejecución del banco marcó **FAIL**: el mapeo fue concedido.

**Lo que dice el árbol.** `mm/mmap.c:462-464`, dentro de `do_mmap()`:

```c
vm_flags |= VM_SHARED | VM_MAYSHARE;
if (!(file->f_mode & FMODE_WRITE))
        vm_flags &= ~(VM_MAYWRITE | VM_SHARED);
```

El dispositivo se abre en `O_RDONLY`, de modo que **el núcleo retira `VM_SHARED` y `VM_MAYWRITE`
antes de llamar a `f_op->mmap`**. El módulo nunca llega a ver la bandera que creía estar filtrando,
y lo que concede es un mapeo privado de solo lectura, tan restringido como el del caso
`MAP_PRIVATE`.

**Por qué no era un fallo de seguridad, y por qué importa igual.** El VMA resultante aparece como
`r--p` en `/proc/self/maps` y rechaza `mprotect(PROT_WRITE)`; la PoC corregida comprueba las dos
cosas en vez de suponerlas. Pero el episodio deja ver algo que ningún repaso del código del módulo
habría mostrado: **la comprobación de `VM_SHARED` de `mg_mmap()` es inalcanzable con un descriptor
de solo lectura.** Para ejercitarla hay que abrir el dispositivo en `O_RDWR`, que es justo lo que
hace ahora el tercer caso de A2.

**La lección operativa.** El conjunto de banderas que recibe un `f_op->mmap` no es el que pidió el
proceso: es el que el núcleo dejó después de aplicar sus propias reglas. Razonar sobre una política
de kernel leyendo solo el código del módulo es razonar sobre una entrada que no existe.

### A8 — El instrumento mintió: `r--s` no significa `VM_SHARED`

**Qué ocurrió.** La corrección de A7 se apoyó en `/proc/self/maps`: si el núcleo retiraba
`VM_SHARED`, el mapeo debía aparecer como `r--p`. La PoC corregida volvió a marcar **FAIL**, ahora
con el mensaje «el VMA quedó compartido (`r--s`)». Dos intentos, dos veredictos equivocados sobre
un módulo que nunca estuvo mal.

**Lo que dice el árbol.** `fs/proc/task_mmu.c:454`:

```c
seq_putc(m, flags & VM_MAYSHARE ? 's' : 'p');
```

El cuarto carácter depende de **`VM_MAYSHARE`**, no de `VM_SHARED`. Y `do_mmap()` retira
`VM_MAYWRITE | VM_SHARED` dejando `VM_MAYSHARE` intacto (`mm/mmap.c:463-464`). Un mapeo ya degradado
a privado sigue mostrándose como `s`.

**El instrumento correcto.** `/proc/self/smaps` imprime una línea `VmFlags:` con un mnemónico por
bandera (`fs/proc/task_mmu.c:1152-1159`): `rd`=`VM_READ`, `wr`=`VM_WRITE`, `sh`=`VM_SHARED`,
`mw`=`VM_MAYWRITE`, `ms`=`VM_MAYSHARE`. Ahí las dos banderas sí se distinguen. Comprobado sobre un
archivo ordinario, sin el módulo de por medio:

| Mapeo | `maps` | `VmFlags` de `smaps` |
| :--- | :---: | :--- |
| `O_RDONLY` + `MAP_SHARED` | `r--s` | `me mr ms rd sd` — **sin `sh`, sin `mw`** |
| `O_RDWR` + `MAP_SHARED` | `r--s` | `me mr ms mw rd sd` **`sh`** |
| `O_RDONLY` + `MAP_PRIVATE` | `r--p` | `me mr mw rd sd` |

Las dos primeras filas son indistinguibles en `maps` y describen VMA con permisos distintos. La
tercera añade el matiz que sostiene la pregunta 8: un mapeo **privado** desde un `fd` de solo
lectura conserva `mw`, y por eso admite `PROT_WRITE` por copia en escritura, mientras que el
compartido lo pierde.

**La lección operativa.** Las tres alucinaciones de esta tanda —A6, A7 y A8— comparten estructura:
una abstracción de usuario (`errno`, las banderas de `mmap`, la columna de `maps`) se dio por
equivalente a la estructura de kernel subyacente, y no lo era. La verificación no termina al mirar
el código del módulo ni al mirar la salida de una herramienta: termina al comprobar **qué imprime
esa herramienta exactamente**, que es también código del kernel y tiene sus propias reglas.

### Sobre el ejemplo de `CR0` que cita el enunciado

El enunciado menciona, como alucinación típica, deshabilitar el bit de protección contra escritura
de `CR0` sin control de concurrencia. **No se produjo en esta sesión**, porque la evaluación
arquitectónica descartó la ruta de hook en la *syscall table* antes de solicitar código para ella
(ver `03-3-arquitectura.md`, sección 2.1). Se deja constancia de que no ocurrió en lugar de atribuirse
una detección que no tuvo lugar.

---

## 4. Hallazgos de verificación proactiva

Estos **no son alucinaciones**: son hechos comprobados contra el árbol *antes* de escribir código o
documentación, de modo que nunca llegaron a convertirse en errores. Se documentan porque constituyen
la evidencia de que el procedimiento de la sección 5 se aplicó.

| # | Hecho verificado | Archivo y línea | Por qué importaba |
| :-: | :--- | :--- | :--- |
| V1 | `class_create()` recibe un solo argumento desde 6.4 | `include/linux/device/class.h:226` | El código de ejemplo habitual pasa `THIS_MODULE` |
| V2 | `vm_flags` es `const` dentro de una unión; hay que usar `vm_flags_set/clear` | `include/linux/mm.h:808,815`, `mm_types.h:835` | `vma->vm_flags \|= ...` no compila en 6.x |
| V3 | `security_add_hooks()` no está exportada | `security/` | Descarta registrar un LSM desde un módulo cargable |
| V4 | `kallsyms_lookup_name` y `sys_call_table` no exportadas | `kernel/kallsyms.c`, `arch/x86/` | Sustenta el descarte del hook de syscalls |
| V5 | El gancho LSM `file_mmap` **no existe**; el real es `mmap_file` | `include/linux/lsm_hook_defs.h:199` | El propio enunciado usa el nombre antiguo |
| V6 | `put_unaligned_be64()` vive en `<linux/unaligned.h>` | `include/linux/unaligned.h` | `<asm/unaligned.h>` ya no está en el árbol |
| V7 | `.page_mkwrite` disponible en `vm_operations_struct` | `include/linux/mm.h:638` | Habilita la defensa en profundidad del plano 1 |
| V8 | 6.18 añade `f_op->mmap_prepare`, excluyente con `.mmap` | `include/linux/fs.h:2285,2314,2385` | Punto de intercepción nuevo, posterior al corte de conocimiento habitual |
| V9 | `do_mmap()` retira `VM_SHARED` y `VM_MAYWRITE` si el `fd` no tiene `FMODE_WRITE` | `mm/mmap.c:462-464` | Verificado **a posteriori**, a raíz del FAIL de A2 (ver A7) |
| V10 | `mprotect` devuelve `-EACCES`, no `-EPERM`, al faltar `VM_MAYWRITE` | `mm/mprotect.c:959-963` | Verificado **a posteriori**, a raíz del INCONCLUSIVE de A3 (ver A6) |
| V11 | El carácter `s`/`p` de `/proc/PID/maps` depende de `VM_MAYSHARE`, no de `VM_SHARED` | `fs/proc/task_mmu.c:454` | Verificado **a posteriori**, a raíz del segundo FAIL de A2 (ver A8) |
| V12 | `smaps` sí separa ambas: `sh`=`VM_SHARED`, `ms`=`VM_MAYSHARE`, `mw`=`VM_MAYWRITE` | `fs/proc/task_mmu.c:1152-1159` | Es el instrumento con el que la PoC afirma el estado real del VMA |

### Por qué la distinción entre las secciones 3 y 4 es el hallazgo central

Las alucinaciones de la sección 3 se detectaron **porque alguien fue a mirar**. Ninguna se delató
sola: `no_llseek` habría fallado en compilación, pero `.scmversion` no produjo error alguno y la
conclusión errónea sobre el árbol de fuentes era internamente coherente y plausible.

Las tres últimas, A6, A7 y A8, son la excepción que confirma la regla, y por eso V9 a V12 están
marcadas como verificaciones *a posteriori*. No las encontró nadie leyendo: las encontró el banco
de pruebas al ejecutarse, porque afirma valores concretos —el `errno` exacto, las banderas reales
del VMA— en lugar de limitarse a comprobar que el ataque fracasa. Una prueba que solo exige «esto
debe fallar» habría dado PASS en los tres casos y habría dejado intactos los seis documentos
equivocados. El oráculo no estaba en el árbol del kernel: estaba en exigirle a la prueba que fuera
específica.

Conviene no suavizar el episodio: el banco marcó FAIL **dos veces seguidas** sobre un módulo que
siempre estuvo bien. La primera porque la prueba esperaba que el módulo viera una bandera que el
núcleo ya había retirado; la segunda porque el instrumento elegido para comprobarlo no medía la
bandera que decía medir. Un veredicto rojo no es prueba de un defecto en el código: es prueba de un
desacuerdo entre el código y lo que alguien creía del código, y el defecto puede estar en cualquiera
de los dos lados. Cada una de esas dos iteraciones terminó en una cita del árbol, que es la única
forma de cerrar el desacuerdo.

El riesgo real de usar asistentes de IA en programación de sistemas no son las afirmaciones
obviamente falsas, que el compilador filtra, sino las **plausibles y silenciosas**: APIs que
existieron, soluciones que funcionaron hace tres versiones, y razonamientos correctos sobre datos
incompletos. El único filtro efectivo contra esa clase de error es la verificación contra la fuente,
que es precisamente lo que produjo la sección 4.

---

## 5. Procedimiento de validación manual y compilación

El procedimiento se estableció al principio y se aplicó de forma sistemática.

### 5.1 Verificación contra el árbol del kernel

Se clonó `linux-msft-wsl-6.18.33.2`, que corresponde exactamente al kernel en ejecución
(`6.18.33.2-microsoft-standard-WSL2`). **Ninguna afirmación sobre una API del kernel se dio por
válida de memoria**: cada una se comprobó por inspección y se citó con archivo y línea, tanto en las
respuestas de la Parte I como en la arquitectura y el código.

### 5.2 Compilación contra los headers de la distribución utilizada

Requisito explícito del enunciado, y el punto que más esfuerzo costó.

El kernel de WSL2 lo compiló Microsoft con GCC 13.2. Al regenerar la configuración con el GCC 15.2
del sistema, `make olddefconfig` autodetectó capacidades distintas del compilador —entre ellas
`CONFIG_CC_HAS_COUNTED_BY`— lo que altera el texto de las cabeceras que `genksyms` convierte en CRC
de símbolo. El módulo compilaba y su *vermagic* coincidía, pero `insmod` lo rechazaba:

```
insmod: ERROR: could not insert module: Invalid module format
dmesg:  kmod_mmap: disagrees about version of symbol module_layout
```

El diagnóstico exigió: leer el log del kernel, comparar ambas configuraciones símbolo a símbolo (19
diferencias, de las que solo una era funcionalmente relevante), confirmar que los CRC se calculan
con `genksyms` sobre texto preprocesado y no con `gendwarfksyms` sobre DWARF, y verificar que la
gramática de `genksyms` procesa atributos. La solución fue instalar GCC 13 y recompilar: las
diferencias bajaron de 19 a 13 y el CRC de `module_layout` cambió de `0x5bc3c700` a `0xadc86883`.

Quedó una diferencia residual que **se predijo inocua y se documentó como predicción, no como
hecho**: el trío de opciones `asm goto`, que GCC 13.4 no reproduce respecto de GCC 13.2. El
razonamiento fue que esas opciones protegen cuerpos de función y macros, no declaraciones de tipos,
que es lo único que `genksyms` procesa. La predicción se confirmó al cargar el módulo con éxito.

### 5.3 Compilación sin advertencias

Exigido por el punto 4.1. Tanto el módulo como las herramientas compilan con `-Wall -Wextra` sin una
sola advertencia.

### 5.4 Validación empírica

Ningún componente se dio por bueno hasta ejecutarlo: carga efectiva del módulo con `insmod`,
verificación en `dmesg`, ejecución de los tres PoCs y comprobación de los contadores de auditoría.

### 5.5 Validación cruzada de las mediciones

Tres salvaguardas específicas, cada una contra un defecto concreto, detalladas en
`04-resultados.md`: consumo obligatorio del checksum para impedir la eliminación del bucle por
código muerto, once muestras con mediana en lugar de una sola, y caché caliente verificada mediante
`majflt = 0`.

La primera salvaguarda nació de un hallazgo concreto: el benchmark del paso 5 del repositorio de
referencia **sí** sufre ese defecto, y reporta 3.134.335 MB/s con 0 fallos de página para un
conjunto de 64 MB. Recompilado con `-O0`, el mismo programa reporta 11.552 MB/s y 1.024 fallos. La
ventaja real de `mmap` sobre `read()` en esa prueba es de aproximadamente 1,5x, no los 380x
anunciados.
