#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
build_report.py - Ensambla el informe del parcial en un unico PDF.

Parcial 25% - Seguridad de Archivos en Capa Kernel y mmap.

Toma los documentos de docs/, el README del repositorio y el registro de la
ultima ejecucion del banco de pruebas, y produce informe.pdf: portada, indice
con numeros de pagina, encabezados por seccion y resaltado de sintaxis.

El diagrama de arquitectura esta escrito en mermaid dentro del markdown, para
que GitHub lo renderice. Aqui ese bloque se sustituye por docs/arquitectura.svg,
que es el mismo diagrama dibujado a mano para impresion.

Dependencias (no requieren privilegios):

    python3 -m venv ~/.venv-informe
    ~/.venv-informe/bin/pip install weasyprint markdown pygments
    ~/.venv-informe/bin/python docs/build_report.py

Uso:  build_report.py [-o salida.pdf]
"""

import argparse
import datetime
import html
import os
import re
import sys

try:
    import markdown
    from weasyprint import HTML
    from pygments.formatters import HtmlFormatter
except ImportError as e:
    sys.exit(f"Falta una dependencia: {e.name}\n"
             "Instale con:  pip install weasyprint markdown pygments\n"
             "(vea la cabecera de este archivo)")

AQUI = os.path.dirname(os.path.abspath(__file__))
RAIZ = os.path.dirname(AQUI)

TITULO = "Seguridad de Archivos mediante Intervención en la Capa del Kernel y mmap"
SUBTITULO = "mmapguard — mapeo protegido con descifrado bajo demanda"
CURSO = "Sistemas Operativos · EAFIT · 2026-2"
# Nombre del estudiante. Se puede pasar por la linea de ordenes: --autor "..."
AUTOR = "«indique el autor con --autor»"
EVALUACION = "Evaluación parcial práctica-teórica · 25%"

# (archivo, titulo en el indice, rotulo de la parte)
PARTES = [
    ("docs/02-preguntas.md",
     "Análisis conceptual: diez preguntas", "PARTE I · 30%"),
    ("docs/03-1-flujo-trabajo-ia.md",
     "Flujo de trabajo asistido por IA", "PARTE II · 30%"),
    ("docs/03-2-prompts-validacion.md",
     "Ingeniería de prompts y verificación crítica", "PARTE II · 30%"),
    ("docs/03-3-arquitectura.md",
     "Estrategia arquitectónica seleccionada", "PARTE II · 30%"),
    ("docs/04-1-implementacion.md",
     "Implementación: código, compilación y PoC", "PARTE III · 40%"),
    ("docs/04-resultados.md",
     "Medición de impacto y análisis de resultados", "PARTE III · 40%"),
    ("docs/04-justificacion-kernel-vs-usuario.md",
     "Justificación: kernel frente a espacio de usuario", "PARTE III · 40%"),
    ("README.md",
     "README del repositorio", "ANEXO A"),
]

LOG_BANCO = "tests/last-run.log"
SVG_ARQUITECTURA = "docs/arquitectura.svg"

EXTENSIONES = ["tables", "fenced_code", "codehilite", "attr_list", "sane_lists",
               "toc", "footnotes", "def_list"]
CONFIG_EXT = {"codehilite": {"guess_lang": False, "linenums": False}}

CSS = """
@page {
    size: A4;
    margin: 20mm 18mm 18mm 18mm;
    @top-left   { content: string(parte);   font-size: 7.5pt; color: #777; }
    @top-right  { content: string(seccion); font-size: 7.5pt; color: #777; }
    @bottom-center {
        content: counter(page) " / " counter(pages);
        font-size: 8pt; color: #777;
    }
}
@page :first { margin: 0; @top-left { content: "" } @top-right { content: "" }
               @bottom-center { content: "" } }

html { font-size: 10pt; }
body { font-family: "DejaVu Serif", serif; line-height: 1.45; color: #16160f;
       text-align: justify; hyphens: auto; }

/* ---------------- portada ---------------- */
.portada { page: first; height: 297mm; padding: 45mm 25mm 20mm 25mm;
           text-align: left; page-break-after: always; }
.portada .curso { font-family: "DejaVu Sans", sans-serif; font-size: 9.5pt;
                  letter-spacing: 1.2px; text-transform: uppercase; color: #6a6a60; }
.portada h1 { font-family: "DejaVu Sans", sans-serif; font-size: 23pt;
              line-height: 1.22; margin: 14mm 0 6mm 0; color: #16160f;
              border: none; padding: 0; }
.portada .sub { font-size: 12pt; color: #44443c; margin-bottom: 16mm; }
.portada .rule { border-top: 2.5pt solid #8a1c1c; width: 38mm; margin-bottom: 8mm; }
.portada .meta { font-family: "DejaVu Sans", sans-serif; font-size: 9.5pt;
                 color: #44443c; line-height: 1.9; }
.portada .meta b { color: #16160f; }
.portada .pie { position: absolute; bottom: 20mm; font-family: "DejaVu Sans", sans-serif;
                font-size: 8.5pt; color: #6a6a60; }

/* ---------------- indice ---------------- */
.indice { page-break-after: always; }
.indice h2 { font-size: 15pt; margin-bottom: 8mm; }
.indice ol { list-style: none; padding: 0; margin: 0; }
.indice li { margin-bottom: 2.6mm; font-family: "DejaVu Sans", sans-serif;
             font-size: 10pt; }
.indice .parte { font-size: 7.5pt; letter-spacing: 1px; color: #8a1c1c;
                 text-transform: uppercase; display: block; margin-top: 5mm; }
.indice a { text-decoration: none; color: #16160f; }
.indice a::after { content: " · " target-counter(attr(href), page); color: #6a6a60; }

/* ---------------- cuerpo ---------------- */
.doc { page-break-before: always; }
.doc h1 { string-set: seccion content(); }
.doc { string-set: parte attr(data-parte); }

h1, h2, h3, h4 { font-family: "DejaVu Sans", sans-serif; text-align: left;
                 hyphens: none; page-break-after: avoid; color: #16160f; }
h1 { font-size: 17pt; line-height: 1.25; margin: 0 0 6mm 0;
     padding-bottom: 2.5mm; border-bottom: 1.5pt solid #8a1c1c; }
h2 { font-size: 12.5pt; margin: 7mm 0 3mm 0; }
h3 { font-size: 10.5pt; margin: 5mm 0 2mm 0; }
h4 { font-size: 9.5pt; margin: 4mm 0 2mm 0; color: #44443c; }
p  { margin: 0 0 2.6mm 0; }
strong { color: #000; }

ul, ol { margin: 0 0 3mm 0; padding-left: 5mm; }
li { margin-bottom: 1.2mm; }

blockquote { margin: 3mm 0; padding: 2mm 0 2mm 4mm; border-left: 2pt solid #c9c9c0;
             color: #44443c; font-size: 9.2pt; }
blockquote p { margin-bottom: 1.5mm; }

hr { border: none; border-top: 0.5pt solid #d6d6ce; margin: 6mm 0; }

/* ---------------- codigo ---------------- */
/* hyphens:none es obligatorio: con guionado automatico el texto justificado
   parte los identificadores (vma_set_pa-ge_prot), y ese guion puede leerse
   como parte del nombre. */
code { font-family: "DejaVu Sans Mono", monospace; font-size: 8.3pt;
       background: #f2f2ee; padding: 0.3mm 0.8mm; border-radius: 1.5pt;
       hyphens: none; word-wrap: break-word; overflow-wrap: break-word; }
pre { font-family: "DejaVu Sans Mono", monospace; font-size: 7.8pt; line-height: 1.35;
      background: #f7f7f3; border: 0.5pt solid #ddddd4; border-left: 2pt solid #8a8a7e;
      padding: 2.5mm 3mm; margin: 2.5mm 0; white-space: pre-wrap; word-wrap: break-word;
      page-break-inside: avoid; }
pre code { background: none; padding: 0; font-size: 7.8pt; }
.codehilite { margin: 2.5mm 0; }

/* ---------------- tablas ---------------- */
table { width: 100%; border-collapse: collapse; margin: 3mm 0; font-size: 8.2pt;
        font-family: "DejaVu Sans", sans-serif; page-break-inside: avoid;
        text-align: left; hyphens: none; }
th { background: #efefe9; border-bottom: 1pt solid #9a9a90; padding: 1.4mm 1.8mm;
     text-align: left; font-size: 8pt; }
td { border-bottom: 0.4pt solid #e2e2da; padding: 1.4mm 1.8mm; vertical-align: top; }
td code, th code { font-size: 7.6pt; }
tr { page-break-inside: avoid; }

/* ---------------- figuras ---------------- */
figure { margin: 4mm 0; text-align: center; page-break-inside: avoid; }
figure img, figure svg { width: 100%; max-width: 165mm; }
figcaption { font-family: "DejaVu Sans", sans-serif; font-size: 8pt; color: #6a6a60;
             margin-top: 2mm; text-align: center; }

/* ---------------- anexo del registro ---------------- */
.registro pre { font-size: 6.9pt; line-height: 1.3; white-space: pre-wrap; }
"""

# Hoja adicional para --breve. No cambia la estructura: aprieta la tipografia
# y los margenes para que la version condensada entre en 20 paginas.
CSS_BREVE = """
@page { margin: 13mm 13mm 11mm 13mm; }
html { font-size: 8.8pt; }
body { line-height: 1.29; }
.portada { padding: 42mm 25mm 20mm 25mm; }
.portada h1 { font-size: 21pt; }
.indice li { margin-bottom: 1.9mm; font-size: 9.2pt; }
.indice .parte { margin-top: 3.5mm; }
h1 { font-size: 13.6pt; margin-bottom: 3.4mm; padding-bottom: 1.8mm; }
h2 { font-size: 10.6pt; margin: 3.8mm 0 1.6mm 0; }
h3 { font-size: 9.3pt; margin: 3mm 0 1.2mm 0; }
p  { margin: 0 0 1.8mm 0; }
ul, ol { margin: 0 0 2mm 0; }
li { margin-bottom: 0.7mm; }
blockquote { margin: 2mm 0; padding: 1.2mm 0 1.2mm 3mm; font-size: 8.5pt; }
table { font-size: 7.3pt; margin: 1.8mm 0; }
th { padding: 1mm 1.4mm; font-size: 7.5pt; }
td { padding: 1mm 1.4mm; }
td code, th code { font-size: 7.1pt; }
code { font-size: 7.9pt; }
pre, pre code { font-size: 7.0pt; line-height: 1.25; }
pre { padding: 1.8mm 2.2mm; margin: 2mm 0; }
figure { margin: 3mm 0; }
/* Ninguna seccion fuerza pagina nueva: con siete saltos se desperdiciaban
   cuatro paginas en blanco al pie de los documentos. El h1 con su filete y el
   encabezado de pagina siguen marcando donde empieza cada seccion. */
.portada { padding: 24mm 22mm 18mm 22mm; }
.indice-portada { page-break-after: always; margin-top: 12mm; }
.indice-portada h2 { font-size: 11.5pt; margin: 0 0 4mm 0;
    font-family: "DejaVu Sans", sans-serif; letter-spacing: .5px;
    text-transform: uppercase; color: #6a6a60; }
.indice-portada li { margin-bottom: 1.5mm; font-size: 8.8pt; }
.indice-portada .parte { margin-top: 3mm; }
.doc { page-break-before: auto; }
.doc:first-of-type { page-break-before: always; }
.doc + .doc h1 { margin-top: 4.5mm; page-break-after: avoid; }
/* Una tabla o un bloque de codigo que no entra en lo que queda de pagina se
   empuja entero y deja un hueco; aqui se permite partirlos, repitiendo la
   cabecera de la tabla, y se mantiene la fila como unidad indivisible. */
table, pre, .codehilite { page-break-inside: auto; }
thead { display: table-header-group; }
tr { page-break-inside: avoid; }
"""


def leer(ruta):
    with open(os.path.join(RAIZ, ruta), encoding="utf-8") as f:
        return f.read()


def sustituir_mermaid(texto):
    """Cambia el bloque mermaid por el SVG de impresion."""
    svg = os.path.join(RAIZ, SVG_ARQUITECTURA)
    if not os.path.exists(svg):
        return texto
    cuerpo = open(svg, encoding="utf-8").read()
    cuerpo = re.sub(r"<\?xml[^>]*\?>", "", cuerpo).strip()
    figura = ('<figure>' + cuerpo +
              '<figcaption>Arquitectura de <code>mmapguard</code>: los tres planos, '
              'la frontera de privilegio y los tres ataques del banco de pruebas.'
              '</figcaption></figure>')
    return re.sub(r"```mermaid\n.*?\n```", lambda _: figura, texto,
                  count=1, flags=re.DOTALL)


def quitar_ansi(texto):
    return re.sub(r"\x1b\[[0-9;]*m", "", texto)


def desplazar_titulos(html_doc):
    """El h1 de cada documento ya lo pone la plantilla; aqui no se tocan."""
    return html_doc


def construir(autor=AUTOR, breve=False):
    md = markdown.Markdown(extensions=EXTENSIONES, extension_configs=CONFIG_EXT)
    partes_html, indice = [], []
    parte_previa = None

    for n, (ruta, titulo, parte) in enumerate(PARTES, 1):
        if breve:
            # En modo breve los anexos no se incluyen y cada documento se busca
            # primero en docs/breve/, que es la version condensada.
            if parte.startswith("ANEXO"):
                continue
            alterna = ruta.replace("docs/", "docs/breve/", 1)
            if ruta.startswith("docs/") and os.path.exists(
                    os.path.join(RAIZ, alterna)):
                ruta = alterna
        destino = os.path.join(RAIZ, ruta)
        if not os.path.exists(destino):
            print(f"  aviso: falta {ruta}, se omite", file=sys.stderr)
            continue
        texto = leer(ruta)
        if ruta.endswith("03-3-arquitectura.md"):
            texto = sustituir_mermaid(texto)
        md.reset()
        cuerpo = md.convert(texto)
        ident = f"sec{n}"
        # En modo breve solo la primera seccion de cada parte empieza en pagina
        # nueva: con siete saltos forzados se desperdician varias paginas en
        # blanco al pie de cada documento.
        clases = "doc"
        if parte != parte_previa:
            clases += " inicio-parte"
            parte_previa = parte
        partes_html.append(
            f'<section class="{clases}" id="{ident}" data-parte="{html.escape(parte)}">'
            f'{cuerpo}</section>')
        indice.append((ident, titulo, parte))
        print(f"  + {ruta}")

    # Anexo B: el registro del banco de pruebas
    log = os.path.join(RAIZ, LOG_BANCO)
    if breve:
        pass
    elif os.path.exists(log):
        contenido = html.escape(quitar_ansi(open(log, encoding="utf-8").read()))
        ident = "anexoB"
        partes_html.append(
            f'<section class="doc registro" id="{ident}" data-parte="ANEXO B">'
            f'<h1>Anexo B. Registro del banco de pruebas</h1>'
            f'<p>Salida íntegra de <code>./tests/run_pocs.sh --load</code>, '
            f'incluido el registro del núcleo que el módulo emitió durante la '
            f'ejecución.</p><pre>{contenido}</pre></section>')
        indice.append((ident, "Registro del banco de pruebas", "ANEXO B"))
        print(f"  + {LOG_BANCO}")
    elif not breve:
        print(f"  aviso: falta {LOG_BANCO}; el anexo B no se incluye",
              file=sys.stderr)

    # Indice
    filas, parte_actual = [], None
    for ident, titulo, parte in indice:
        if parte != parte_actual:
            filas.append(f'<li><span class="parte">{html.escape(parte)}</span></li>')
            parte_actual = parte
        filas.append(f'<li><a href="#{ident}">{html.escape(titulo)}</a></li>')

    hoy = datetime.date.today().strftime("%d/%m/%Y")
    pyg = HtmlFormatter(style="friendly").get_style_defs(".codehilite")
    indice_html = ('<div class="indice"><h2>Contenido</h2><ol>%s</ol></div>'
                   % "".join(filas))
    if breve:
        # Cabe al pie de la portada: una pagina propia para ocho entradas es
        # una pagina de 20 gastada en blanco.
        indice_html = ""

    return f"""<!doctype html>
<html lang="es"><head><meta charset="utf-8"><title>{html.escape(TITULO)}</title>
<style>{CSS}
{CSS_BREVE if breve else ""}
{pyg}
</style></head><body>
<div class="portada">
  <div class="curso">{html.escape(CURSO)}<br>{html.escape(EVALUACION)}</div>
  <h1>{html.escape(TITULO)}</h1>
  <div class="rule"></div>
  <div class="sub">{html.escape(SUBTITULO)}</div>
  <div class="meta">
    <b>Autor</b> · {html.escape(autor)}<br>
    <b>Modalidad</b> · individual<br>
    <b>Entregable</b> · módulo de kernel, herramientas, banco de pruebas e informe<br>
    <b>Kernel de desarrollo</b> · 6.18.33.2-microsoft-standard-WSL2, x86_64<br>
    <b>Banco de pruebas</b> · 8 de 8 PoC superadas
  </div>
  {('<div class="indice indice-portada"><h2>Contenido</h2><ol>'
     + "".join(filas) + "</ol></div>") if breve else ""}
  <div class="pie">Generado el {hoy} · las citas <i>archivo:línea</i> remiten al árbol
  del kernel 6.18.33.2</div>
</div>

{indice_html}

{''.join(partes_html)}
</body></html>"""


def main():
    ap = argparse.ArgumentParser(description="Ensambla el informe en PDF.")
    ap.add_argument("-o", "--salida", default=os.path.join(RAIZ, "informe.pdf"))
    ap.add_argument("--autor", default=AUTOR, help="nombre del estudiante")
    ap.add_argument("--html", action="store_true",
                    help="escribe tambien el HTML intermedio, para depurar")
    ap.add_argument("--breve", action="store_true",
                    help="version condensada: docs/breve/, sin anexos, "
                         "tipografia densa")
    args = ap.parse_args()

    print("Ensamblando el informe%s:" % (" (breve)" if args.breve else ""))
    doc = construir(args.autor, breve=args.breve)
    if args.html:
        ruta_html = os.path.splitext(args.salida)[0] + ".html"
        with open(ruta_html, "w", encoding="utf-8") as f:
            f.write(doc)
        print(f"  HTML intermedio: {ruta_html}")

    HTML(string=doc, base_url=RAIZ).write_pdf(args.salida)
    tam = os.path.getsize(args.salida) / 1024
    print(f"\n[+] {args.salida} ({tam:.0f} KB)")


if __name__ == "__main__":
    main()
