# 3.1 Definición del Flujo de Trabajo Asistido por IA

## 1. Premisa: la asimetría que define el método

El trabajo se desarrolló con un modelo de lenguaje con acceso a terminal (Claude Code) sobre la
máquina de desarrollo. Esa capacidad —ejecutar comandos, no solo redactar— determina la forma del
flujo por una razón concreta:

> El modelo tiene conocimiento amplio del kernel de Linux pero **difuso en la versión**. El árbol de
> fuentes de la versión exacta está en el disco, a un `grep` de distancia.

Un modelo entrenado sobre dos décadas de código de kernel no distingue con fiabilidad qué era cierto
en 5.10, qué en 6.1 y qué en 6.18: produce un promedio histórico, idiomático y convincente, sesgado
hacia lo antiguo porque lleva más tiempo acumulándose. De ahí la regla operativa, establecida una
sola vez y vigente toda la sesión:

> **Antes de afirmar que una API existe, verificarlo contra `kernel-wsl/` y citar archivo y línea.**

No es una regla de estilo: convierte una afirmación de memoria, que no se puede evaluar, en una
afirmación con una referencia que el lector puede abrir y desmentir.

## 2. División del trabajo

| Responsabilidad | Quién |
| :--- | :--- |
| Alcance, plazo y criterio de «suficiente» | Humano |
| Elección del mapeo de solo lectura (Opción A) | Humano, sobre consecuencias expuestas por el modelo |
| Qué planos se comprometen y cuál queda condicional | Humano |
| Enumeración de mecanismos viables y descarte razonado | Modelo |
| Redacción del código C y de las herramientas | Modelo |
| Verificación de APIs contra el árbol de fuentes | Modelo, con el árbol como oráculo |
| Ejecución de lo privilegiado: `insmod`, `rmmod`, `chmod` en `/dev` | Humano |
| Reporte de los fallos reales observados al cargar | Humano |
| Diseño de la medición y atribución del sobrecosto | Modelo |

Las dos últimas filas humanas no son triviales: el problema más difícil de la sesión —el desajuste de
CRC de `MODVERSIONS`— solo se pudo diagnosticar porque el usuario ejecutó `insmod` con privilegios y
trajo el mensaje exacto de `dmesg`. Sin ese circuito de retorno el síntoma habría sido invisible.

## 3. El flujo en cuatro fases

Lo importante no es el orden —que es obvio— sino que ninguna fase avanza sobre afirmaciones no
verificadas de la anterior.

| Fase | Actividad | Criterio de salida |
| :-: | :--- | :--- |
| 0 | Reconocimiento: qué pide el parcial, qué entorno hay, qué es reutilizable | Un árbol de kernel que produce módulos **cargables**, verificado con un módulo trivial |
| 1 | Exploración arquitectónica: enumerar mecanismos, descartar con razón, asignar un plano por objetivo | Cada descarte respaldado por una comprobación contra el árbol, no por una intuición |
| 2 | Generación del código C, **con verificación API por API antes de compilar** | Compilación sin una advertencia con `-Wall -Wextra` y carga efectiva |
| 3 | PoC de los vectores de ataque y medición del sobrecosto | Cifras reproducibles, con la causa del sobrecosto atribuida y demostrada |

La fase 0 terminó mucho más tarde de lo previsto, y ese desfase es el dato metodológico más útil del
proyecto: entre tener el árbol y tener un árbol que produce módulos cargables hubo dos problemas no
evidentes —el sufijo `+` de versión local y el CRC de `MODVERSIONS`—. Cualquier planificación que dé
la fase 0 por hecha subestima el proyecto.

## 4. Rol de la IA en la exploración arquitectónica

**El valor no estuvo en proponer una solución, sino en producir el conjunto de descartes.** Pedir
«cómo protejo archivos mapeados» devuelve una respuesta; pedir «enumera los mecanismos, descarta los
que no sirven y di por qué» devuelve cuatro rutas cerradas, cada una con la razón que la cierra:

| Mecanismo | Razón del descarte |
| :--- | :--- |
| Hook en la tabla de syscalls | Exige desarmar el bit WP de `CR0`; frágil y detectable |
| LSM propio como módulo cargable | `security_add_hooks()` no está exportada: no hay vía legítima |
| eBPF solo con `fentry`/`kprobes` | Observa pero **no puede denegar**: su retorno se ignora |
| `remap_pfn_range()` (lo del material de referencia) | Obliga a mantener el archivo **completo** descifrado en RAM |

Un descarte razonado es defendible ante una pregunta; una elección sin alternativas, no.

**Un caso en que la exploración cambió el diseño, no solo lo justificó.** El descarte de
`remap_pfn_range()` empezó como comparación de rendimiento y terminó siendo de seguridad: si las
páginas se entregan de a una bajo demanda, la ventana de exposición en memoria física baja de «todo
el archivo» a «las páginas tocadas». La paginación por demanda, que en el material de referencia es
una optimización, se convirtió en un **control de minimización de exposición**.

**Un caso en que evitó trabajo inútil.** Antes de escribir una línea de eBPF se verificó la
viabilidad real: `CONFIG_BPF_LSM=y`, pero `CONFIG_LSM` no incluye `bpf`. Los ganchos existen y **no
son adjuntables**. Un programa BPF LSM habría compilado, habría pasado el verificador y habría
fallado al adjuntarse. El plano 2 se declaró condicional con esa evidencia, no por falta de tiempo.

**Y el caso en contra, que importa igual.** En esta misma fase el modelo estuvo a punto de hacer
descartar la ruta del módulo cargable —el 40 % de la nota— por una lectura truncada de un bloque de
`Kconfig` (alucinación A3 de 3.2). La exploración arquitectónica es donde una conclusión equivocada
sale más cara, porque no la contradice ningún compilador. De ahí que el criterio de salida de la
fase 1 sea una comprobación, no un consenso.

## 5. Rol de la IA en el código C de bajo nivel

> **Generar y verificar son dos actividades con fiabilidad muy distinta, aunque las ejecute el mismo
> modelo.** La primera produjo código plausible con cinco APIs inexistentes. La segunda, con el árbol
> como oráculo, las encontró todas antes de compilar.

| Afirmación generada | Realidad en 6.18.33.2 |
| :--- | :--- |
| `.llseek = no_llseek` | Eliminado del kernel en 6.12 |
| `#include <asm/unaligned.h>` | Se mudó a `<linux/unaligned.h>` |
| `vma->vm_flags \|= ...` | `vm_flags` es `const` en una unión: hay que usar `vm_flags_set/clear` |
| `class_create(THIS_MODULE, name)` | Recibe un solo argumento desde 6.4 |
| `kallsyms_lookup_name()` | Ya no está exportada |

Ninguna de las cinco es un error de razonamiento: todas fueron correctas y están en innumerables
ejemplos publicados. El modo de fallo no es «el modelo se inventa cosas», es **el modelo escribe
código correcto para una versión que no es la nuestra**. Consecuencia sobre el flujo: la generación
se trató como borrador, nunca como entregable, y entre generar y compilar se interpuso un paso
obligatorio de verificación símbolo por símbolo. No basta con dejar que filtre el compilador: este
filtra sintaxis y tipos, no errores de **versión, configuración y entorno**, que en kernel son los
que cuestan el tiempo —de ahí que el oráculo del flujo sea el árbol de fuentes y no el `make`.

## 6. Rol de la IA en las PoC controladas

El sesgo a vencer aquí es más incómodo: **el modelo tiende a escribir pruebas que pasan y a reportar
cifras favorables.** Tres contramedidas explícitas:

**(a) Blindar la medición contra el optimizador.** No fue una exigencia teórica: salió de encontrar
el fallo en el banco del repositorio de referencia (`paso5_benchmark_comparativo`), que compila con
`-O2` y nunca consume el checksum del bucle de `mmap`, así que GCC lo elimina por código muerto:
reporta **3.134.335 MB/s y 0 fallos de página** para 64 MB —ambas físicamente imposibles— y concluye
que `mmap` es 380 veces más rápido; con `-O0`, 11.552 MB/s y ventaja real ~1,5x. El código está bien
escrito: el fallo está en la **metodología de medición**, que es el patrón que un modelo reproduce con
más facilidad porque es el que abunda.

**(b) Rechazar la cifra única.** La primera medición de latencia de `mmap()` dio 43,2 µs frente a
3,2 µs, un factor de 13 que invitaba a un titular. Repetida once veces y tomada la mediana, la
diferencia quedó bajo el ruido del planificador, y se reportó el resultado negativo.

**(c) No esconder el sobrecosto, atribuirlo.** El resultado honesto es **17,60x** en acceso
secuencial. La aportación no fue maquillarlo sino explicarlo —el costo dominante no es AES sino la
ausencia de *fault-around*, y la sección 4.2 lo descompone—, y de ahí salió una propuesta de mejora
cuantificada que una cifra maquillada no habría permitido formular. De las seis PoC diseñadas se
ejecutaron las seis que aplican (A0–A5, 8/8 PASS); A6 exige AEAD y se declara no aplicable en vez de
presentarse como verificada.

## 7. Lo que este flujo no cubre

**Detecta afirmaciones falsas, no afirmaciones no verificadas que resultan ciertas.** La alucinación
A5 de 3.2 acertó, y precisamente por eso no la detectó nada: una afirmación causal confiada y
correcta es, para quien lee, indistinguible de una verificada. El flujo reduce la probabilidad de
error, no la de exceso de confianza. **Verifica el presente, no el futuro:** `no_llseek` también
tenía su archivo y su línea en 6.11, así que toda la verificación de este informe caduca con la
versión. Y **depende de que exista un oráculo**: donde no lo hay —decisiones de diseño, juicios de
suficiencia, compensaciones— el flujo no aporta nada y la responsabilidad vuelve entera al humano. La
elección del mapeo de solo lectura es de ese tipo: el modelo expuso la consecuencia criptográfica de
reutilizar el nonce en CTR, pero aceptar perder la escritura a cambio de un cifrado correcto fue una
decisión de alcance, no un resultado verificable.
