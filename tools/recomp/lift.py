from dataclasses import dataclass, field

from flags import (
    ARITHMETIC,
    COMPARE,
    CONDITION_USES,
    LOGIC,
    RESULT,
    RESULT_ZF_SF,
    live_after,
    uses_and_defines,
)

from analysis import Address, Function, Instruction, Operand, Program
from hashing import fnv1a64

REG16 = {
    "ax": "c->ax.x",
    "bx": "c->bx.x",
    "cx": "c->cx.x",
    "dx": "c->dx.x",
    "si": "c->si",
    "di": "c->di",
    "bp": "c->bp",
    "sp": "c->sp",
    "cs": "c->cs",
    "ds": "c->ds",
    "es": "c->es",
    "ss": "c->ss",
}
REG8 = {
    "al": "c->ax.b.l",
    "ah": "c->ax.b.h",
    "bl": "c->bx.b.l",
    "bh": "c->bx.b.h",
    "cl": "c->cx.b.l",
    "ch": "c->cx.b.h",
    "dl": "c->dx.b.l",
    "dh": "c->dx.b.h",
}
CONDITIONS = {
    "jo": "FLAG_OF(c)",
    "jno": "!FLAG_OF(c)",
    "jb": "FLAG_CF(c)",
    "jae": "!FLAG_CF(c)",
    "je": "FLAG_ZF(c)",
    "jne": "!FLAG_ZF(c)",
    "jbe": "(FLAG_CF(c) || FLAG_ZF(c))",
    "ja": "!(FLAG_CF(c) || FLAG_ZF(c))",
    "js": "FLAG_SF(c)",
    "jns": "!FLAG_SF(c)",
    "jp": "FLAG_PF(c)",
    "jnp": "!FLAG_PF(c)",
    "jl": "(FLAG_SF(c) != FLAG_OF(c))",
    "jge": "(FLAG_SF(c) == FLAG_OF(c))",
    "jle": "(FLAG_ZF(c) || FLAG_SF(c) != FLAG_OF(c))",
    "jg": "(!FLAG_ZF(c) && FLAG_SF(c) == FLAG_OF(c))",
}
ALU = {"add": "add", "adc": "adc", "sub": "sub", "sbb": "sbb", "and": "and", "or": "or", "xor": "xor"}
SHIFTS = {
    "shl": "shl",
    "sal": "shl",
    "shr": "shr",
    "sar": "sar",
    "rol": "rol",
    "ror": "ror",
    "rcl": "rcl",
    "rcr": "rcr",
}
FLAG_OPS = {
    "cli": "c->flags &= ~F_IF;",
    "sti": "c->flags |= F_IF;",
    "cld": "c->flags &= ~F_DF;",
    "std": "c->flags |= F_DF;",
    "clc": "c->flags &= ~F_CF;",
    "stc": "c->flags |= F_CF;",
    "cmc": "c->flags ^= F_CF;",
}


SIGNATURE_SIZE = 32
PRINT_SIZE = 8
WRITING_MNEMONICS = {"mov", "add", "adc", "sub", "sbb", "and", "or", "xor", "inc", "dec", "neg", "not", "pop", "xchg"}


class LiftError(Exception):
    pass


def self_written_bytes(program: Program) -> set[int]:
    written = set()
    for function in program.functions.values():
        for instruction in function.instructions.values():
            if instruction.mnemonic not in WRITING_MNEMONICS or not instruction.operands:
                continue
            target = instruction.operands[0]
            if target.kind == "mem" and target.segment == "cs" and target.base is None and target.index is None:
                start = target.disp & 0xFFFF
                written.update(range(start, start + max(target.size, 1)))
    return written


def label(address: int) -> str:
    return f"L_{address:04x}"


@dataclass
class Naming:
    prefix: str
    functions: dict[Address, str] = field(default_factory=dict)
    notes: dict[Address, list[str]] = field(default_factory=dict)
    data: dict[Address, tuple[str, str]] = field(default_factory=dict)

    def function(self, key: Address) -> str:
        if key in self.functions:
            return self.functions[key]
        segment, address = key
        if segment == 0:
            return f"{self.prefix}_{address:04x}"
        return f"{self.prefix}_{segment:04x}_{address:04x}"


# Data names of the module being lifted, by segment register (offset -> C name), set by lift_program.
DATA_NAMES: dict[str, dict[int, str]] = {"cs": {}, "ds": {}}


def effective_offset(operand: Operand) -> str:
    parts = []
    if operand.base:
        parts.append(REG16[operand.base])
    if operand.index:
        parts.append(REG16[operand.index])
    displacement = operand.disp & 0xFFFF
    segment = operand.segment or ("ss" if operand.base == "bp" else "ds")
    names = DATA_NAMES.get(segment, {})
    if displacement in names:
        parts.append(names[displacement])
    elif operand.disp or not parts:
        parts.append(f"0x{displacement:04x}")
    # Only a sum needs the cast (it wraps at 64 KB); rd/wr take a uint16_t anyway.
    return parts[0] if len(parts) == 1 else f"(uint16_t)({' + '.join(parts)})"


# The enum constants have the values of the numbers they replace, so the compiled code does not change.
# Names in segment 0000 are in the module's own image: CS data, and in a COM program or overlay (DS = CS) DS data too.
# Names in another segment are the data segment of an EXE (DS data).
def data_name_lines(naming: Naming, *, reserved: set[str], exe: bool) -> list[str]:
    for names in DATA_NAMES.values():
        names.clear()
    used = set(naming.functions.values()) | reserved
    lines = []
    for (segment, offset), (name, text) in sorted(naming.data.items(), key=lambda item: (item[0][0], item[0][1])):
        identifier = f"{name}_data" if name in used else name
        used.add(identifier)
        if segment == 0:
            DATA_NAMES["cs"][offset] = identifier
            if not exe:
                DATA_NAMES["ds"][offset] = identifier
        elif exe:
            DATA_NAMES["ds"][offset] = identifier
        where = "code segment" if segment == 0 and exe else "data"
        comment = f" /* {where}: {text.replace('*/', '* /')} */" if text else f" /* {where} */"
        lines.append(f"    {identifier} = 0x{offset:04x},{comment}")
    if not lines:
        return []
    return ["/* Data of this module (offsets, from the names files). */", "enum {", *lines, "};", ""]


def segment_of(operand: Operand) -> str:
    if operand.segment:
        return REG16[operand.segment]
    if operand.base == "bp":
        return "c->ss"
    return "c->ds"


def read(operand: Operand) -> str:
    if operand.kind == "reg":
        return REG8.get(operand.reg) or REG16[operand.reg]
    if operand.kind == "imm":
        mask = 0xFF if operand.size == 1 else 0xFFFF
        if operand.relocated:
            return f"(uint16_t)(0x{operand.imm & 0xFFFF:04x} + c->load_seg)"
        return f"0x{operand.imm & mask:0{operand.size * 2}x}"
    width = 8 if operand.size == 1 else 16
    return f"rd{width}(c, {segment_of(operand)}, {effective_offset(operand)})"


def write(operand: Operand, value: str) -> str:
    if operand.kind == "reg":
        return f"{REG8.get(operand.reg) or REG16[operand.reg]} = {value};"
    width = 8 if operand.size == 1 else 16
    return f"wr{width}(c, {segment_of(operand)}, {effective_offset(operand)}, {value});"


def width_of(instruction: Instruction) -> int:
    sizes = [operand.size for operand in instruction.operands if operand.kind != "imm"]
    return 8 if (sizes[0] if sizes else instruction.operands[0].size) == 1 else 16


def string_segment(instruction: Instruction) -> str:
    for operand in instruction.operands:
        if operand.kind == "mem" and operand.base == "si":
            return segment_of(operand)
    return "c->ds"


def lift_string(instruction: Instruction) -> list[str]:
    name = instruction.mnemonic
    width = 8 if name.endswith("b") else 16
    step = 1 if width == 8 else 2
    source = string_segment(instruction)
    kind = name[:-1]
    body = {
        "lods": [
            f"{'c->ax.b.l' if width == 8 else 'c->ax.x'} = rd{width}(c, {source}, c->si);",
            f"c->si += STEP({step});",
        ],
        "stos": [f"wr{width}(c, c->es, c->di, {'c->ax.b.l' if width == 8 else 'c->ax.x'});", f"c->di += STEP({step});"],
        "movs": [
            f"wr{width}(c, c->es, c->di, rd{width}(c, {source}, c->si));",
            f"c->si += STEP({step});",
            f"c->di += STEP({step});",
        ],
        "cmps": [
            f"alu_sub{width}(c, rd{width}(c, {source}, c->si), rd{width}(c, c->es, c->di));",
            f"c->si += STEP({step});",
            f"c->di += STEP({step});",
        ],
        "scas": [
            f"alu_sub{width}(c, {'c->ax.b.l' if width == 8 else 'c->ax.x'}, rd{width}(c, c->es, c->di));",
            f"c->di += STEP({step});",
        ],
    }[kind]
    if instruction.rep is None:
        return body
    stop = ""
    if kind in ("cmps", "scas"):
        stop = " if (!FLAG_ZF(c)) break;" if instruction.rep in ("rep", "repe") else " if (FLAG_ZF(c)) break;"
    return ["while (c->cx.x) {", "    c->icount++;", *("    " + line for line in body), f"    c->cx.x--;{stop}", "}"]


def lift_instruction(
    instruction: Instruction, function: Function, *, naming: Naming, entries: set[Address]
) -> list[str]:
    name = instruction.mnemonic
    ops = instruction.operands
    next_ip = f"0x{instruction.next_address & 0xFFFF:04x}"
    if name == "mov":
        return [write(ops[0], read(ops[1]))]
    if name in ALU:
        width = width_of(instruction)
        return [write(ops[0], f"alu_{ALU[name]}{width}(c, {read(ops[0])}, {read(ops[1])})")]
    if name == "cmp":
        return [f"alu_sub{width_of(instruction)}(c, {read(ops[0])}, {read(ops[1])});"]
    if name == "test":
        return [f"alu_and{width_of(instruction)}(c, {read(ops[0])}, {read(ops[1])});"]
    if name in ("inc", "dec", "neg"):
        return [write(ops[0], f"alu_{name}{width_of(instruction)}(c, {read(ops[0])})")]
    if name == "not":
        mask = "0xff" if width_of(instruction) == 8 else "0xffff"
        return [write(ops[0], f"(uint{width_of(instruction)}_t)({read(ops[0])} ^ {mask})")]
    if name in SHIFTS:
        count = read(ops[1]) if len(ops) > 1 else "1"
        width = width_of(instruction)
        return [write(ops[0], f"alu_{SHIFTS[name]}{width}(c, {read(ops[0])}, {count})")]
    if name == "xchg":
        return [
            "{",
            f"    uint{width_of(instruction)}_t t = {read(ops[0])};",
            "    " + write(ops[0], read(ops[1])),
            "    " + write(ops[1], "t"),
            "}",
        ]
    if name == "lea":
        return [write(ops[0], effective_offset(ops[1]))]
    if name in ("lds", "les"):
        segment = "c->ds" if name == "lds" else "c->es"
        base = ops[1]
        return [
            "{",
            f"    uint16_t o = {effective_offset(base)};",
            "    " + write(ops[0], f"rd16(c, {segment_of(base)}, o)"),
            f"    {segment} = rd16(c, {segment_of(base)}, (uint16_t)(o + 2));",
            "}",
        ]
    if name == "push":
        return [f"push16(c, {read(ops[0])});"]
    if name == "pop":
        return [write(ops[0], "pop16(c)")]
    if name in ("pusha", "popa", "pushf", "popf", "cbw", "cwd", "lahf", "sahf", "xlatb"):
        return [f"op_{name}(c);"]
    if name in FLAG_OPS:
        return [FLAG_OPS[name]]
    if name == "leave":
        return ["c->sp = c->bp;", "c->bp = pop16(c);"]
    if name == "enter":
        return [f"op_enter(c, {read(ops[0])}, {read(ops[1])});"]
    if name == "imul" and len(ops) in (2, 3):
        source = read(ops[1]) if len(ops) == 3 else read(ops[0])
        factor = read(ops[2]) if len(ops) == 3 else read(ops[1])
        return [write(ops[0], f"op_imul_trunc16(c, {source}, {factor})")]
    if name in ("mul", "imul", "div", "idiv") and len(ops) == 1:
        return [f"op_{name}{width_of(instruction)}(c, {read(ops[0])});"]
    if name in ("lodsb", "lodsw", "stosb", "stosw", "movsb", "movsw", "cmpsb", "cmpsw", "scasb", "scasw"):
        return lift_string(instruction)
    if name == "in":
        port = read(ops[1]) if ops[1].kind == "imm" else "c->dx.x"
        width = 8 if ops[0].reg == "al" else 16
        return [f"{read(ops[0])} = rt_in{width}(c, {port});"]
    if name == "out":
        port = read(ops[0]) if ops[0].kind == "imm" else "c->dx.x"
        width = 8 if ops[1].reg == "al" else 16
        return [f"rt_out{width}(c, {port}, {read(ops[1])});"]
    if name == "int":
        return [f"rt_int(c, 0x{ops[0].imm & 0xFF:02x});"]
    if name == "hlt":
        return ["rt_halt(c);"]
    if name == "nop":
        return []
    if name in CONDITIONS:
        return [f"if ({CONDITIONS[name]}) goto {label(instruction.direct_target())};"]
    if name == "jcxz":
        return [f"if (c->cx.x == 0) goto {label(instruction.direct_target())};"]
    if name in ("loop", "loope", "loopne"):
        extra = {"loop": "", "loope": " && FLAG_ZF(c)", "loopne": " && !FLAG_ZF(c)"}[name]
        return [f"if (--c->cx.x != 0{extra}) goto {label(instruction.direct_target())};"]
    if name == "jmp":
        target = instruction.direct_target()
        if target is None:
            return [f"rt_jump_near(c, {read(ops[0])});", "return;"]
        if (function.segment, target) in function.tail_calls:
            return [f"{naming.function((function.segment, target))}(c);", "return;"]
        return [f"goto {label(target)};"]
    if name in ("lcall", "ljmp"):
        if len(ops) == 2:
            segment, offset = read(ops[0]), read(ops[1])
        else:
            segment = f"rd16(c, {segment_of(ops[0])}, (uint16_t)({effective_offset(ops[0])} + 2))"
            offset = f"rd16(c, {segment_of(ops[0])}, {effective_offset(ops[0])})"
        head = ["{", f"    uint16_t far_seg = {segment};", f"    uint16_t far_off = {offset};"]
        if name == "ljmp":
            return head + ["    rt_jump_far(c, far_seg, far_off);", "    return;", "}"]
        return head + [
            "    uint16_t caller_cs = c->cs;",
            "    push16(c, c->cs);",
            f"    push16(c, {next_ip});",
            "    rt_call_far(c, far_seg, far_off);",
            f"    rt_check_far_return(c, caller_cs, {next_ip});",
            "    c->cs = caller_cs;",
            "}",
        ]
    if name == "call":
        target = instruction.direct_target()
        lines = [f"push16(c, {next_ip});"]
        if target is None:
            lines.append(f"rt_call_near(c, {read(ops[0])});")
        else:
            lines.append(f"{naming.function((function.segment, target))}(c);")
        lines.append(f"if (rt_check_return(c, {next_ip})) return;")
        return lines
    if name == "ret":
        lines = ["c->ret_ip = pop16(c);"]
        if ops:
            lines.append(f"c->sp += {read(ops[0])};")
        return lines + ["return;"]
    if name == "retf":
        lines = ["c->ret_ip = pop16(c);", "c->ret_cs = pop16(c);"]
        if ops:
            lines.append(f"c->sp += {read(ops[0])};")
        return lines + ["return;"]
    if name == "iret":
        return ["c->ret_ip = pop16(c);", "c->ret_cs = pop16(c);", "c->flags = pop16(c);", "return;"]
    raise LiftError(
        f"{instruction.segment:04x}:{instruction.address:04x}: unsupported instruction '{instruction.text}'"
    )


def signed(value: str, width: int) -> str:
    return "0" if value in ("0x00", "0x0000") else f"(int{width}_t){value}"


def same_register(instruction: Instruction) -> bool:
    first, second = instruction.operands
    return first.kind == "reg" and second.kind == "reg" and first.reg == second.reg


def flagless_lines(instruction: Instruction) -> list[str]:
    """The instruction without its flags (nothing reads them)."""
    name = instruction.mnemonic
    ops = instruction.operands
    width = width_of(instruction)
    if name in ("cmp", "test"):
        return [f"(void){read(operand)};" for operand in ops if operand.kind == "mem"]
    if name in LOGIC:
        if same_register(instruction):
            return [write(ops[0], "0")] if name == "xor" else []
        return [write(ops[0], f"{read(ops[0])} {LOGIC[name]} {read(ops[1])}")]
    values = {"a": read(ops[0]), "b": read(ops[1]) if len(ops) > 1 else ""}
    return [write(ops[0], f"(uint{width}_t)({ARITHMETIC[name].format(**values)})")]


def folded_jump(instruction: Instruction, jump: str) -> tuple[list[str], str] | None:
    """Lines for the instruction and the C condition of the conditional jump after it, or None."""
    name = instruction.mnemonic
    ops = instruction.operands
    width = width_of(instruction)
    if name == "cmp":
        a, b = read(ops[0]), read(ops[1])
        if jump in ("js", "jns"):
            return [], f"(int{width}_t)({a} - {b}) {'<' if jump == 'js' else '>='} 0"
        if jump not in COMPARE:
            return None
        return [], COMPARE[jump].format(a=a, b=b, sa=signed(a, width), sb=signed(b, width))
    if name == "test":
        if jump not in RESULT:
            return None
        a, b = read(ops[0]), read(ops[1])
        result = a if same_register(instruction) else f"({a} & {b})"
        return [], RESULT[jump].format(r=result, sr=signed(result, width))
    if ops[0].kind != "reg":
        return None
    if name in LOGIC and jump in RESULT or name in ("add", "sub", "inc", "dec") and jump in RESULT_ZF_SF:
        result = read(ops[0])
        return flagless_lines(instruction), RESULT[jump].format(r=result, sr=signed(result, width))
    return None


def polls(instruction: Instruction) -> bool:
    target = instruction.direct_target()
    jumps = instruction.mnemonic in CONDITIONS or instruction.mnemonic in ("jcxz", "loop", "loope", "loopne", "jmp")
    return target is not None and target <= instruction.address and jumps


def flag_plan(
    function: Function, *, needed: set[int], safe_points: frozenset[int]
) -> tuple[dict[int, list[str]], dict[int, str]]:
    """Replacement lines for instructions whose flags nothing reads, and C conditions for the jumps folded into them."""
    after = live_after(function)
    replaced: dict[int, list[str]] = {}
    conditions: dict[int, str] = {}
    for address, instruction in function.instructions.items():
        name = instruction.mnemonic
        if name not in ARITHMETIC and name not in LOGIC and name != "cmp":
            continue
        defines = uses_and_defines(instruction, function)[1]
        jump = function.instructions.get(instruction.next_address)
        foldable = (
            jump is not None
            and jump.mnemonic in CONDITION_USES
            and jump.address not in needed
            and jump.address not in safe_points
            and CONDITION_USES[jump.mnemonic] & ~defines == 0
            and after[jump.address] & defines == 0
        )
        folded = folded_jump(instruction, jump.mnemonic) if foldable else None
        if folded is not None:
            lines, condition = folded
            if polls(jump) and ("rd8(" in condition or "rd16(" in condition):
                lines, condition = [f"taken = {condition};"], "taken"
            replaced[address] = lines
            conditions[jump.address] = condition
        elif after[address] & defines == 0:
            replaced[address] = flagless_lines(instruction)
    return replaced, conditions


def falls_through(instruction: Instruction) -> bool:
    return instruction.mnemonic not in ("jmp", "ljmp", "ret", "retf", "iret")


def ends_block(instruction: Instruction) -> bool:
    name = instruction.mnemonic
    return name in CONDITIONS or name in ("jmp", "ljmp", "jcxz", "loop", "loope", "loopne", "ret", "retf", "iret")


def instruction_counts(body: list[tuple[str, int]], function: Function, *, needed: set[int]) -> dict[int, int]:
    counts = {}
    start = None
    for kind, address in body:
        if kind != "ins":
            start = None
            continue
        if start is None or address in needed:
            start = address
            counts[start] = 0
        counts[start] += 1
        if ends_block(function.instructions[address]):
            start = None
    return counts


def note_lines(naming: Naming, key: Address, *, indent: str) -> list[str]:
    return [f"{indent}/* {text.replace('*/', '* /')} */" for text in naming.notes.get(key, [])]


def lift_function(
    function: Function, *, naming: Naming, entries: set[Address], safe_points: frozenset[int] = frozenset()
) -> list[str]:
    lines = note_lines(naming, function.key, indent="")
    lines += [f"static void {naming.function(function.key)}(Cpu *c)", "{"]
    addresses = sorted(function.instructions)
    order = {address: position for position, address in enumerate(addresses)}
    needed = {function.entry} if addresses[0] != function.entry else set()
    for instruction in function.instructions.values():
        target = instruction.direct_target()
        jumps = instruction.mnemonic in CONDITIONS or instruction.mnemonic in ("jcxz", "loop", "loope", "loopne")
        tail = (function.segment, target) in function.tail_calls
        if target is not None and (jumps or (instruction.mnemonic == "jmp" and not tail)):
            needed.add(target)
    body = []
    for position, address in enumerate(addresses):
        instruction = function.instructions[address]
        body.append(("ins", address))
        following = addresses[position + 1] if position + 1 < len(addresses) else None
        if falls_through(instruction) and instruction.next_address != following:
            if instruction.next_address in order:
                needed.add(instruction.next_address)
                body.append(("goto", instruction.next_address))
            else:
                body.append(("end", instruction.next_address))
    replaced, conditions = flag_plan(function, needed=needed, safe_points=safe_points)
    if "taken" in conditions.values():
        lines.append("    int taken;")
    if addresses[0] != function.entry:
        lines.append(f"    goto {label(function.entry)};")
    block_sizes = instruction_counts(body, function, needed=needed)
    for kind, address in body:
        if kind == "goto":
            lines.append(f"    goto {label(address)};")
            continue
        if kind == "end":
            lines.append(f"    rt_fell_off(c, 0x{address:04x});")
            lines.append("    return;")
            continue
        instruction = function.instructions[address]
        if address in needed:
            lines.append(f"{label(address)}:")
        if address != function.entry:
            lines += note_lines(naming, (function.segment, address), indent="    ")
        lines.append(f"    /* {address:04x}: {instruction.text} */")
        if address in block_sizes:
            lines.append(f"    c->icount += {block_sizes[address]};")
        if address in safe_points:
            lines.append(f"    rt_safe_point(c, 0x{address:04x});")
        if polls(instruction):
            lines.append(f"    rt_poll_loop(c, 0x{address:04x});")
        if address in replaced:
            body_lines = replaced[address]
        elif address in conditions:
            body_lines = [f"if ({conditions[address]}) goto {label(instruction.direct_target())};"]
        else:
            body_lines = lift_instruction(instruction, function, naming=naming, entries=entries)
        for line in body_lines:
            lines.append(f"    {line}")
        if instruction.mnemonic in ("sti", "popf"):
            lines.append("    rt_poll_interrupts(c);")
    lines += ["}", ""]
    return lines


def code_print(program: Program, *, segment: int, address: int) -> tuple[int, int]:
    """Hash and length of the first code bytes of the function: only bytes of its own instructions."""
    function = program.functions[(segment, address)]
    length = 0
    current = address
    while length < PRINT_SIZE and current in function.instructions:
        instruction = function.instructions[current]
        length += instruction.next_address - current
        if instruction.mnemonic in ("ret", "retf", "iret", "jmp", "ljmp"):
            break
        current = instruction.next_address
    length = min(length, PRINT_SIZE)
    start = segment * 16 + address - program.base
    return fnv1a64(program.image[start : start + length]), length


def interpreted_function(function: Function, *, naming: Naming) -> list[str]:
    name = naming.function(function.key)
    return [f"static void {name}(Cpu *c)", "{", f"    rt_interpret(c, 0x{function.entry:04x});", "}", ""]


def lift_program(
    program: Program,
    *,
    naming: Naming,
    name: str,
    interpret: bool = False,
    safe_points: frozenset[int] = frozenset(),
    reserved: frozenset[str] = frozenset(),
    exe: bool = False,
) -> str:
    prefix = naming.prefix
    entries = set(program.functions)
    out = [f"/* Generated by tools/recomp/recomp.py from {name}. Do not edit. */", '#include "rt.h"', ""]
    out += data_name_lines(naming, reserved=set(reserved), exe=exe)
    for key in sorted(program.functions):
        out.append(f"static void {naming.function(key)}(Cpu *c);")
    out.append("")
    for key in sorted(program.functions):
        if interpret:
            out += interpreted_function(program.functions[key], naming=naming)
        else:
            out += lift_function(program.functions[key], naming=naming, entries=entries, safe_points=safe_points)
    out.append(f"const RtEntry {prefix}_entries[] = {{")
    for segment, address in sorted(program.functions):
        print_hash, length = code_print(program, segment=segment, address=address)
        func = naming.function((segment, address))
        out.append(f"    {{0x{segment:04x}, 0x{address:04x}, {func}, 0x{print_hash:016x}ull, {length}}},")
    out += ["    {0, 0, 0, 0, 0},", "};", ""]
    mutable = self_written_bytes(program)
    mask = sum(1 << index for index in range(SIGNATURE_SIZE) if program.base + index not in mutable)
    window = program.image[:SIGNATURE_SIZE].ljust(SIGNATURE_SIZE, b"\0")
    masked = bytes(value if mask >> index & 1 else 0 for index, value in enumerate(window))
    out.append(
        f'const RtModule {prefix}_module = {{"{name}", 0x{program.base:04x}, 0x{fnv1a64(masked):016x}ull, '
        f"0x{mask:08x}u, {prefix}_entries}};"
    )
    out.append("")
    return "\n".join(out)
