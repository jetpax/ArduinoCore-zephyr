#!/usr/bin/env python

# Copyright (c) Arduino s.r.l. and/or its affiliated companies
# SPDX-License-Identifier: Apache-2.0

import argparse
import hashlib
import itertools
import os
import re
import struct
import sys
import textwrap
import traceback

from elftools.construct.macros import UNInt32, UNInt64
from elftools.common.exceptions import ELFError
from elftools.common.utils import parse_cstring_from_stream, struct_parse
from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection


NativePtr = None

def get_str_at(elf, addr):
    for section in elf.iter_sections():
        if section['sh_type'] == 'SHT_NOBITS' or addr < section['sh_addr'] or addr >= section['sh_addr'] + section['sh_size']:
            continue

        file_offset = section['sh_offset'] + addr - section['sh_addr']
        return parse_cstring_from_stream(elf.stream, file_offset).decode('utf-8', errors='replace')

    return None

def get_ptr_at(elf, addr):
    for section in elf.iter_sections():
        if section['sh_type'] == 'SHT_NOBITS' or addr < section['sh_addr'] or addr >= section['sh_addr'] + section['sh_size']:
            continue

        file_offset = section['sh_offset'] + addr - section['sh_addr']
        return struct_parse(NativePtr, elf.stream, file_offset)

    return None

def get_all_syms(elf):
    syms = {}
    for section in elf.iter_sections():
        if not isinstance(section, SymbolTableSection):
            continue
        for symbol in section.iter_symbols():
            syms[symbol.name] = symbol

    return syms

def get_llext_syms(elf):
    syms = {}
    for section in elf.iter_sections():
        if not isinstance(section, SymbolTableSection):
            continue
        for symbol in section.iter_symbols():
            if symbol.name.startswith("__llext_sym_"):
                llext_sym_addr = symbol['st_value']
                sym_name = get_str_at(elf, get_ptr_at(elf, llext_sym_addr))
                sym_value = get_ptr_at(elf, llext_sym_addr + NativePtr.length)
                if sym_value:
                    syms[sym_name] = sym_value

    return syms

def main():
    global NativePtr

    # parse the command-line arguments and invoke ReadElf
    argparser = argparse.ArgumentParser(
            formatter_class=argparse.RawDescriptionHelpFormatter,
            description="Extract symbols from ELF files",
            epilog=textwrap.dedent(f'''\
            SYMBOL DEFINITION
            -----------------
            Each <sym> describes symbol names to extract. This can be an exact symbol
            name, a regular expression, or a rename expression. Also, the symbol can be
            dereferenced, so that the pointer value stored at the symbol address is
            exported, or the size of the symbol can be exported as well.
            The exact rules are as follows:

             - if <sym> starts with a slash ('/'), the rest is treated as a regexp. All
               symbols matching the regexp will be extracted. Can be combined with
               dereferencing, but not renames.
             - if <sym> does not start with a slash, but contains an equals sign ('='),
               it is treated as a rename expression, where the part before the equals
               is the symbol name to extract, and the part after the equals is
               the new name to use in the output.
             - if the first char of <sym> is an asterisk ('*'), the symbol is
               dereferenced, i.e. the pointer value stored at the symbol address is
               exported instead of the symbol address itself.
             - if the first char of <sym> is a plus ('+'), in addition to itself, a
               second symbol called '<sym>_size' is defined with the size of the
               current symbol.

            For example, the symbol definition:

              *sketch_base_addr=__sketch_start

            will export the value stored at the address of the 'sketch_base_addr' symbol
            as '__sketch_start', while

              /__device_dts_ord_.*

            will export all symbols starting with '__device_dts_ord_' as-is. Also,

              +kheap_llext_heap

            will export the value of the 'kheap_llext_heap' symbol and its size in a
            separate 'kheap_llext_heap_size' symbol.
            '''))
    argparser.add_argument('-v', '--verbose',
            action='store_true',
            help='Write the source of the symbol definition as a comment')
    argparser.add_argument('-L', '--llext',
            action='store_true',
            help='Extract symbols from the __llext_sym_* symbols')
    argparser.add_argument('-F', '--funcs',
            action='store_true',
            help='Extract all public functions')
    argparser.add_argument('-T', '--tls-defs',
            action='store_true',
            help='Extract TLS symbol offsets to a .S file')
    argparser.add_argument('file',
            help='ELF file to parse')
    argparser.add_argument('syms', nargs='*',
            help='Symbols to export')

    args = argparser.parse_intermixed_args()

    wants_provides = bool(args.syms or args.funcs or args.llext)
    if wants_provides and args.tls_defs:
        sys.stderr.write("Cannot generate TLS defs when also generating PROVIDEs.\n")
        sys.exit(1)
    elif not wants_provides and not args.tls_defs:
        sys.stderr.write("Nothing specified for export.\n")
        sys.exit(1)

    exact_syms = set()
    regex_syms = set()
    deref_syms = set()
    sized_syms = set()
    tls_syms = set() # tuple of (name, value, size)
    rename_map = {}
    for sym in args.syms:
        sym_class = None
        if sym[0] == '/':
            # Regexp
            sym = f"^{sym[1:]}$"
            sym_class = "regexp"
        elif '=' in sym:
            # Rename expression
            sym, new_sym = sym.split('=')
            sym_class = "rename"
        else:
            # Exact symbol
            sym_class = "exact"

        if sym[0] == '*':
            # Dereference symbol
            sym = sym[1:]
            deref_syms.add(sym)
        elif sym[0] == '+':
            # Store size as well
            sym = sym[1:]
            sized_syms.add(sym)

        if sym_class == "regexp":
            regex_syms.add(sym)
        else:
            exact_syms.add(sym)
            if sym_class == "rename":
                rename_map[sym] = new_sym

    with open(args.file, 'rb') as file:
        try:
            elf_sha = hashlib.sha256(file.read()).hexdigest()
            elf = ELFFile(file)
        except ELFError as ex:
            sys.stdout.flush()
            sys.stderr.write('ELF error: %s\n' % ex)
            traceback.print_exc()
            sys.exit(1)

        if elf.elfclass == 32:
            NativePtr = UNInt32("ptr")
        elif elf.elfclass == 64:
            NativePtr = UNInt64("ptr")

        all_syms = get_all_syms(elf)
        out_syms = {}
        fail = False

        for name, sym in all_syms.items():
            if sym['st_info']['type'] == 'STT_TLS':
                # Collect TLS symbol information, ignore for PROVIDEs
                tls_syms.add((name, sym['st_value'], sym['st_size']))
                continue

            value = None
            comment = []
            if name in exact_syms or any(re.match(r, name) for r in regex_syms):
                comment = "cmd_line"
                value = sym['st_value']
            elif args.funcs and (sym['st_info']['type'] == 'STT_FUNC'
                            and sym['st_info']['bind'] == 'STB_GLOBAL'):
                comment = "public_fn"
                value = sym['st_value']
            elif args.llext and name.startswith("__llext_sym_"):
                comment = "llext_sym"
                llext_sym_addr = sym['st_value']
                name = get_str_at(elf, get_ptr_at(elf, llext_sym_addr))
                value = get_ptr_at(elf, llext_sym_addr + NativePtr.length)

            if name in deref_syms:
                value = get_ptr_at(elf, value)
            if name in rename_map:
                name = rename_map[name]

            if value is None:
                continue

            if name in out_syms:
                if out_syms[name][0] != value:
                    sys.stderr.write(
                        f"Warning: duplicate symbol {name} with different values: "
                        f"{out_syms[name][0]:#010x} vs {value:#010x}\n")
                    fail = True
                out_syms[name][1].append(comment)
            else:
                out_syms[name] = (value, [comment])

            if name in sized_syms:
                out_syms[name + "_size"] = (sym['st_size'], [f"size of {name}"])

        if fail:
            sys.exit(1)

        print(f"""
/*
 * Automatically generated by {os.path.basename(sys.argv[0])}, do not edit!
 *
 * Source: {args.file}
 * SHA256: {elf_sha}
 */
""")

        if wants_provides:
            if not out_syms:
                print("/* No symbols found matching the criteria */")
                sys.stderr.write("warning: no symbols found matching the criteria.\n")
            else:
                sym_comment = nul_comment = ""
                for name, (value, comments) in sorted(out_syms.items(), key=lambda x: x[0]):
                    if args.verbose:
                        comment = ', '.join(sorted(comments))
                        sym_comment = f"/* {comment} */"
                        nul_comment = f" ({comment})"
                    if value:
                        print(f"PROVIDE({name} = {value:#010x});{sym_comment}")
                    else:
                        print(f"/* NULL {name}{nul_comment} */")

        if args.tls_defs:
            # ARM and AArch64 use '@' as a line-comment character in GAS, so the
            # assembler requires '%' as the type prefix there.  All other targets use '@'.
            prefix = '%' if elf['e_machine'] in ('EM_ARM', 'EM_AARCH64') else '@'

            # The TLS Variant 1 layout (used by ARM/AArch64) places a Thread
            # Control Block (TCB) before the TLS data.  The thread pointer
            # returned by __aeabi_read_tp() points to the TCB, so the actual
            # runtime address of a TLS variable must be offset by that size.
            #
            # TCB is 2 pointers: 8 bytes on 32-bit, 16 bytes on 64-bit.
            # See zephyr/arch/arm/core/tls.c: arch_tls_stack_setup().
            tcb_size = (elf.elfclass // 8) * 2

            # Emit the loader's TLS symbols as a REAL .tbss section, NOT as
            # absolute (.set) values. ld cannot satisfy a TLS local-exec
            # relocation (R_AARCH64_TLSLE_* / R_ARM_TLS_LE32) against an SHN_ABS
            # symbol, so the moment a sketch actually references one of these
            # (e.g. errno in the WiFi/Net socket error path) an absolute .set
            # fails the link with "dangerous relocation: unsupported relocation".
            # Placing each symbol at its loader TLS-block offset in .tbss gives it
            # a real tpoff that matches the loader's per-thread slot, so the
            # link-time-baked TP+offset access lands on the running thread's
            # instance of the variable. _TLS_MODULE_BASE_ is omitted: once a
            # .tbss exists the linker defines that anchor itself, and an explicit
            # definition would be a multiple definition.
            syms = sorted(
                (offset, size, name)
                for name, offset, size in tls_syms
                if name != '_TLS_MODULE_BASE_'
            )
            if syms:
                print(f"\n/* tpoffs include {tcb_size} bytes for TCB data */")
                print(f'.section .tbss,"awT",{prefix}nobits')
                print(".balign 8")
                cur = 0
                for offset, size, name in syms:
                    if offset > cur:
                        print(f".zero {offset - cur}")
                        cur = offset
                    print(
                        f"/* tpoff {offset + tcb_size:#x}: {name} ({size} bytes) */\n"
                        f".global {name}\n"
                        f".type {name}, {prefix}tls_object\n"
                        f"{name}:"
                    )
                    if size > 0:
                        print(f".zero {size}")
                        cur += size

#-------------------------------------------------------------------------------
if __name__ == '__main__':
    main()
