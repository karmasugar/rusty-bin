import dataclasses

import capstone
from capstone import x86

CONDITIONAL_JUMPS = {
    "jo",
    "jno",
    "jb",
    "jae",
    "je",
    "jne",
    "jbe",
    "ja",
    "js",
    "jns",
    "jp",
    "jnp",
    "jl",
    "jge",
    "jle",
    "jg",
    "jcxz",
    "loop",
    "loope",
    "loopne",
}
FLOW_END = {"ret", "retf", "iret", "jmp", "ljmp"}
MNEMONIC_ALIASES = {
    "pushaw": "pusha",
    "popaw": "popa",
    "pushfw": "pushf",
    "popfw": "popf",
    "iretw": "iret",
    "retfw": "retf",
}

# capstone 5 names the 16-bit forms of 98h/99h with their 32-bit names; the size (no 66h prefix) tells them apart.
SIXTEEN_BIT_NAMES = {"cwde": "cbw", "cdq": "cwd"}

Address = tuple[int, int]


class AnalysisError(Exception):
    pass


@dataclasses.dataclass(frozen=True)
class Operand:
    kind: str
    size: int
    reg: str | None = None
    imm: int | None = None
    segment: str | None = None
    base: str | None = None
    index: str | None = None
    disp: int = 0
    relocated: bool = False


@dataclasses.dataclass(frozen=True)
class Instruction:
    segment: int
    address: int
    size: int
    mnemonic: str
    text: str
    operands: tuple[Operand, ...]
    rep: str | None

    @property
    def next_address(self) -> int:
        return (self.address + self.size) & 0xFFFF

    def direct_target(self) -> int | None:
        if len(self.operands) == 1 and self.operands[0].kind == "imm":
            return self.operands[0].imm & 0xFFFF
        return None

    def far_target(self) -> Address | None:
        if self.mnemonic in ("lcall", "ljmp") and len(self.operands) == 2 and self.operands[0].relocated:
            return (self.operands[0].imm & 0xFFFF, self.operands[1].imm & 0xFFFF)
        return None


@dataclasses.dataclass
class Function:
    segment: int
    entry: int
    instructions: dict[int, Instruction] = dataclasses.field(default_factory=dict)
    labels: set[int] = dataclasses.field(default_factory=set)
    calls: set[Address] = dataclasses.field(default_factory=set)
    tail_calls: set[Address] = dataclasses.field(default_factory=set)

    @property
    def key(self) -> Address:
        return (self.segment, self.entry)


@dataclasses.dataclass
class Program:
    image: bytes
    base: int
    functions: dict[Address, Function]


def convert_operand(ins: capstone.CsInsn, operand: x86.X86Op, *, relocated: bool) -> Operand:
    if operand.type == x86.X86_OP_REG:
        return Operand(kind="reg", size=operand.size, reg=ins.reg_name(operand.reg))
    if operand.type == x86.X86_OP_IMM:
        return Operand(kind="imm", size=operand.size, imm=operand.imm, relocated=relocated)
    memory = operand.mem
    return Operand(
        kind="mem",
        size=operand.size,
        segment=ins.reg_name(memory.segment) if memory.segment else None,
        base=ins.reg_name(memory.base) if memory.base else None,
        index=ins.reg_name(memory.index) if memory.index else None,
        disp=memory.disp,
    )


class Decoder:
    def __init__(self, image: bytes, *, base: int, relocations: set[int]) -> None:
        self.image = image
        self.base = base
        self.relocations = relocations
        self.engine = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
        self.engine.detail = True
        self.cache: dict[Address, Instruction] = {}

    def linear(self, segment: int, address: int) -> int:
        return segment * 16 + address - self.base

    def decode(self, segment: int, address: int) -> Instruction:
        key = (segment, address)
        if key in self.cache:
            return self.cache[key]
        offset = self.linear(segment, address)
        if not 0 <= offset < len(self.image):
            raise AnalysisError(f"address {segment:04x}:{address:04x} is outside the image")
        found = next(self.engine.disasm(self.image[offset : offset + 16], address, count=1), None)
        if found is None:
            raise AnalysisError(f"cannot decode at {segment:04x}:{address:04x}")
        mnemonic = found.mnemonic
        rep = None
        for prefix in ("repne ", "repe ", "rep "):
            if mnemonic.startswith(prefix):
                rep = prefix.strip()
                mnemonic = mnemonic[len(prefix) :]
        mnemonic = MNEMONIC_ALIASES.get(mnemonic, mnemonic)
        if found.size == 1 and mnemonic in SIXTEEN_BIT_NAMES:
            mnemonic = SIXTEEN_BIT_NAMES[mnemonic]
        immediate_relocated = bool(found.imm_offset) and (offset + found.imm_offset) in self.relocations
        operands = []
        for position, operand in enumerate(found.operands):
            relocated = False
            if immediate_relocated and operand.type == x86.X86_OP_IMM:
                relocated = mnemonic not in ("lcall", "ljmp") or position == 0
            operands.append(convert_operand(found, operand, relocated=relocated))
        instruction = Instruction(
            segment=segment,
            address=address,
            size=found.size,
            mnemonic=mnemonic,
            text=f"{found.mnemonic} {found.op_str}".strip(),
            operands=tuple(operands),
            rep=rep,
        )
        self.cache[key] = instruction
        return instruction


def explore_function(decoder: Decoder, key: Address, *, entries: set[Address]) -> Function:
    segment, entry = key
    function = Function(segment=segment, entry=entry)
    function.labels.add(entry)
    pending = [entry]
    while pending:
        address = pending.pop()
        previous = None
        while address not in function.instructions:
            instruction = decoder.decode(segment, address)
            function.instructions[address] = instruction
            mnemonic = instruction.mnemonic
            target = instruction.direct_target()
            if mnemonic == "call" and target is not None:
                function.calls.add((segment, target))
            far = instruction.far_target()
            if far is not None and mnemonic == "lcall":
                function.calls.add(far)
            if far is not None and mnemonic == "ljmp":
                function.tail_calls.add(far)
            if mnemonic in CONDITIONAL_JUMPS:
                function.labels.add(target)
                pending.append(target)
            elif mnemonic == "jmp" and target is not None:
                if (segment, target) in entries and target != entry:
                    function.tail_calls.add((segment, target))
                else:
                    function.labels.add(target)
                    pending.append(target)
                break
            if mnemonic in FLOW_END or ends_program(instruction, previous=previous):
                break
            previous = instruction
            address = instruction.next_address
    return function


def ends_program(instruction: Instruction, *, previous: Instruction | None) -> bool:
    if instruction.mnemonic != "int" or not instruction.operands:
        return False
    number = instruction.operands[0].imm
    if number in (0x20, 0x27):
        return True
    if number != 0x21 or previous is None or previous.mnemonic != "mov" or len(previous.operands) != 2:
        return False
    target, source = previous.operands
    if target.kind != "reg" or source.kind != "imm":
        return False
    ah = {"ax": (source.imm >> 8) & 0xFF, "ah": source.imm & 0xFF}.get(target.reg)
    return ah in (0x4C, 0x31)


def interrupt_handlers(functions: dict[Address, Function]) -> set[Address]:
    handlers = set()
    for function in functions.values():
        dx_value = None
        ah_value = None
        for address in sorted(function.instructions):
            instruction = function.instructions[address]
            ops = instruction.operands
            if instruction.mnemonic == "mov" and len(ops) == 2 and ops[0].kind == "reg" and ops[1].kind == "imm":
                if ops[0].reg == "dx":
                    dx_value = ops[1].imm & 0xFFFF
                elif ops[0].reg == "ax":
                    ah_value = (ops[1].imm >> 8) & 0xFF
                elif ops[0].reg == "ah":
                    ah_value = ops[1].imm & 0xFF
            elif instruction.mnemonic == "int" and ops and ops[0].imm == 0x21:
                if ah_value == 0x25 and dx_value is not None:
                    handlers.add((function.segment, dx_value))
            elif instruction.mnemonic in ("call", "ret", "retf", "jmp"):
                dx_value = None
                ah_value = None
    return handlers


def callback_targets(functions: dict[Address, Function]) -> set[Address]:
    slots = set()
    for function in functions.values():
        for instruction in function.instructions.values():
            if instruction.mnemonic in ("call", "lcall", "jmp", "ljmp") and len(instruction.operands) == 1:
                operand = instruction.operands[0]
                if operand.kind == "mem" and operand.base is None and operand.index is None:
                    slots.add((function.segment, operand.disp & 0xFFFF))
    targets = set()
    for function in functions.values():
        for instruction in function.instructions.values():
            ops = instruction.operands
            if instruction.mnemonic == "mov" and len(ops) == 2 and ops[0].kind == "mem" and ops[1].kind == "imm":
                simple = ops[0].base is None and ops[0].index is None and ops[0].size == 2
                if simple and (function.segment, ops[0].disp & 0xFFFF) in slots:
                    targets.add((function.segment, ops[1].imm & 0xFFFF))
    return targets


def analyze_entries(
    image: bytes, *, base: int, entries: list[Address], relocations: set[int], optional: set[Address]
) -> Program:
    decoder = Decoder(image, base=base, relocations=relocations)
    known = set(entries)
    functions: dict[Address, Function] = {}
    pending = list(entries)
    while pending:
        key = pending.pop()
        if key in functions:
            continue
        try:
            function = explore_function(decoder, key, entries=known)
        except AnalysisError:
            if key not in optional:
                raise
            optional.discard(key)
            known.discard(key)
            continue
        functions[key] = function
        for callee in function.calls | function.tail_calls:
            if callee not in known:
                known.add(callee)
                pending.append(callee)
    changed = True
    while changed:
        changed = False
        for key in list(functions):
            rebuilt = explore_function(decoder, key, entries=known)
            if rebuilt.tail_calls != functions[key].tail_calls or len(rebuilt.instructions) != len(
                functions[key].instructions
            ):
                functions[key] = rebuilt
                changed = True
    return Program(image=image, base=base, functions=functions)


def executed_constants(program: Program, *, executed: set[int]) -> set[Address]:
    segments = {function.segment for function in program.functions.values()}
    found = set()
    for function in program.functions.values():
        for instruction in function.instructions.values():
            ops = instruction.operands
            value = None
            if instruction.mnemonic in ("mov", "push") and ops and ops[-1].kind == "imm" and ops[-1].size == 2:
                value = ops[-1].imm & 0xFFFF
            if value is None or ops[-1].relocated:
                continue
            for segment in segments:
                if segment * 16 + value - program.base in executed:
                    found.add((segment, value))
    return found


def jump_table_targets(program: Program, *, executed: set[int] | None) -> set[Address]:
    image = program.image
    starts = {
        (function.segment, address) for function in program.functions.values() for address in function.instructions
    }
    found = set()
    for function in program.functions.values():
        for instruction in function.instructions.values():
            if instruction.mnemonic not in ("jmp", "call") or len(instruction.operands) != 1:
                continue
            operand = instruction.operands[0]
            if operand.kind != "mem" or (operand.base is None and operand.index is None) or operand.size != 2:
                continue
            if not operand.disp:
                continue
            table = function.segment * 16 + (operand.disp & 0xFFFF) - program.base
            for index in range(256):
                position = table + index * 2
                if not 0 <= position < len(image) - 1:
                    break
                value = image[position] | (image[position + 1] << 8)
                code = function.segment * 16 + value - program.base
                if (function.segment, value) not in starts and not (executed and code in executed):
                    break
                found.add((function.segment, value))
    return found


def data_table_targets(program: Program, *, executed: set[int]) -> set[Address]:
    image = program.image
    hits = {}
    for offset in range(len(image) - 1):
        value = image[offset] | (image[offset + 1] << 8)
        if value >= 0x10 and value - program.base in executed:
            hits[offset] = value
    found = set()
    for offset, value in hits.items():
        for neighbour in (offset - 2, offset + 2):
            if neighbour in hits and hits[neighbour] != value:
                found.add((0, value))
    return found


def analyze(
    image: bytes,
    *,
    base: int,
    entries: list[Address],
    relocations: set[int] | None = None,
    executed: set[int] | None = None,
    data_tables: bool = False,
    optional_entries: list[Address] | None = None,
) -> Program:
    relocations = relocations or set()
    known = list(entries)
    optional: set[Address] = set()
    for key in optional_entries or []:
        if key not in known and 0 <= key[0] * 16 + key[1] - base < len(image):
            known.append(key)
            optional.add(key)
    while True:
        program = analyze_entries(image, base=base, entries=known, relocations=relocations, optional=optional)
        known = [key for key in known if key in program.functions or key not in optional]
        extra = (
            interrupt_handlers(program.functions)
            | callback_targets(program.functions)
            | jump_table_targets(program, executed=executed)
        )
        if executed:
            extra |= executed_constants(program, executed=executed)
            if data_tables:
                extra |= data_table_targets(program, executed=executed)
        found = [
            key
            for key in sorted(extra)
            if key not in program.functions and 0 <= key[0] * 16 + key[1] - base < len(image)
        ]
        found = [key for key in found if key not in known]
        if not found:
            return program
        optional.update(found)
        known += found
