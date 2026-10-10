from analysis import Function, Instruction

CF = 0x0001
PF = 0x0004
AF = 0x0010
ZF = 0x0040
SF = 0x0080
OF = 0x0800
ALL = CF | PF | AF | ZF | SF | OF

CONDITION_USES = {
    "jo": OF,
    "jno": OF,
    "jb": CF,
    "jae": CF,
    "je": ZF,
    "jne": ZF,
    "jbe": CF | ZF,
    "ja": CF | ZF,
    "js": SF,
    "jns": SF,
    "jp": PF,
    "jnp": PF,
    "jl": SF | OF,
    "jge": SF | OF,
    "jle": ZF | SF | OF,
    "jg": ZF | SF | OF,
}
DEFINES_ALL = {"add", "sub", "cmp", "neg", "adc", "sbb", "and", "or", "xor", "test"}
NEUTRAL = {
    "mov",
    "lea",
    "lds",
    "les",
    "push",
    "pop",
    "xchg",
    "not",
    "cbw",
    "cwd",
    "xlatb",
    "in",
    "out",
    "nop",
    "cli",
    "sti",
    "cld",
    "std",
    "enter",
    "leave",
    "pusha",
    "popa",
    "jcxz",
    "loop",
    "lodsb",
    "lodsw",
    "stosb",
    "stosw",
    "movsb",
    "movsw",
}
# These compute their flags in the runtime and may leave them unchanged (count 0, rep with CX = 0).
SELF_CONTAINED = {
    "shl",
    "sal",
    "shr",
    "sar",
    "rol",
    "ror",
    "mul",
    "imul",
    "div",
    "idiv",
    "cmpsb",
    "cmpsw",
    "scasb",
    "scasw",
}
JUMPS = set(CONDITION_USES) | {"jcxz", "loop", "loope", "loopne", "jmp"}


def uses_and_defines(instruction: Instruction, function: Function) -> tuple[int, int]:
    name = instruction.mnemonic
    if name in DEFINES_ALL:
        return (CF if name in ("adc", "sbb") else 0), ALL
    if name in ("inc", "dec"):
        return 0, ALL & ~CF
    if name in CONDITION_USES:
        return CONDITION_USES[name], 0
    if name in ("loope", "loopne"):
        return ZF, 0
    if name in ("rcl", "rcr"):
        return CF, 0
    if name in ("clc", "stc"):
        return 0, CF
    if name == "cmc":
        return CF, CF
    if name == "popf":
        return 0, ALL
    if name == "sahf":
        return 0, CF | PF | AF | ZF | SF
    if name in SELF_CONTAINED or name in NEUTRAL:
        return 0, 0
    if name == "jmp":
        target = instruction.direct_target()
        local = target is not None and target in function.instructions
        return (0 if local and (function.segment, target) not in function.tail_calls else ALL), 0
    return ALL, 0


def successors(instruction: Instruction, function: Function) -> list[int | None]:
    """Following instructions inside the function; None stands for code outside it (all flags live there)."""
    name = instruction.mnemonic
    found: list[int | None] = []
    if name in JUMPS:
        target = instruction.direct_target()
        tail = name == "jmp" and (function.segment, target) in function.tail_calls
        if target is not None and not tail:
            found.append(target if target in function.instructions else None)
    if name not in ("jmp", "ljmp", "ret", "retf", "iret"):
        following = instruction.next_address
        found.append(following if following in function.instructions else None)
    return found


def live_after(function: Function) -> dict[int, int]:
    """Flags that a later instruction can read, after each instruction (returns and calls read all of them)."""
    effects = {address: uses_and_defines(ins, function) for address, ins in function.instructions.items()}
    following = {address: successors(ins, function) for address, ins in function.instructions.items()}
    live_in = {address: 0 for address in function.instructions}
    after = {address: 0 for address in function.instructions}
    changed = True
    while changed:
        changed = False
        for address in sorted(function.instructions, reverse=True):
            out = 0
            for target in following[address]:
                out |= ALL if target is None else live_in[target]
            uses, defines = effects[address]
            value = (out & ~defines) | uses
            if out != after[address] or value != live_in[address]:
                after[address] = out
                live_in[address] = value
                changed = True
    return after


ARITHMETIC = {
    "add": "{a} + {b}",
    "sub": "{a} - {b}",
    "adc": "{a} + {b} + FLAG_CF(c)",
    "sbb": "{a} - {b} - FLAG_CF(c)",
    "inc": "{a} + 1",
    "dec": "{a} - 1",
    "neg": "0 - {a}",
}
LOGIC = {"and": "&", "or": "|", "xor": "^", "test": "&"}
COMPARE = {
    "je": "{a} == {b}",
    "jne": "{a} != {b}",
    "jb": "{a} < {b}",
    "jae": "{a} >= {b}",
    "jbe": "{a} <= {b}",
    "ja": "{a} > {b}",
    "jl": "{sa} < {sb}",
    "jge": "{sa} >= {sb}",
    "jle": "{sa} <= {sb}",
    "jg": "{sa} > {sb}",
}
# Conditions on a result r: for and/or/xor/test (CF = OF = 0) all of them, for add/sub/inc/dec only ZF and SF.
RESULT = {
    "je": "{r} == 0",
    "jne": "{r} != 0",
    "js": "{sr} < 0",
    "jns": "{sr} >= 0",
    "jbe": "{r} == 0",
    "ja": "{r} != 0",
    "jl": "{sr} < 0",
    "jge": "{sr} >= 0",
    "jle": "{sr} <= 0",
    "jg": "{sr} > 0",
}
RESULT_ZF_SF = {"je", "jne", "js", "jns"}
