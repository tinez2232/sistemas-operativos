# mmapguard

**Seguridad de archivos mediante intervención en la capa del kernel y mapeo de memoria (`mmap`)**

Módulo de kernel para Linux que expone archivos cifrados a través de un mapeo de memoria y los
descifra **una página de 4 KB a la vez, en el manejador de fallos de página**, de modo que el texto
plano nunca existe completo en memoria y ningún proceso de usuario tiene acceso a la clave.

> Parcial 25% — Sistemas Operativos, EAFIT 2026-2. Modalidad individual.
> El repositorio del curso (`SO2026B/USOS/CapaKernel`) es material de referencia, no la entrega.

---

## Qué garantiza, con precisión

| Propiedad | Mecanismo |
| :--- | :--- |
| El archivo es ilegible en reposo | AES-128-CTR, nonce derivado del índice de página |
| Solo un UID autorizado puede asociar y mapear | `mg_caller_is_authorized()` en el `ioctl` y en `mmap` |
| El mapeo es de **solo lectura**, sin excepción | `mg_mmap()` rechaza `PROT_WRITE` y `MAP_SHARED` |
| `mprotect(PROT_WRITE)` posterior falla | `vm_flags_clear(vma, VM_MAYWRITE)`: lo rechaza **el núcleo** |
| La exposición en RAM se limita a lo que se toca | `vm_ops->fault`, descifrado bajo demanda |
| La clave no es accesible desde el espacio de usuario | `module_param(key_hex, charp, 0)`: sin entrada en sysfs |
| Todo intento queda registrado | `struct mg_stats` vía `ioctl` + `pr_warn` a `dmesg` |

**Lo que NO hace, dicho sin rodeos:**

- **No admite escritura.** No es una limitación de tiempo sino de diseño: el modo CTR reutilizaría
  clave y nonce al recifrar una página modificada, revelando el XOR de los textos planos. Admitir
  escritura exige un modo AEAD con contador de versión. Está especificado como trabajo futuro.
- **Solo protege lo que pasa por `/dev/mmapguard`**, no archivos arbitrarios del sistema de archivos.
  Extenderlo a cobertura global es el plano 2 de la arquitectura (eBPF LSM), que **no está
  implementado** por un requisito de arranque que este entorno no satisface.
- **Cuesta 17,60x** en acceso secuencial completo, y 1,94x en acceso disperso. La causa está medida y
  atribuida más abajo; no es el cifrado.

---

## Requisitos

Este módulo se compiló y probó contra **un único entorno**, y los dos primeros requisitos no son
negociables:

| Requisito | Valor | Por qué |
| :--- | :--- | :--- |
| Kernel | `6.18.33.2-microsoft-standard-WSL2` | `CONFIG_MODVERSIONS=y`: el *vermagic* y los CRC deben coincidir |
| Compilador | **`gcc-13`** | El kernel lo compiló Microsoft con GCC 13.2; con GCC 15 los CRC no coinciden (ver abajo) |
| Árbol de fuentes | compilado por completo, con `Module.symvers` | `modules_prepare` no basta si hay `MODVERSIONS` |
| Distribución de prueba | Ubuntu 26.04 LTS sobre WSL2 | — |

```bash
sudo apt install -y build-essential bison flex libssl-dev libelf-dev cpio \
                    dwarves gcc-13 gcc-13-plugin-dev
```

> **Si `insmod` falla con `Invalid module format`**, consulte `dmesg`. Si dice
> *"disagrees about version of symbol module_layout"*, se compiló con el compilador equivocado:
> use `CC=gcc-13`. Si el *vermagic* lleva un sufijo `+` que el kernel en ejecución no tiene,
> el árbol de fuentes tiene un directorio `.git` y `scripts/setlocalversion` lo está añadiendo;
> hay que ocultarlo. El truco de `touch .scmversion` **ya no funciona** en 6.18.

---

## Compilación

```bash
# 1. El módulo de kernel
make modules KDIR=/ruta/al/arbol/del/kernel CC=gcc-13

# 2. Las herramientas de espacio de usuario
make tools

# 3. Los datos de demostración (se generan, no vienen en el repositorio)
make demo
```

`make help` lista todos los objetivos. El módulo compila **sin una sola advertencia** con
`-Wall -Wextra`, que es un requisito del enunciado.

---

## Uso

Las operaciones privilegiadas van marcadas con `sudo`.

```bash
# Cargar, autorizando al UID 1000
sudo insmod mmapguard.ko authorized_uid=1000
sudo chmod 666 /dev/mmapguard

# Ejecutar las pruebas de concepto
cd demo && ../tools/mg_test secreto.enc

# Medir el sobrecosto (stride: 1 = secuencial, 8 = salto medio, 64 = disperso)
../tools/mg_bench grande.txt grande.enc 1

# Descargar
sudo rmmod mmapguard
```

**Parámetros del módulo**

| Parámetro | Valor por omisión | Significado |
| :--- | :--- | :--- |
| `authorized_uid` | `-1` (solo root) | UID efectivo autorizado a asociar y mapear |
| `key_hex` | clave de demostración | Clave AES-128 en hexadecimal, 32 caracteres |

Para cifrar un archivo propio con el esquema que el driver espera:

```bash
tools/mg_encrypt claro.txt cifrado.enc 00112233445566778899aabbccddeeff
sudo rmmod mmapguard && sudo insmod mmapguard.ko \
     authorized_uid=1000 key_hex=00112233445566778899aabbccddeeff
```

> ⚠️ La clave `000102030405060708090a0b0c0d0e0f` que trae el código es una **clave de demostración**
> para que las pruebas corran sin configuración. No tiene otro propósito. Nótese además que
> `key_hex` se declara con permiso `0`, de modo que no queda legible en
> `/sys/module/mmapguard/parameters/`, pero sí es visible en la línea de `insmod` y por tanto en el
> historial del intérprete de comandos: en un uso real la clave debe entrar por otra vía.

---

## Pruebas de concepto

`mg_test` ejecuta tres ataques y comprueba que el mecanismo los ataja. Salida esperada:

```
=== mmapguard: prueba del plano de datos ===

[A1] Lectura DIRECTA del archivo (sin el driver):
     <bytes ilegibles>
     -> ilegible: el archivo esta cifrado en reposo.

[+] Archivo asociado al driver.

[  ] Lectura a traves del MAPEO PROTEGIDO:
     NOMINA CONFIDENCIAL - EAFIT 2026-2 - NO DIVULGAR
     -> legible: descifrado pagina a pagina en el fallo.

[A3] Intento de escalada con mprotect(PROT_WRITE):
     -> DENEGADO (Operation not permitted): VM_MAYWRITE fue retirado en el mmap.

[A2] Intento de mmap con PROT_WRITE:
     -> DENEGADO (Permission denied): politica de solo lectura.

--- Auditoria del driver ---
  Tamano del archivo    : 4764 bytes
  Paginas descifradas   : 1
  Bytes descifrados     : 4096
  Mapeos denegados      : 1
  Escrituras denegadas  : 0
```

Las dos últimas cifras son el resultado más informativo del conjunto:

- **`Mapeos denegados: 1`, no 2.** Hubo dos ataques de escritura y el módulo solo contó uno: el
  `mprotect` **nunca llegó al módulo**. Lo rechazó el propio núcleo al comprobar `VM_MAYWRITE`. Eso
  es precisamente el efecto buscado al retirar ese bit en vez de interceptar la llamada: la política
  es un invariante del VMA, no una lista de comprobaciones que haya que acertar a enumerar.
- **`Escrituras denegadas: 0`.** No es un mecanismo que falló, es la medida de que la ruta de
  escritura es **inalcanzable**: `page_mkwrite` está registrado como defensa en profundidad y nunca
  tuvo que actuar.

Mapeo de cada PoC con las preguntas de la Parte I: **A1 → 5, A2 → 2, A3 → 3.** Las PoC A4
(pregunta 8) y A5 (pregunta 9) están diseñadas en la documentación pero **no ejecutadas**.

---

## Sobrecosto medido

Archivo de 16.777.285 bytes (4.097 páginas), mediana de 11 repeticiones.

| Patrón de acceso | Páginas tocadas | Fallos: `mmap` / `mmapguard` | Sobrecosto | Archivo descifrado |
| :--- | ---: | ---: | ---: | ---: |
| Secuencial | 4.097 | 257 / 4.097 | **17,60x** | 100 % |
| Salto de 8 | 513 | 257 / 513 | 2,93x | 12,5 % |
| Disperso (64) | 65 | 65 / 65 | **1,94x** | **1,6 %** |

**El costo dominante no es AES, es la ausencia de *fault-around*.** `mmap()` ordinario puebla 16 PTE
por fallo (`fault_around_pages = 65536 >> PAGE_SHIFT`); nosotros una. La razón 15,9x explica casi
todo el 17,60x, y la prueba es la última fila: cuando ambos esquemas hacen los mismos 65 fallos, el
sobrecosto cae a 1,94x. El mecanismo cobra por página mediada, no como impuesto fijo.

Implementar `.map_pages` —descifrar un lote de páginas por fallo— bajaría el secuencial a ~9x sin
perder ni la decisión en Ring 0 ni la granularidad de página. Está identificado y no implementado.

---

## Arquitectura

Un plano por cada objetivo del enunciado:

| Objetivo | Plano | Mecanismo | Estado |
| :--- | :--- | :--- | :--- |
| Aislamiento de memoria | Datos | `vm_ops->fault` con descifrado por página | **Implementado** |
| Prevención de modificaciones | Política | `mg_mmap()` + `vm_flags_clear(VM_MAYWRITE)` | **Implementado** (alcance: `/dev/mmapguard`) |
| | | eBPF LSM sobre `mmap_file` y `file_mprotect` | No implementado (alcance global) |
| Auditoría de accesos | Observabilidad | `struct mg_stats` vía `ioctl` + `getrusage` | **Implementado** |

**La decisión técnica central** es usar `vm_operations_struct->fault` en lugar de
`remap_pfn_range()`, que es lo que emplea el material de referencia del curso. `remap_pfn_range()`
programa todas las PTE de golpe, lo que obliga a mantener **el archivo completo descifrado en RAM**
durante toda la vida del mapeo. Con el manejador de fallos se descifra una página cuando se toca, y
solo esa: un cliente que lee 64 bytes de un archivo de 2 páginas deja 4.096 bytes en claro, no 8.192.
Eso convierte la paginación por demanda, que en el material de referencia es una optimización, en un
**control de minimización de exposición**.

**Alternativas descartadas**, cada una por una razón verificada contra el árbol de fuentes: hook en
la tabla de syscalls (exige desarmar el bit WP de `CR0`), LSM propio como módulo cargable
(`security_add_hooks()` no está exportada), y eBPF solo con `fentry`/`kprobes` (observa pero no puede
denegar).

**Por qué en el kernel y no en espacio de usuario.** Porque el evento que hay que mediar —el acceso a
una página— **no es una llamada al sistema**, y todo lo que el espacio de usuario puede interceptar
es una frontera de syscall. `strace` sobre el cliente registra 30 llamadas a `mmap` y ninguna señal
de las 4.097 páginas descifradas. El argumento completo, incluido el examen de `seccomp` y de FUSE,
está en [`docs/04-justificacion-kernel-vs-usuario.md`](docs/04-justificacion-kernel-vs-usuario.md).

---

## Estructura

```
mmapguard/
├── Makefile                  # modulo + objetivos tools, demo, clean, help
├── src/
│   ├── mmapguard.c           # el modulo
│   └── mmapguard_uapi.h      # contrato ioctl compartido con el usuario
├── tools/
│   ├── mg_encrypt.c          # cifra un archivo con el esquema del driver (OpenSSL)
│   ├── mg_test.c             # las tres pruebas de concepto
│   └── mg_bench.c            # comparativa a 3 bandas con getrusage
├── demo/
│   └── gen_demo.sh           # genera los datos de prueba, deterministas
└── docs/
    ├── 02-preguntas.md                        # Parte I: 10 preguntas conceptuales
    ├── 03-1-flujo-trabajo-ia.md               # 3.1 flujo de trabajo asistido por IA
    ├── 03-2-prompts-validacion.md             # 3.2 prompts y alucinaciones detectadas
    ├── 03-3-arquitectura.md                   # 3.3 estrategia arquitectonica
    ├── 04-resultados.md                       # 4.2 medicion de impacto
    └── 04-justificacion-kernel-vs-usuario.md  # 4.2 justificacion teorica
```

Los datos de `demo/` y los artefactos de compilación no se versionan: se regeneran con `make demo`,
que produce archivos **byte a byte idénticos** a los usados en las mediciones de este informe.

---

## Notas de reproducibilidad

Las cifras de este README salieron de un único entorno y dependen de él. Dos advertencias
metodológicas que vale la pena heredar:

1. **Compile las mediciones de forma que el optimizador no las borre.** El banco de pruebas del
   repositorio de referencia (`paso5_benchmark_comparativo`) nunca consume el checksum de su bucle,
   así que GCC `-O2` lo elimina por código muerto y reporta 3.134.335 MB/s y 0 fallos de página para
   64 MB: ambas cifras imposibles. Recompilado con `-O0` da 11.552 MB/s y 1.024 fallos. `mg_bench`
   imprime todos sus checksums precisamente por eso.
2. **No confíe en una sola muestra.** La primera medición de latencia de `mmap()` dio 43,2 µs frente
   a 3,2 µs; con 11 repeticiones y mediana, la diferencia quedó por debajo del ruido del
   planificador.

Y una tercera, sobre el instrumento: **`strace` es inútil para observar este módulo**, porque un
fallo de página no es una syscall. Hay que usar `getrusage`, `ftrace` o `perf`.
