#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
mg_poc.py - Banco de ataques contra mmapguard.ko desde espacio de usuario.

Parcial 25% - Seguridad de Archivos en Capa Kernel y mmap.

Por que en Python y no en C: las PoC de este archivo necesitan control crudo
sobre mmap(), mprotect() y madvise() -- banderas exactas y errno exacto -- y
ademas necesitan provocar un SIGSEGV deliberado en un proceso hijo aislado.
ctypes da acceso directo a libc sin compilar nada, de modo que el banco de
ataques se puede modificar y reejecutar sin pasar por el Makefile. El cliente
en C (tools/mg_test) queda como implementacion de referencia.

Cada subcomando es una PoC independiente y devuelve un codigo de salida:

    0  PASS          la defensa se comporto como se esperaba
    1  FAIL          fallo de seguridad: el ataque tuvo exito
    2  INCONCLUSIVE  la prueba no pudo concluir (no es un fallo de seguridad)
    3  ENV           problema de entorno (modulo no cargado, permisos, datos)

Uso:
    python3 mg_poc.py <poc> [argumentos]
    python3 mg_poc.py --list
"""

import ctypes
import ctypes.util
import fcntl
import hashlib
import os
import resource
import signal
import struct
import sys
import threading
import time

DEV = "/dev/mmapguard"
PAGE = os.sysconf("SC_PAGE_SIZE")

RC_PASS = 0
RC_FAIL = 1
RC_INCONC = 2
RC_ENV = 3

# --------------------------------------------------------------------------
# Salida
# --------------------------------------------------------------------------

_COLOR = ((sys.stdout.isatty() or os.environ.get("MG_COLOR") == "1")
          and os.environ.get("NO_COLOR") is None)


def _c(code, text):
    return f"\033[{code}m{text}\033[0m" if _COLOR else text


def head(text):
    print(_c("1;36", f"--- {text}"))


def info(text):
    print(f"    {text}")


def ok(text):
    print("    " + _c("0;32", f"-> PASS: {text}"))
    return RC_PASS


def bad(text):
    print("    " + _c("0;31", f"-> FAIL: {text}"))
    return RC_FAIL


def inconc(text):
    print("    " + _c("1;33", f"-> INCONCLUSIVE: {text}"))
    return RC_INCONC


class EnvError(Exception):
    pass


# --------------------------------------------------------------------------
# Interfaz del driver (replica de src/mmapguard_uapi.h)
# --------------------------------------------------------------------------

MG_PATH_MAX = 256
MG_IOC_MAGIC = ord("G")


class MgAttachReq(ctypes.Structure):
    _fields_ = [("path", ctypes.c_char * MG_PATH_MAX),
                ("flags", ctypes.c_uint64)]


class MgStats(ctypes.Structure):
    _fields_ = [("file_size", ctypes.c_uint64),
                ("faults", ctypes.c_uint64),
                ("bytes_decrypted", ctypes.c_uint64),
                ("denied_mmap", ctypes.c_uint64),
                ("denied_write", ctypes.c_uint64)]


# Codificacion de <asm-generic/ioctl.h>: dir(2) size(14) type(8) nr(8).
_IOC_NONE, _IOC_WRITE, _IOC_READ = 0, 1, 2


def _IOC(direction, typ, nr, size):
    return (direction << 30) | (size << 16) | (typ << 8) | nr


MG_IOC_ATTACH = _IOC(_IOC_WRITE, MG_IOC_MAGIC, 1, ctypes.sizeof(MgAttachReq))
MG_IOC_STATS = _IOC(_IOC_READ, MG_IOC_MAGIC, 2, ctypes.sizeof(MgStats))


def dev_open(mode=os.O_RDONLY):
    """Abre /dev/mmapguard, distinguiendo 'no cargado' de 'sin permisos'."""
    try:
        return os.open(DEV, mode)
    except FileNotFoundError:
        raise EnvError(f"{DEV} no existe: el modulo no esta cargado "
                       "(sudo insmod mmapguard.ko authorized_uid=$(id -u))")
    except PermissionError:
        raise EnvError(f"sin permisos sobre {DEV} (sudo chmod 666 {DEV})")
    except OSError as e:
        raise EnvError(f"no se pudo abrir {DEV}: {e.strerror}")


def attach(fd, path):
    """MG_IOC_ATTACH. Propaga OSError para que cada PoC lea el errno."""
    req = MgAttachReq(path=os.path.abspath(path).encode(), flags=0)
    fcntl.ioctl(fd, MG_IOC_ATTACH, bytes(req))


def stats(fd):
    buf = bytearray(ctypes.sizeof(MgStats))
    fcntl.ioctl(fd, MG_IOC_STATS, buf)
    return MgStats.from_buffer_copy(buf)


def print_stats(st, prefix="    "):
    print(f"{prefix}contadores: faults={st.faults} "
          f"bytes_descifrados={st.bytes_decrypted} "
          f"denied_mmap={st.denied_mmap} denied_write={st.denied_write}")


# --------------------------------------------------------------------------
# libc
# --------------------------------------------------------------------------

libc = ctypes.CDLL("libc.so.6", use_errno=True)
libc.mmap.restype = ctypes.c_void_p
libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                      ctypes.c_int, ctypes.c_int, ctypes.c_long]
libc.munmap.restype = ctypes.c_int
libc.munmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
libc.mprotect.restype = ctypes.c_int
libc.mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
libc.madvise.restype = ctypes.c_int
libc.madvise.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]

MAP_FAILED = ctypes.c_void_p(-1).value

PROT_NONE, PROT_READ, PROT_WRITE = 0x0, 0x1, 0x2
MAP_SHARED, MAP_PRIVATE = 0x01, 0x02
MADV_DONTNEED = 4


def do_mmap(fd, length, prot, flags, offset=0):
    """Devuelve (addr, 0) o (None, errno)."""
    ctypes.set_errno(0)
    addr = libc.mmap(None, length, prot, flags, fd, offset)
    if addr is None or addr == MAP_FAILED:
        return None, ctypes.get_errno()
    return addr, 0


def do_mprotect(addr, length, prot):
    """Devuelve 0 o el errno."""
    ctypes.set_errno(0)
    if libc.mprotect(ctypes.c_void_p(addr), length, prot) != 0:
        return ctypes.get_errno()
    return 0


def errname(err):
    return f"{os.strerror(err)} (errno {err}/{_ERRNAMES.get(err, '?')})"


_ERRNAMES = {1: "EPERM", 13: "EACCES", 22: "EINVAL", 16: "EBUSY", 2: "ENOENT"}


def printable_ratio(data):
    if not data:
        return 0.0
    n = sum(1 for b in data if 32 <= b < 127 or b in (9, 10, 13))
    return n / len(data)


def maps_flags(addr):
    """Los cuatro caracteres de permisos de /proc/self/maps para addr.

    CUIDADO: el cuarto caracter NO refleja VM_SHARED. Es
    `flags & VM_MAYSHARE ? 's' : 'p'` (`fs/proc/task_mmu.c:454`), y do_mmap()
    retira VM_SHARED dejando VM_MAYSHARE intacto, de modo que un mapeo ya
    degradado a privado sigue mostrandose como 's'. Para VM_SHARED hay que
    usar vma_flags().
    """
    try:
        with open("/proc/self/maps") as f:
            for line in f:
                parts = line.split()
                start, end = (int(x, 16) for x in parts[0].split("-"))
                if start <= addr < end:
                    return parts[1]
    except OSError:
        pass
    return None


def vma_flags(addr):
    """Mnemonicos de VmFlags en /proc/self/smaps para el VMA que contiene addr.

    smaps imprime una bandera por mnemonico (`fs/proc/task_mmu.c:1152-1159`),
    de modo que distingue lo que maps confunde:

        rd = VM_READ   wr = VM_WRITE   sh = VM_SHARED
        mw = VM_MAYWRITE               ms = VM_MAYSHARE

    Devuelve un conjunto, o None si no se pudo leer.
    """
    try:
        with open("/proc/self/smaps") as f:
            dentro = False
            for line in f:
                if line.startswith("VmFlags:"):
                    if dentro:
                        return set(line.split()[1:])
                    continue
                parts = line.split()
                if not parts or "-" not in parts[0]:
                    continue
                try:
                    start, end = (int(x, 16) for x in parts[0].split("-"))
                except ValueError:
                    continue
                dentro = start <= addr < end
    except OSError:
        pass
    return None


def need_file(path):
    if not os.path.isfile(path):
        raise EnvError(f"falta el archivo '{path}' "
                       "(genere los datos con: make demo)")
    return path


# --------------------------------------------------------------------------
# A0 - Acceso no autorizado por UID
# --------------------------------------------------------------------------

def poc_unauth(enc):
    """Un UID no autorizado no debe poder asociar un archivo protegido.

    Se ejecuta bajo otro UID (el orquestador usa setpriv). El driver compara
    current_euid() contra el parametro authorized_uid y devuelve -EPERM.
    """
    head(f"A0  Asociacion desde un UID no autorizado (uid={os.geteuid()})")
    if os.geteuid() == 0:
        return inconc("se esta ejecutando como root, que siempre esta "
                      "autorizado; relance bajo un UID sin privilegios")

    fd = dev_open()
    try:
        try:
            attach(fd, enc)
        except OSError as e:
            if e.errno == 1:  # EPERM
                info(f"MG_IOC_ATTACH rechazado: {errname(e.errno)}")
                st = stats(fd)
                print_stats(st)
                if st.denied_mmap == 0:
                    return inconc("denegado, pero el contador denied_mmap "
                                  "no se incremento")
                return ok("el driver rechaza el UID no autorizado y lo audita")
            return inconc(f"rechazado con un error inesperado: "
                          f"{errname(e.errno)}")
        return bad("un UID no autorizado logro asociar el archivo protegido")
    finally:
        os.close(fd)


# --------------------------------------------------------------------------
# A1 - El archivo es ilegible en reposo
# --------------------------------------------------------------------------

def poc_a1(enc, claro=None):
    """read() directo sobre el archivo cifrado no debe revelar nada."""
    head("A1  Lectura directa del archivo con read(), sin pasar por el driver")
    with open(enc, "rb") as f:
        raw = f.read(64)
    info("primeros 64 bytes: "
         + "".join(chr(b) if 32 <= b < 127 else "." for b in raw))
    ratio = printable_ratio(raw)
    info(f"fraccion imprimible: {ratio:.2f} (texto en claro ~1.00)")

    if claro:
        with open(claro, "rb") as f:
            plain = f.read(64)
        if raw == plain:
            return bad("el archivo en disco coincide con el texto en claro")
        info("difiere del texto en claro de referencia")
    if ratio > 0.90:
        return bad("el contenido en reposo parece texto legible")
    return ok("ilegible en reposo: el cifrado protege el archivo en disco")


# --------------------------------------------------------------------------
# Control positivo - el mapeo protegido si descifra, y descifra bien
# --------------------------------------------------------------------------

def poc_read(enc, claro=None):
    """Control positivo: PROT_READ + MAP_PRIVATE debe entregar texto claro.

    Sin esta prueba, un modulo que denegara absolutamente todo pasaria el
    resto del banco. Cuando hay un archivo en claro de referencia se compara
    el mapeo COMPLETO byte a byte, no solo el prefijo.
    """
    head("OK  Lectura a traves del mapeo protegido (PROT_READ, MAP_PRIVATE)")
    size = os.path.getsize(enc)
    fd = dev_open()
    try:
        try:
            attach(fd, enc)
        except OSError as e:
            if e.errno == 1:
                raise EnvError("MG_IOC_ATTACH devolvio EPERM: este UID no es "
                               "el authorized_uid con el que se cargo el modulo")
            raise EnvError(f"MG_IOC_ATTACH fallo: {errname(e.errno)}")

        addr, err = do_mmap(fd, size, PROT_READ, MAP_PRIVATE)
        if addr is None:
            return bad(f"el mapeo de solo lectura fue rechazado: {errname(err)}")
        try:
            got = ctypes.string_at(addr, min(size, 64))
            info("primeros 64 bytes: "
                 + "".join(chr(b) if 32 <= b < 127 else "." for b in got))
            if claro:
                whole = ctypes.string_at(addr, size)
                with open(claro, "rb") as f:
                    plain = f.read()
                if len(plain) != size:
                    return inconc(f"el claro mide {len(plain)} y el cifrado "
                                  f"{size}: no son el mismo par")
                if whole != plain:
                    first = next(i for i in range(size) if whole[i] != plain[i])
                    return bad(f"el descifrado no coincide con el claro "
                               f"(primer byte distinto en el offset {first})")
                info(f"los {size} bytes del mapeo coinciden con '{claro}' "
                     f"(sha256 {hashlib.sha256(whole).hexdigest()[:16]}...)")

            # Los contadores se leen DESPUES de recorrer el mapeo: leerlos
            # antes solo refleja las paginas tocadas hasta ese punto.
            st = stats(fd)
            print_stats(st)
            if claro:
                exp = (size + PAGE - 1) // PAGE
                if st.faults == exp:
                    info(f"se materializaron las {exp} paginas que cubre el "
                         f"archivo, una por fallo")
                else:
                    info(f"nota: se materializaron {st.faults} paginas de las "
                         f"{exp} que cubre el archivo")
            return ok("el mapeo entrega el texto en claro, descifrado por pagina")
        finally:
            libc.munmap(ctypes.c_void_p(addr), size)
    finally:
        os.close(fd)


# --------------------------------------------------------------------------
# A2 - mmap escribible y mmap compartido
# --------------------------------------------------------------------------

def poc_a2(enc, claro=None):
    """PROT_WRITE siempre se deniega. MAP_SHARED depende del modo del fd.

    La primera ejecucion del banco marco FAIL aqui, y la causa estaba en la
    prueba, no en el modulo: con un descriptor abierto en O_RDONLY el propio
    nucleo retira VM_SHARED y VM_MAYWRITE del conjunto de banderas ANTES de
    llamar a f_op->mmap (`mm/mmap.c:462-464`):

        vm_flags |= VM_SHARED | VM_MAYSHARE;
        if (!(file->f_mode & FMODE_WRITE))
                vm_flags &= ~(VM_MAYWRITE | VM_SHARED);

    De modo que mg_mmap() nunca llega a ver VM_SHARED y concede el mapeo. Lo
    que concede es un mapeo PRIVADO de solo lectura, tan restringido como el
    del caso MAP_PRIVATE, y la PoC lo demuestra en vez de suponerlo. Para
    alcanzar la comprobacion de VM_SHARED del modulo hay que abrir el
    dispositivo en O_RDWR, que es el tercer caso.
    """
    head("A2  mmap() con PROT_WRITE, y mmap() con MAP_SHARED")
    size = os.path.getsize(enc)
    problemas = []

    # --- Caso 1: PROT_WRITE sobre un fd O_RDONLY: lo deniega el modulo ---
    fd = dev_open()
    try:
        attach(fd, enc)
        addr, err = do_mmap(fd, size, PROT_READ | PROT_WRITE, MAP_PRIVATE)
        if addr is not None:
            libc.munmap(ctypes.c_void_p(addr), size)
            problemas.append("PROT_WRITE|MAP_PRIVATE fue concedido")
            info("PROT_READ|PROT_WRITE, MAP_PRIVATE: se CONCEDIO el mapeo")
        else:
            info(f"PROT_READ|PROT_WRITE, MAP_PRIVATE: DENEGADO con {errname(err)}")
            if err != 13:
                problemas.append(f"PROT_WRITE denegado con {errname(err)}, "
                                 "no con EACCES")

        # --- Caso 2: MAP_SHARED sobre un fd O_RDONLY: lo degrada el nucleo ---
        addr, err = do_mmap(fd, size, PROT_READ, MAP_SHARED)
        if addr is None:
            info(f"PROT_READ, MAP_SHARED (fd O_RDONLY): DENEGADO con "
                 f"{errname(err)}")
        else:
            try:
                perms = maps_flags(addr)
                vmf = vma_flags(addr)
                info(f"PROT_READ, MAP_SHARED (fd O_RDONLY): concedido; "
                     f"maps dice '{perms}'")
                if vmf is None:
                    problemas.append("no se pudo leer /proc/self/smaps")
                else:
                    info(f"    smaps VmFlags: {' '.join(sorted(vmf))}")
                    if "sh" in vmf:
                        problemas.append("el VMA conservo VM_SHARED ('sh' en "
                                         "VmFlags)")
                    elif "mw" in vmf:
                        problemas.append("el VMA conservo VM_MAYWRITE ('mw' en "
                                         "VmFlags)")
                    else:
                        info("    sin 'sh' ni 'mw': el nucleo retiro VM_SHARED "
                             "y VM_MAYWRITE porque el fd no tiene FMODE_WRITE "
                             "(mm/mmap.c:462-464)")
                        if perms and perms[3] == "s" and "ms" in vmf:
                            info("    maps sigue mostrando 's' porque ese "
                                 "caracter depende de VM_MAYSHARE, no de "
                                 "VM_SHARED (fs/proc/task_mmu.c:454)")
                merr = do_mprotect(addr, size, PROT_READ | PROT_WRITE)
                if merr == 0:
                    problemas.append("el mapeo degradado SI admitio PROT_WRITE")
                    info("    mprotect(PROT_WRITE) fue CONCEDIDO")
                else:
                    info(f"    mprotect(PROT_WRITE) sobre el: DENEGADO con "
                         f"{errname(merr)}; VM_MAYWRITE tambien se retiro")
            finally:
                libc.munmap(ctypes.c_void_p(addr), size)

        print_stats(stats(fd))
    finally:
        os.close(fd)

    # --- Caso 3: MAP_SHARED sobre un fd O_RDWR: aqui si actua el modulo ---
    try:
        fd = dev_open(os.O_RDWR)
    except EnvError as e:
        info(f"no se pudo abrir {DEV} en O_RDWR: {e}")
        return inconc("sin un fd O_RDWR no se puede ejercitar la comprobacion "
                      "de VM_SHARED del modulo")
    try:
        attach(fd, enc)
        addr, err = do_mmap(fd, size, PROT_READ, MAP_SHARED)
        if addr is not None:
            vmf = vma_flags(addr)
            libc.munmap(ctypes.c_void_p(addr), size)
            info(f"PROT_READ, MAP_SHARED (fd O_RDWR): se CONCEDIO "
                 f"(VmFlags: {' '.join(sorted(vmf)) if vmf else '?'})")
            problemas.append("MAP_SHARED con un fd escribible fue concedido")
        else:
            info(f"PROT_READ, MAP_SHARED (fd O_RDWR): DENEGADO con {errname(err)}")
            if err != 13:
                problemas.append(f"MAP_SHARED denegado con {errname(err)}, "
                                 "no con EACCES")
        st = stats(fd)
        print_stats(st)
        if st.denied_mmap < 1 and not problemas:
            problemas.append("el modulo denego pero no incremento denied_mmap")
    finally:
        os.close(fd)

    if problemas:
        return bad("; ".join(problemas))
    return ok("PROT_WRITE lo ataja mg_mmap(); MAP_SHARED lo degrada el nucleo "
              "con un fd de solo lectura y lo ataja mg_mmap() con uno escribible")


# --------------------------------------------------------------------------
# A3 - Escalada con mprotect
# --------------------------------------------------------------------------

def poc_a3(enc, claro=None):
    """mprotect(PROT_WRITE) sobre el mapeo concedido debe dar EACCES.

    Lo rechaza el propio nucleo, porque mg_mmap() retiro VM_MAYWRITE del VMA;
    el modulo no intercepta mprotect en ninguna parte. La consecuencia
    observable es que denied_mmap NO se incrementa: la llamada no llega al
    modulo. Esa asimetria es la prueba de que la defensa esta en el nucleo.

    El errno es EACCES, no EPERM: la comprobacion es
    `(newflags & ~(newflags >> 4)) & VM_ACCESS_FLAGS` en
    `mm/mprotect.c:959-963`, que contrasta cada permiso pedido contra su
    VM_MAY* correspondiente y devuelve -EACCES.
    """
    head("A3  Escalada de privilegios con mprotect(PROT_WRITE)")
    size = os.path.getsize(enc)
    fd = dev_open()
    try:
        attach(fd, enc)
        addr, err = do_mmap(fd, size, PROT_READ, MAP_PRIVATE)
        if addr is None:
            raise EnvError(f"el mapeo de solo lectura fallo: {errname(err)}")
        before = stats(fd)
        try:
            _ = ctypes.string_at(addr, 16)   # materializa la primera pagina
            err = do_mprotect(addr, size, PROT_READ | PROT_WRITE)
            if err == 0:
                return bad("mprotect(PROT_WRITE) fue concedido")
            info(f"mprotect(PROT_WRITE) DENEGADO: {errname(err)}")
            after = stats(fd)
            print_stats(after)
            if after.denied_mmap != before.denied_mmap:
                info("nota: denied_mmap cambio, luego la llamada paso por el "
                     "modulo y no por la comprobacion de VM_MAYWRITE")
            else:
                info("denied_mmap no cambio: la llamada no llego al modulo, "
                     "la rechazo el nucleo al ver que falta VM_MAYWRITE")
            if err != 13:
                return inconc(f"denegado, pero con {errname(err)} en vez de "
                              "EACCES (mm/mprotect.c:959-963)")
            return ok("el nucleo cierra la escalada sin interceptar mprotect")
        finally:
            libc.munmap(ctypes.c_void_p(addr), size)
    finally:
        os.close(fd)


# --------------------------------------------------------------------------
# A4 - Evasion de permisos POSIX (pregunta 8 del enunciado)
# --------------------------------------------------------------------------

def poc_a4(enc, claro=None):
    """O_RDONLY + PROT_WRITE + MAP_PRIVATE.

    Dos mitades. Primero se demuestra que la evasion existe: sobre un archivo
    ORDINARIO abierto en O_RDONLY el nucleo concede el mapeo escribible, se
    puede alterar el contenido en memoria y el archivo en disco no cambia.
    Despues se demuestra que sobre el dispositivo protegido el mismo ataque
    es rechazado.
    """
    head("A4  fd en O_RDONLY mapeado PROT_WRITE|MAP_PRIVATE")
    if not claro:
        return inconc("hace falta un archivo ordinario de referencia (--claro)")

    # --- Mitad 1: sobre un archivo ordinario, el nucleo lo permite ---
    size = min(os.path.getsize(claro), PAGE)
    with open(claro, "rb") as f:
        digest_before = hashlib.sha256(f.read()).hexdigest()
    ffd = os.open(claro, os.O_RDONLY)
    leaked = False
    try:
        addr, err = do_mmap(ffd, size, PROT_READ | PROT_WRITE, MAP_PRIVATE)
        if addr is None:
            info(f"sobre '{os.path.basename(claro)}' el mapeo fue rechazado: "
                 f"{errname(err)}")
        else:
            try:
                original = ctypes.string_at(addr, 8)
                ctypes.memset(ctypes.c_void_p(addr), 0x41, 8)
                mutated = ctypes.string_at(addr, 8)
                leaked = mutated != original
                info(f"sobre '{os.path.basename(claro)}' (O_RDONLY): el nucleo "
                     f"CONCEDE el mapeo escribible")
                info(f"    en memoria: {original!r} -> {mutated!r}")
            finally:
                libc.munmap(ctypes.c_void_p(addr), size)
    finally:
        os.close(ffd)

    with open(claro, "rb") as f:
        digest_after = hashlib.sha256(f.read()).hexdigest()
    if digest_before != digest_after:
        return bad("MAP_PRIVATE propago la escritura al archivo en disco")
    if leaked:
        info("el archivo en disco NO cambio (sha256 identico): la alteracion "
             "vive solo en la copia COW del proceso")
        info("esa es la evasion que describe la pregunta 8: O_RDONLY no impide "
             "alterar los datos en memoria ni verlos en claro")
    else:
        info("no se pudo demostrar la mitad vulnerable sobre el archivo ordinario")

    # --- Mitad 2: sobre el dispositivo protegido, se deniega ---
    esize = os.path.getsize(enc)
    fd = dev_open()
    try:
        attach(fd, enc)
        addr, err = do_mmap(fd, esize, PROT_READ | PROT_WRITE, MAP_PRIVATE)
        if addr is not None:
            libc.munmap(ctypes.c_void_p(addr), esize)
            return bad("sobre /dev/mmapguard el mapeo escribible fue concedido")
        info(f"sobre {DEV}: DENEGADO con {errname(err)}")
        print_stats(stats(fd))
        if not leaked:
            return inconc("el dispositivo deniega el ataque, pero no se pudo "
                          "demostrar que la evasion exista sobre un archivo "
                          "ordinario")
        return ok("la evasion existe en el caso general y mg_mmap() la cierra "
                  "para los archivos protegidos")
    finally:
        os.close(fd)


# --------------------------------------------------------------------------
# A5 - Carrera entre madvise(MADV_DONTNEED) y escritura (pregunta 9)
# --------------------------------------------------------------------------

def _a5_write_child(enc):
    """Hijo aislado: intenta escribir en el mapeo protegido.

    Se ejecuta en un proceso aparte porque un SIGSEGV no se puede capturar
    desde Python: la muerte del hijo por SIGSEGV ES el resultado positivo.
    """
    size = os.path.getsize(enc)
    pid = os.fork()
    if pid == 0:
        # Los codigos de salida distinguen 'no se pudo montar la prueba' de
        # 'la prueba se monto y la escritura fue permitida'.
        try:
            fd = os.open(DEV, os.O_RDONLY)
            attach(fd, enc)
        except BaseException:
            os._exit(74)
        try:
            addr, err = do_mmap(fd, size, PROT_READ, MAP_PRIVATE)
            if addr is None:
                os._exit(71)
            _ = ctypes.string_at(addr, 16)      # materializa la pagina
            ctypes.memset(ctypes.c_void_p(addr), 0x41, 1)   # debe morir aqui
            os._exit(72)                        # si sigue vivo: fallo
        except BaseException:
            os._exit(73)
    _, status = os.waitpid(pid, 0)
    return status


def poc_a5(enc, claro=None, seconds=None, nthreads=4):
    """La carrera de la pregunta 9 no tiene precondicion sobre este mapeo.

    Fase 1: la escritura que la carrera necesitaria es imposible; la MMU mata
    al proceso. Sin escritura no hay nada que serializar contra el descarte.

    Fase 2: se martillea madvise(MADV_DONTNEED) desde varios hilos mientras el
    hilo principal relee y verifica el contenido completo. Se mide minflt para
    probar que las PTE se estaban descartando de verdad -- sin ese dato el
    martilleo no demuestra nada -- y se comparan los contadores del driver
    para ver si el descarte obliga a redescifrar.
    """
    head("A5  Carrera entre madvise(MADV_DONTNEED) y escritura")
    if seconds is None:
        try:
            seconds = float(os.environ.get("MG_A5_SECONDS", "2"))
        except ValueError:
            seconds = 2.0

    # --- Fase 1 ---
    info("fase 1: un proceso hijo intenta escribir en el mapeo protegido")
    status = _a5_write_child(enc)
    if os.WIFSIGNALED(status):
        sig = os.WTERMSIG(status)
        if sig == signal.SIGSEGV:
            info("    el hijo murio con SIGSEGV: la escritura la rechaza la "
                 "MMU por el bit R/W de la PTE, sin llegar al modulo")
        else:
            return inconc(f"el hijo murio con la senal {sig} "
                          f"({signal.Signals(sig).name}), no SIGSEGV")
    else:
        code = os.WEXITSTATUS(status)
        if code == 72:
            return bad("la escritura sobre el mapeo protegido tuvo exito")
        if code == 74:
            raise EnvError(f"el proceso hijo no pudo abrir ni asociar {DEV}: "
                           "compruebe que el modulo esta cargado, que el "
                           "dispositivo es 666 y que su UID es el autorizado")
        if code == 71:
            raise EnvError("el hijo no consiguio el mapeo de solo lectura")
        raise EnvError(f"el hijo termino de forma inesperada (codigo {code})")

    # --- Fase 2 ---
    size = os.path.getsize(enc)
    fd = dev_open()
    try:
        attach(fd, enc)
        addr, err = do_mmap(fd, size, PROT_READ, MAP_PRIVATE)
        if addr is None:
            raise EnvError(f"el mapeo de solo lectura fallo: {errname(err)}")
        try:
            ref = hashlib.sha256(ctypes.string_at(addr, size)).hexdigest()
            st0 = stats(fd)
            mf0 = resource.getrusage(resource.RUSAGE_SELF).ru_minflt

            info(f"fase 2: {nthreads} hilos martilleando MADV_DONTNEED "
                 f"durante {seconds:.0f}s mientras se relee el mapeo")
            stop = threading.Event()
            rounds = [0] * nthreads

            def hammer(idx):
                # ctypes libera el GIL en la llamada foránea: la concurrencia
                # con el hilo lector es real.
                n = 0
                while not stop.is_set():
                    libc.madvise(ctypes.c_void_p(addr), size, MADV_DONTNEED)
                    n += 1
                rounds[idx] = n

            ths = [threading.Thread(target=hammer, args=(i,), daemon=True)
                   for i in range(nthreads)]
            for t in ths:
                t.start()

            reads = mismatches = 0
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                if hashlib.sha256(ctypes.string_at(addr, size)).hexdigest() != ref:
                    mismatches += 1
                reads += 1

            stop.set()
            for t in ths:
                t.join()

            mf1 = resource.getrusage(resource.RUSAGE_SELF).ru_minflt
            st1 = stats(fd)
            minflt = mf1 - mf0

            info(f"    descartes: {sum(rounds)}  relecturas completas: {reads}  "
                 f"discrepancias: {mismatches}")
            info(f"    fallos de pagina menores del proceso: +{minflt}")
            info(f"    contadores del driver: faults +{st1.faults - st0.faults}, "
                 f"bytes descifrados +{st1.bytes_decrypted - st0.bytes_decrypted}")

            if mismatches:
                return bad(f"el contenido del mapeo cambio en {mismatches} de "
                           f"{reads} relecturas")
            if minflt == 0:
                return inconc("no hubo fallos de pagina adicionales: el "
                              "martilleo no llego a descartar ninguna PTE y "
                              "la prueba no demuestra nada")
            if st1.faults == st0.faults:
                info("el driver no redescifro: el descarte retira la PTE pero "
                     "la pagina en claro sigue en la cache de la sesion")
            return ok("sin escritura posible no hay carrera; el descarte "
                      "repetido no altera ni corrompe el contenido")
        finally:
            libc.munmap(ctypes.c_void_p(addr), size)
    finally:
        os.close(fd)


# --------------------------------------------------------------------------
# Despacho
# --------------------------------------------------------------------------

POCS = {
    "unauth": (poc_unauth, "A0  asociacion desde un UID no autorizado"),
    "a1": (poc_a1, "A1  lectura directa del archivo cifrado"),
    "read": (poc_read, "OK  control positivo: el mapeo descifra correctamente"),
    "a2": (poc_a2, "A2  mmap con PROT_WRITE y con MAP_SHARED"),
    "a3": (poc_a3, "A3  escalada con mprotect(PROT_WRITE)"),
    "a4": (poc_a4, "A4  evasion de permisos POSIX (O_RDONLY + PROT_WRITE)"),
    "a5": (poc_a5, "A5  carrera madvise(MADV_DONTNEED) contra escritura"),
}


def usage(stream=sys.stdout):
    print(__doc__.strip(), file=stream)
    print("\nPoC disponibles:", file=stream)
    for name, (_, desc) in POCS.items():
        print(f"    {name:<8} {desc}", file=stream)


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help", "--list"):
        usage()
        return RC_PASS

    name = argv[1]
    if name not in POCS:
        print(f"PoC desconocida: {name}", file=sys.stderr)
        usage(sys.stderr)
        return RC_ENV

    args = argv[2:]
    enc = args[0] if args else "demo/secreto.enc"
    claro = args[1] if len(args) > 1 else None

    fn = POCS[name][0]
    try:
        need_file(enc)
        if claro:
            need_file(claro)
        if name in ("unauth", "a1"):
            return fn(enc, claro) if name == "a1" else fn(enc)
        return fn(enc, claro)
    except EnvError as e:
        print("    " + _c("1;33", f"-> ENV: {e}"))
        return RC_ENV
    except OSError as e:
        print("    " + _c("1;33", f"-> ENV: error del sistema: {e}"))
        return RC_ENV


if __name__ == "__main__":
    sys.exit(main(sys.argv))
