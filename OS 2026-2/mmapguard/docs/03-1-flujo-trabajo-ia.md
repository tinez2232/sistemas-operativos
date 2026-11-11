# 3.1 Definición del Flujo de Trabajo Asistido por IA

---

## 1. Premisa: la asimetría que define el método

El trabajo se desarrolló con un modelo de lenguaje con acceso a terminal (Claude Code) sobre la
máquina de desarrollo. Esa capacidad —ejecutar comandos, no solo redactar texto— es lo que determina
la forma del flujo, por una razón concreta:

> El modelo tiene conocimiento amplio del kernel de Linux pero **difuso en la versión**. El árbol de
> fuentes de la versión exacta está en el disco, a un `grep` de distancia.

Un modelo entrenado sobre dos décadas de código y documentación de kernel no distingue con fiabilidad
qué era cierto en 5.10, qué en 6.1 y qué en 6.18. Produce, de hecho, un promedio histórico: código
idiomático, convincente y a menudo correcto para *alguna* versión. Y el material publicado está
sesgado hacia lo antiguo, porque lleva más tiempo acumulándose.

Todo el flujo de trabajo es la consecuencia de tomarse esa asimetría en serio. La regla operativa
—establecida una sola vez y vigente durante toda la sesión— fue:

> **Antes de afirmar que una API existe o que un mecanismo está disponible, verificarlo contra
> `kernel-wsl/` y citar archivo y línea.**

No es una regla de estilo documental. Es la que hace que el conocimiento del modelo sea utilizable:
convierte una afirmación de memoria, que no se puede evaluar, en una afirmación con una referencia
que el lector puede abrir y desmentir.

---

## 2. División del trabajo

Conviene ser preciso sobre quién decidió qué, porque el enunciado pide un flujo de trabajo y no una
atribución de autoría difusa.

| Responsabilidad | Quién |
| :--- | :--- |
| Alcance, plazo y criterio de "suficiente" | Humano |
| Elección entre mapeo de solo lectura y mapeo escribible (Opción A) | Humano, sobre alternativas y consecuencias expuestas por el modelo |
| Qué planos se comprometen y cuál queda condicional | Humano |
| Enumeración de mecanismos viables y descarte razonado | Modelo |
| Redacción del código C y de las herramientas de usuario | Modelo |
| Verificación de APIs contra el árbol de fuentes | Modelo, con el árbol como oráculo |
| Ejecución de todo lo privilegiado: `insmod`, `rmmod`, `chmod` sobre `/dev` | Humano |
| Reporte de los fallos reales observados al cargar el módulo | Humano |
| Diseño de la medición y atribución de causa del sobrecosto | Modelo |

La última fila de la columna humana no es trivial. El problema más difícil de la sesión —el desajuste
de CRC de `MODVERSIONS`— solo se pudo diagnosticar porque el usuario ejecutó `insmod` en una sesión
con privilegios y trajo de vuelta el mensaje exacto de `dmesg`. El modelo no tenía TTY para
autenticar `sudo`; sin ese circuito de retorno, el síntoma habría sido invisible.

---

## 3. El flujo en cuatro fases

Cada fase tiene un criterio de salida explícito. Lo importante del flujo no es el orden —que es casi
obvio— sino que ninguna fase avanza sobre afirmaciones no verificadas de la anterior.

| Fase | Entrada | Actividad | Criterio de salida |
| :-: | :--- | :--- | :--- |
| 0 | Enunciado y repositorio de referencia | Reconocimiento: qué pide el parcial, qué entorno hay, qué del material de referencia es reutilizable | Un árbol de kernel que produce módulos **cargables**, verificado con un módulo trivial |
| 1 | Objetivos de seguridad del enunciado | Exploración arquitectónica: enumerar mecanismos, descartar con razón, asignar un plano por objetivo | Cada descarte respaldado por una comprobación contra el árbol, no por una intuición |
| 2 | Arquitectura cerrada | Generación del código C, **seguida de verificación API por API antes de compilar** | Compilación sin una sola advertencia con `-Wall -Wextra`, y carga efectiva del módulo |
| 3 | Módulo cargado | PoC de los vectores de ataque y medición del sobrecosto | Cifras reproducibles, con la causa del sobrecosto atribuida y demostrada |

La fase 0 terminó mucho más tarde de lo previsto, y ese desfase es el dato metodológico más útil de
todo el proyecto: entre tener el árbol de fuentes y tener un árbol que produce módulos cargables hubo
dos problemas no evidentes (el sufijo `+` de versión local y el CRC de `MODVERSIONS` por cambio de
compilador). Cualquier planificación que dé la fase 0 por hecha subestima el proyecto.

---

## 4. Rol de la IA en la exploración arquitectónica

**El valor no estuvo en proponer una solución, sino en producir el conjunto de descartes.**

Pedir "cómo protejo archivos mapeados con `mmap()`" devuelve una respuesta. Pedir "enumera los
mecanismos viables, descarta los que no sirven y di por qué" devuelve algo cualitativamente distinto:
cuatro rutas cerradas, cada una con la razón que la cierra.

| Mecanismo | Razón del descarte |
| :--- | :--- |
| Hook en la tabla de syscalls | Exige desarmar el bit WP de `CR0`; frágil y detectable |
| LSM propio como módulo cargable | `security_add_hooks()` no está exportada: no hay forma legítima |
| eBPF solo con `fentry`/`kprobes` | Observa pero **no puede denegar**: el valor de retorno no altera el flujo |
| `remap_pfn_range()` (lo que usa el material de referencia) | Obliga a mantener el archivo **completo** descifrado en RAM |

Esos descartes valen más que la elección, porque la elección se sigue de ellos. Y en un informe
evaluado, un descarte razonado es defendible ante una pregunta; una elección sin alternativas, no.

**Un caso en que la exploración cambió el diseño, no solo lo justificó.** El descarte de
`remap_pfn_range()` empezó como una comparación de rendimiento y terminó siendo de seguridad: si las
páginas se entregan de a una bajo demanda, la ventana de exposición en memoria física se reduce de
"todo el archivo" a "las páginas realmente tocadas". La paginación por demanda, que en el material
de referencia es una optimización, se convirtió en un **control de minimización de exposición**. Eso
quedó medido: un cliente que lee 64 bytes de un archivo de 2 páginas deja 4.096 bytes en claro en
RAM, no 8.192.

**Un caso en que la exploración evitó trabajo inútil.** Antes de escribir una línea de eBPF se
verificó la viabilidad contra la configuración real: `CONFIG_BPF_LSM=y` está activo, pero
`CONFIG_LSM` vale
`"landlock,lockdown,yama,loadpin,safesetid,integrity,selinux,apparmor,tomoyo"` y **no incluye
`bpf`**. Los ganchos existen y no son adjuntables. Un programa BPF LSM habría compilado, habría
pasado el verificador, y habría fallado al adjuntarse. El plano 2 se declaró condicional con esa
evidencia en la mano, no por falta de tiempo.

**Y el caso en contra, que importa igual.** En esta misma fase el modelo estuvo a punto de hacer
descartar la ruta del módulo cargable —el 40% de la nota— por una lectura truncada de un bloque de
`Kconfig` (alucinación A3 de la sección 3.2). La exploración arquitectónica es donde una conclusión
equivocada es más caro, porque no la contradice ningún compilador: se descubre semanas después, si se
descubre. De ahí que el criterio de salida de la fase 1 sea una comprobación, no un consenso.

---

## 5. Rol de la IA en la generación y verificación de código C de bajo nivel

Aquí está el hallazgo central de esta sección:

> **Generar y verificar son dos actividades con fiabilidad muy distinta, aunque las ejecute el mismo
> modelo.** La primera produjo código plausible con cinco APIs inexistentes. La segunda, con el árbol
> de fuentes como oráculo, las encontró todas antes de compilar.

El primer borrador de `mmapguard.c` era idiomático, estaba bien estructurado y contenía:

| Afirmación generada | Realidad en 6.18.33.2 |
| :--- | :--- |
| `.llseek = no_llseek` | Eliminado del kernel en 6.12 |
| `#include <asm/unaligned.h>` | Se mudó a `<linux/unaligned.h>` |
| `vma->vm_flags \|= ...` | `vm_flags` es `const` en una unión: hay que usar `vm_flags_set/clear` |
| `class_create(THIS_MODULE, name)` | Recibe un solo argumento desde 6.4 |
| `kallsyms_lookup_name()` | Ya no está exportada |

Ninguna de las cinco es un error de razonamiento. Todas fueron correctas, y están en innumerables
ejemplos publicados. El modo de fallo no es "el modelo se inventa cosas": es **el modelo escribe
código correcto para una versión que no es la nuestra**.

Eso tiene una consecuencia directa sobre el flujo: la generación se trató como un borrador, nunca
como un entregable, y entre generar y compilar se interpuso un paso obligatorio de verificación (el
prompt P4 de la sección 3.2) cuyo único trabajo era contrastar cada símbolo usado contra el árbol.

**Por qué no basta con "que compile".** Es tentador saltarse la verificación y dejar que el
compilador haga de filtro. No alcanza, y hay dos pruebas de ello en esta misma sesión:

- `touch .scmversion`, recomendado para corregir el sufijo de versión, **no produce ningún error**.
  Simplemente no hace nada en 6.18. El fallo aparece mucho después, al cargar el módulo, con un
  mensaje que no menciona la versión local.
- El desajuste de CRC de `MODVERSIONS` por compilar con GCC 15 en lugar de GCC 13 compila
  perfectamente. Falla en `insmod`, con `Invalid module format`, y la causa real solo aparece en
  `dmesg` como *"disagrees about version of symbol module_layout"*.

El compilador filtra los errores de sintaxis y de tipos. No filtra los errores de **versión, de
configuración y de entorno**, que en desarrollo de kernel son los que cuestan el tiempo. Por eso el
oráculo del flujo es el árbol de fuentes, no el `make`.

---

## 6. Rol de la IA en los casos de prueba y las PoC controladas

El sesgo a vencer en esta fase es distinto y más incómodo: **el modelo tiende a escribir pruebas que
pasan y a reportar cifras favorables.** Las tres contramedidas que se aplicaron fueron explícitas.

**(a) Blindar la medición contra el optimizador.** Esta exigencia no fue teórica: salió de haber
encontrado el fallo en el banco de pruebas del repositorio de referencia
(`paso5_benchmark_comparativo`). Compila con `-O2` y nunca consume el checksum del bucle de `mmap`,
de modo que GCC elimina el bucle entero por código muerto. Reporta **3.134.335 MB/s y 0 fallos de
página** para 64 MB —ambas cifras físicamente imposibles— y concluye que `mmap` es 380 veces más
rápido. Recompilado con `-O0`: 11.552 MB/s, 1.024 fallos, ventaja real ~1,5x. Nuestro `mg_bench`
imprime todos los checksums precisamente por eso.

Es un ejemplo valioso porque el código es correcto, está bien escrito y el fallo no está en el
programa sino en la **metodología de medición**. Un modelo que genera benchmarks reproduce ese patrón
con facilidad, porque es el patrón que abunda.

**(b) Rechazar la cifra única.** La primera medición de latencia de `mmap()` dio 43,2 µs frente a
3,2 µs: un factor de 13 que invitaba a un titular. Repetida once veces y tomando la mediana, la
diferencia quedó por debajo del ruido del planificador. Se adoptó la mediana de once muestras como
método, y se reportó el resultado negativo.

**(c) No esconder el sobrecosto, atribuirlo.** El resultado honesto es que `mmapguard` cuesta
**17,60x** en acceso secuencial. La aportación del flujo no fue maquillarlo sino explicarlo: `mmap()`
ordinario puebla 16 PTE por fallo mediante *fault-around* y nosotros una, de modo que hacemos 4.097
fallos donde el otro hace 257. La razón 15,9x explica casi todo el 17,60x: **el costo dominante no es
AES, es la ausencia de fault-around.** La prueba es que al variar el patrón de acceso a un salto de
64 páginas, donde ambos hacen 65 fallos, el sobrecosto cae a 1,94x. De ahí sale una propuesta
concreta y cuantificada —implementar `.map_pages` bajaría el secuencial a ~9x— que una cifra
maquillada no habría permitido formular.

**Lo que no se cubrió, dicho explícitamente.** De las siete PoC diseñadas en la sección 3.3 se
ejecutaron las seis que aplican (A0 a A5, **8 de 8 PASS**), que cubren las preguntas 4, 5, 2, 3, 8 y
9. A6 no aplica a un mapeo de solo lectura porque exige el modo AEAD que la sección de trabajo futuro
documenta, y el informe lo declara como «no aplicable» en la tabla correspondiente en vez de
presentarlo como verificado.

---

## 7. Lo que este flujo no cubre

Un flujo de trabajo honesto incluye sus límites.

**Detecta afirmaciones falsas, no afirmaciones no verificadas que resultan ciertas.** La alucinación
A5 de la sección 3.2 —afirmar la causa del desajuste de CRC antes de comprobarla— acertó. Y
precisamente por eso no la detectó nada: una afirmación causal confiada y correcta es, para quien
lee, indistinguible de una verificada. El flujo reduce la probabilidad de error, no la de exceso de
confianza.

**Verifica el presente, no el futuro.** Citar `archivo:línea` del árbol 6.18.33.2 garantiza que la
afirmación es cierta hoy, en esta versión. `no_llseek` también tenía su archivo y su línea en 6.11.
Toda la verificación de este informe caduca con la versión.

**Depende de que exista un oráculo.** Funcionó porque el árbol de fuentes está en el disco y es la
verdad. Donde no hay un oráculo así —decisiones de diseño, juicios de suficiencia, trade-offs— el
flujo no aporta nada y la responsabilidad vuelve entera al humano. La elección del mapeo de solo
lectura es de ese tipo: el modelo expuso la consecuencia criptográfica de reutilizar el nonce en modo
CTR, pero aceptar perder la escritura a cambio de un cifrado correcto fue una decisión de alcance,
no un resultado verificable.

---

**Documentos relacionados:** el procedimiento de verificación detallado y el registro de
alucinaciones están en [`03-2-prompts-validacion.md`](03-2-prompts-validacion.md); las decisiones
arquitectónicas y sus descartes, en [`03-3-arquitectura.md`](03-3-arquitectura.md); las mediciones,
en [`04-resultados.md`](04-resultados.md).
