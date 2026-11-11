# sistemas-operativos

Trabajos de Sistemas Operativos, EAFIT 2026-2. **Esta rama, `parcial-3`, contiene el Parcial 25%.**

## Parcial 25% — Seguridad de archivos en capa kernel y `mmap`

El entregable está en **[`OS 2026-2/mmapguard/`](OS%202026-2/mmapguard/)**: un módulo de kernel que
expone archivos cifrados con AES-128-CTR a través de un mapeo de memoria de **solo lectura** y los
descifra **una página de 4 KB a la vez en el manejador de fallos de página** (`vm_ops->fault`), de
modo que el texto plano nunca existe completo en RAM y la clave no sale del kernel.

| Qué | Dónde |
| :--- | :--- |
| **Informe (20 págs., el que se evalúa)** | [`reporte parcial 3 resumen.pdf`](OS%202026-2/mmapguard/reporte%20parcial%203%20resumen.pdf) |
| Informe íntegro (63 págs., con anexos) | [`reporte parcial 3 completo.pdf`](OS%202026-2/mmapguard/reporte%20parcial%203%20completo.pdf) |
| Código, Makefile, uso y limitaciones | [`mmapguard/README.md`](OS%202026-2/mmapguard/README.md) |
| Módulo de kernel | `mmapguard/src/mmapguard.c` |
| Herramientas de usuario | `mmapguard/tools/` |
| Banco de PoC y su registro (8/8 PASS) | `mmapguard/tests/`, `tests/last-run.log` |
| Documentos fuente del informe | `mmapguard/docs/` |

> **Requiere el kernel `6.18.33.2-microsoft-standard-WSL2` y `gcc-13`.** Con
> `CONFIG_MODVERSIONS=y`, si el *vermagic* o un solo CRC no coinciden, `insmod` falla con
> `Invalid module format`. El README del módulo explica cómo recompilar contra otro kernel o cómo
> fijar WSL a esa versión.

Las ramas anteriores conservan los trabajos previos: `parcial-2` y `main`.
