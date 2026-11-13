"""Decode complete Thumb hook prefixes, including bounded IT blocks."""
import re
from capstone import CS_GRP_CALL, CS_GRP_JUMP
from capstone.arm import ARM_REG_PC


def hook_prefix(disassembler, code, address, minimum=4):
    instructions = []
    predicated = set()
    size = remaining = 0
    for instruction in disassembler.disasm(code, address):
        if instruction.address != address + size:
            raise ValueError('Noncontiguous hook prefix')
        mnemonic = instruction.mnemonic
        is_it = re.fullmatch(r'it[te]{0,3}', mnemonic) is not None
        if is_it:
            if remaining:
                raise ValueError('Nested IT in hook prefix')
            remaining = len(mnemonic) - 1
        elif remaining:
            # These instructions must remain one-for-one copies: expanding
            # even a literal load would change which operations IT predicates.
            reads, writes = instruction.regs_access()
            if (instruction.group(CS_GRP_JUMP) or instruction.group(CS_GRP_CALL)
                    or ARM_REG_PC in reads or ARM_REG_PC in writes):
                raise ValueError('Unsupported flow or PC access inside hook IT block')
            predicated.add(instruction.address)
            remaining -= 1
        instructions.append(instruction)
        size += instruction.size
        if size >= minimum and not remaining:
            return instructions, predicated
    raise ValueError('Truncated hook prefix or incomplete IT block')
