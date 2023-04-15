from __future__ import annotations
from collections import defaultdict
from itertools import chain
from pathlib import Path
from struct import Struct
from typing import (
    BinaryIO, ClassVar, Generator, Iterable, Mapping, NamedTuple, NewType,
    Optional, Type, TypeVar,
)

import re

_re = re.compile('|'.join((
    r'(?P<reg>\b[A-Z]+\b)',
    r'(?P<number>([0-9]+|$[0-9a-fA-F]+))',
    r'(?P<operand>(!a|#i|[a-z]+(\.[a-z]+)?))',
    r'(?P<lparen>\()',
    r'(?P<rparen>\))',
    r'(?P<lsq>\[)',
    r'(?P<rsq>\])',
    r'(?P<plus>\+)',
    r'(?P<symbol>[\.,\/])',
    r'(?P<comment>#.*?(?=\n))',          # up to but not including newline
    r'(?P<whitespace>[^\S\n][^\S\n]*)',  # non-newline whitespace
    r'(?P<newline>\n)',
)))
_ignore_toks = frozenset({'whitespace', 'comment', 'newline'})


Opcode = NewType('Opcode', int)


class _Reg(NamedTuple):
    reg: str


class _Sym(NamedTuple):
    sym: str


class _Adr(NamedTuple):
    adr: str


class Token:
    __slots__ = ()


class Sym(_Sym, Token):
    __slots__ = ()

    def __str__(self) -> str:
        return self.sym


class Reg(_Reg, Token):
    __slots__ = ()

    def __str__(self) -> str:
        return self.reg


class Adr(_Adr, Token):
    __slots__ = ()

    @property
    def fmt(self) -> str:
        tab = {
            '!a': 'H',
            '#i': 'B',
            'd': 'B',
            'r': 'b',
            'm.b': 'H',
            'v': 'B',
        }
        return tab[self.adr]

    @staticmethod
    def _fmt_x8(v: int, addr: Optional[int] = None) -> str:
        return f'${v:02x}'

    @staticmethod
    def _fmt_x16(v: int, addr: Optional[int] = None) -> str:
        return f'${v:04x}'

    @staticmethod
    def _fmt_imm(v: int, addr: Optional[int] = None) -> str:
        return f'#${v:02x}'

    @staticmethod
    def _fmt_rel(v: int, addr: Optional[int] = None) -> str:
        if addr is None:
            return str(v)
        else:
            return f'${addr + v:04x}'

    @staticmethod
    def _fmt_membit(v: int, addr: Optional[int] = None) -> str:
        bit = v >> 13
        mem = v & 0x1fff
        return f'${mem:04x}.{bit}'

    _ftab = {
        '!a': _fmt_x16,
        'd': _fmt_x8,
        'v': _fmt_x8,
        'r': _fmt_rel,
        '#i': _fmt_imm,
        'm.b': _fmt_membit,
    }

    def format(self, v: int, addr: Optional[int] = None) -> str:
        return self._ftab[self.adr](v, addr)


TokenStream = tuple[Token, ...]

_lookup: dict[str, Type[Reg] | Type[Sym] | Type[Adr]] = {
    'reg': Reg,
    'number': Sym,
    'operand': Adr,
    'lparen': Sym,
    'rparen': Sym,
    'lsq': Sym,
    'rsq': Sym,
    'plus': Sym,
    'symbol': Sym,
}


def _tokenize(s: str) -> Generator[Token, None, None]:
    pos = 0

    while True:
        m = _re.match(s, pos)
        if not m:
            break
        grp = m.lastgroup
        begin, end = m.span()
        pos = end
        if grp in _ignore_toks:
            continue
        assert grp is not None
        val = s[begin:end]
        yield _lookup[grp](val)

    if pos < len(s):
        bad = s[pos:]
        raise Exception('Lex error', bad)


class OpDef(NamedTuple):
    opcode: Opcode
    mnemonic: str
    optoks: tuple[TokenStream, ...]


def load_opcode_tbl(p: Path,
                    ) -> Generator[OpDef, None, None]:
    with p.open() as f:
        for line in f:
            line = line.strip()
            opval, mnemonic, *opt_oprs = line.split(None, maxsplit=2)
            if opt_oprs:
                oprs, = opt_oprs
                opt_oprs = oprs.split(',')
                toks = tuple(tuple(_tokenize(opr)) for opr in opt_oprs)
            else:
                toks = ()
            yield OpDef(
                Opcode(int(opval, 16)),
                mnemonic,
                toks,
            )


OpcodeTable = Iterable[OpDef]


class DisOp(NamedTuple):
    mnemonic: str
    opr_struct: Struct
    dis_toks: TokenStream


class Dis:
    __slots__ = (
        '_tab',
    )

    _mtab: Mapping[Opcode, DisOp]

    def __init__(self, opcode_tbl: OpcodeTable) -> None:
        oprs: dict[tuple[Adr, ...], Struct] = {}

        def _adr(s: Iterable[Token]) -> tuple[Adr, ...]:
            return tuple(t for t in s if isinstance(t, Adr))

        def op(d: OpDef) -> DisOp:
            t = tuple(chain(*d.optoks))
            mode = _adr(t)
            try:
                s = oprs[mode]
            except KeyError:
                s = Struct(''.join(a.fmt for a in mode))
                oprs[mode] = s

            return DisOp(
                d.mnemonic,
                s,
                t,
            )

        self._tab = {d.opcode: op(d) for d in opcode_tbl}

    @staticmethod
    def _fmt(toks: TokenStream,
             operands: Iterable[int],
             addr: Optional[int] = None,
             ) -> Generator[str, None, None]:
        it = iter(operands)
        for t in toks:
            if isinstance(t, Adr):
                yield t.format(next(it), addr)
            else:
                yield str(t)

    def dis(self, b: bytes,
            addr: Optional[int] = None,
            ) -> Generator[str, None, None]:
        it = iter(b)
        next_addr = None
        while True:
            try:
                cur_op = next(it)
            except StopIteration:
                break

            op = Opcode(cur_op)
            mnem, a, t = self._tab[op]
            try:
                opbuf = bytes(next(it) for _ in range(a.size))
            except RuntimeError:
                break

            if addr is not None:
                next_addr = addr + 1 + a.size

            operands = a.unpack(opbuf)
            args = ''.join(self._fmt(t, operands, addr=next_addr))
            line = f'{mnem:6s}{args}'
            if addr is None:
                yield line
            else:
                yield f'{addr:04x}: {line}'
                addr = next_addr


_T = TypeVar('_T', bound='SerdeMixin')


class SerdeMixin:
    __slots__ = ()

    _fmt: ClassVar[Struct]
    size: ClassVar[int]

    @classmethod
    def read_from(cls: Type[_T], f: BinaryIO) -> _T:
        buf = f.read(cls.size)
        return cls.unpack_from(buf)

    @classmethod
    def unpack_from(cls: Type[_T], buf: bytes, off: int = 0) -> _T:
        return cls(*cls._fmt.unpack_from(buf, off))

    def pack(self) -> bytes:
        return self._fmt.pack(*self)  # type: ignore


class _SPC_Hdr(NamedTuple):
    format_id: str
    magic: int
    id666_tag_status: int
    version_minor: int


class SPC_Hdr(_SPC_Hdr, SerdeMixin):
    _fmt = Struct('<33sHBB')
    size = _fmt.size

    __slots__ = ()


class _SPC_Regs(NamedTuple):
    pc: int
    a: int
    x: int
    y: int
    psw: int
    sp: int


class SPC_Regs(_SPC_Regs, SerdeMixin):
    _fmt = Struct('<HBBBBBxx')
    size = _fmt.size

    __slots__ = ()


class SPCFile:
    __slots__ = (
        'hdr',
        'regs',
        'aram',
        'dsp_regs',
        'extra_ram',
    )

    hdr: SPC_Hdr
    regs: SPC_Regs
    aram: bytes
    dsp_regs: bytes
    extra_ram: bytes

    def __init__(self, f: BinaryIO) -> None:
        self.hdr = SPC_Hdr.read_from(f)
        self.regs = SPC_Regs.read_from(f)
        _ = f.read(210)
        self.aram = f.read(0x10000)
        self.dsp_regs = f.read(0x80)
        _ = f.read(64)
        self.extra_ram = f.read(64)

    @classmethod
    def from_file(cls, p: Path) -> SPCFile:
        with p.open('rb') as f:
            return cls(f)


def asm_table(opcode_tbl: OpcodeTable,
              ) -> dict[str, dict[tuple[TokenStream, ...], Opcode]]:
    ret: defaultdict[str, dict[tuple[TokenStream, ...],
                               Opcode]] = defaultdict(dict)
    for op, mnemonic, toks in opcode_tbl:
        ret[mnemonic][toks] = op
    return ret


def modes(opcode_tbl: OpcodeTable) -> None:
    addr_modes = set()
    for opdef in opcode_tbl:
        for operand in opdef.optoks:
            if any(isinstance(term, Adr) for term in operand):
                addr_modes.add(operand)

    for mode in sorted(addr_modes):
        print(mode)


def main() -> None:
    opcodes = tuple(load_opcode_tbl(Path('tbl/spc700.opcode.tbl')))
    # asm = asm_table(opcodes)
    modes(opcodes)
    dis = Dis(opcodes)

    p = Path.home() / 'spc/sf2/03 Player Select.spc'
    spc = SPCFile.from_file(p)
    print(spc.regs)

    off = 0x02f8
    end = 0x0d05
    for insn in dis.dis(spc.aram[off:end], addr=off):
        print(insn)


if __name__ == '__main__':
    main()
