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

### Por qué la distinción entre las secciones 3 y 4 es el hallazgo central

Las alucinaciones de la sección 3 se detectaron **porque alguien fue a mirar**. Ninguna se delató
sola: `no_llseek` habría fallado en compilación, pero `.scmversion` no produjo error alguno y la
conclusión errónea sobre el árbol de fuentes era internamente coherente y plausible.

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
