// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/IrBuilder.h"

#include "Luau/Common.h"
#include "Luau/IrAnalysis.h"
#include "Luau/IrUtils.h"

#include "CustomExecUtils.h"
#include "IrTranslation.h"

#include "lapi.h"

namespace Luau
{
namespace CodeGen
{

constexpr unsigned kNoAssociatedBlockIndex = ~0u;

void IrBuilder::buildFunctionIr(Proto* proto)
{
    function.proto = proto;

    // Rebuild original control flow blocks
    rebuildBytecodeBasicBlocks(proto);

    function.bcMapping.resize(proto->sizecode, {~0u, 0});

    // Translate all instructions to IR inside blocks
    for (int i = 0; i < proto->sizecode;)
    {
        const Instruction* pc = &proto->code[i];
        LuauOpcode op = LuauOpcode(LUAU_INSN_OP(*pc));

        int nexti = i + getOpLength(op);
        LUAU_ASSERT(nexti <= proto->sizecode);

        function.bcMapping[i] = {uint32_t(function.instructions.size()), 0};

        // Begin new block at this instruction if it was in the bytecode or requested during translation
        if (instIndexToBlock[i] != kNoAssociatedBlockIndex)
            beginBlock(blockAtInst(i));

        // We skip dead bytecode instructions when they appear after block was already terminated
        if (!inTerminatedBlock)
            translateInst(op, pc, i);

        i = nexti;
        LUAU_ASSERT(i <= proto->sizecode);

        // If we are going into a new block at the next instruction and it's a fallthrough, jump has to be placed to mark block termination
        if (i < int(instIndexToBlock.size()) && instIndexToBlock[i] != kNoAssociatedBlockIndex)
        {
            if (!isBlockTerminator(function.instructions.back().cmd))
                inst(IrCmd::JUMP, blockAtInst(i));
        }
    }

    // Now that all has been generated, compute use counts
    updateUseCounts(function);
}

void IrBuilder::rebuildBytecodeBasicBlocks(Proto* proto)
{
    instIndexToBlock.resize(proto->sizecode, kNoAssociatedBlockIndex);

    // Mark jump targets
    std::vector<uint8_t> jumpTargets(proto->sizecode, 0);

    for (int i = 0; i < proto->sizecode;)
    {
        const Instruction* pc = &proto->code[i];
        LuauOpcode op = LuauOpcode(LUAU_INSN_OP(*pc));

        int target = getJumpTarget(*pc, uint32_t(i));

        if (target >= 0 && !isFastCall(op))
            jumpTargets[target] = true;

        i += getOpLength(op);
        LUAU_ASSERT(i <= proto->sizecode);
    }


    // Bytecode blocks are created at bytecode jump targets and the start of a function
    jumpTargets[0] = true;

    for (int i = 0; i < proto->sizecode; i++)
    {
        if (jumpTargets[i])
        {
            IrOp b = block(IrBlockKind::Bytecode);
            instIndexToBlock[i] = b.index;
        }
    }
}

void IrBuilder::translateInst(LuauOpcode op, const Instruction* pc, int i)
{
    switch (op)
    {
    case LOP_NOP:
        break;
    case LOP_LOADNIL:
        translateInstLoadNil(*this, pc);
        break;
    case LOP_LOADB:
        translateInstLoadB(*this, pc, i);
        break;
    case LOP_LOADN:
        translateInstLoadN(*this, pc);
        break;
    case LOP_LOADK:
        translateInstLoadK(*this, pc);
        break;
    case LOP_LOADKX:
        translateInstLoadKX(*this, pc);
        break;
    case LOP_MOVE:
        translateInstMove(*this, pc);
        break;
    case LOP_GETGLOBAL:
        translateInstGetGlobal(*this, pc, i);
        break;
    case LOP_SETGLOBAL:
        translateInstSetGlobal(*this, pc, i);
        break;
    case LOP_CALL:
        inst(IrCmd::LOP_CALL, constUint(i), vmReg(LUAU_INSN_A(*pc)), constInt(LUAU_INSN_B(*pc) - 1), constInt(LUAU_INSN_C(*pc) - 1));

        if (activeFastcallFallback)
        {
            inst(IrCmd::JUMP, fastcallFallbackReturn);

            beginBlock(fastcallFallbackReturn);

            activeFastcallFallback = false;
        }
        break;
    case LOP_RETURN:
        inst(IrCmd::LOP_RETURN, constUint(i), vmReg(LUAU_INSN_A(*pc)), constInt(LUAU_INSN_B(*pc) - 1));
        break;
    case LOP_GETTABLE:
        translateInstGetTable(*this, pc, i);
        break;
    case LOP_SETTABLE:
        translateInstSetTable(*this, pc, i);
        break;
    case LOP_GETTABLEKS:
        translateInstGetTableKS(*this, pc, i);
        break;
    case LOP_SETTABLEKS:
        translateInstSetTableKS(*this, pc, i);
        break;
    case LOP_GETTABLEN:
        translateInstGetTableN(*this, pc, i);
        break;
    case LOP_SETTABLEN:
        translateInstSetTableN(*this, pc, i);
        break;
    case LOP_JUMP:
        translateInstJump(*this, pc, i);
        break;
    case LOP_JUMPBACK:
        translateInstJumpBack(*this, pc, i);
        break;
    case LOP_JUMPIF:
        translateInstJumpIf(*this, pc, i, /* not_ */ false);
        break;
    case LOP_JUMPIFNOT:
        translateInstJumpIf(*this, pc, i, /* not_ */ true);
        break;
    case LOP_JUMPIFEQ:
        translateInstJumpIfEq(*this, pc, i, /* not_ */ false);
        break;
    case LOP_JUMPIFLE:
        translateInstJumpIfCond(*this, pc, i, IrCondition::LessEqual);
        break;
    case LOP_JUMPIFLT:
        translateInstJumpIfCond(*this, pc, i, IrCondition::Less);
        break;
    case LOP_JUMPIFNOTEQ:
        translateInstJumpIfEq(*this, pc, i, /* not_ */ true);
        break;
    case LOP_JUMPIFNOTLE:
        translateInstJumpIfCond(*this, pc, i, IrCondition::NotLessEqual);
        break;
    case LOP_JUMPIFNOTLT:
        translateInstJumpIfCond(*this, pc, i, IrCondition::NotLess);
        break;
    case LOP_JUMPX:
        translateInstJumpX(*this, pc, i);
        break;
    case LOP_JUMPXEQKNIL:
        translateInstJumpxEqNil(*this, pc, i);
        break;
    case LOP_JUMPXEQKB:
        translateInstJumpxEqB(*this, pc, i);
        break;
    case LOP_JUMPXEQKN:
        translateInstJumpxEqN(*this, pc, i);
        break;
    case LOP_JUMPXEQKS:
        translateInstJumpxEqS(*this, pc, i);
        break;
    case LOP_ADD:
        translateInstBinary(*this, pc, i, TM_ADD);
        break;
    case LOP_SUB:
        translateInstBinary(*this, pc, i, TM_SUB);
        break;
    case LOP_MUL:
        translateInstBinary(*this, pc, i, TM_MUL);
        break;
    case LOP_DIV:
        translateInstBinary(*this, pc, i, TM_DIV);
        break;
    case LOP_MOD:
        translateInstBinary(*this, pc, i, TM_MOD);
        break;
    case LOP_POW:
        translateInstBinary(*this, pc, i, TM_POW);
        break;
    case LOP_ADDK:
        translateInstBinaryK(*this, pc, i, TM_ADD);
        break;
    case LOP_SUBK:
        translateInstBinaryK(*this, pc, i, TM_SUB);
        break;
    case LOP_MULK:
        translateInstBinaryK(*this, pc, i, TM_MUL);
        break;
    case LOP_DIVK:
        translateInstBinaryK(*this, pc, i, TM_DIV);
        break;
    case LOP_MODK:
        translateInstBinaryK(*this, pc, i, TM_MOD);
        break;
    case LOP_POWK:
        translateInstBinaryK(*this, pc, i, TM_POW);
        break;
    case LOP_NOT:
        translateInstNot(*this, pc);
        break;
    case LOP_MINUS:
        translateInstMinus(*this, pc, i);
        break;
    case LOP_LENGTH:
        translateInstLength(*this, pc, i);
        break;
    case LOP_NEWTABLE:
        translateInstNewTable(*this, pc, i);
        break;
    case LOP_DUPTABLE:
        translateInstDupTable(*this, pc, i);
        break;
    case LOP_SETLIST:
        inst(IrCmd::LOP_SETLIST, constUint(i), vmReg(LUAU_INSN_A(*pc)), vmReg(LUAU_INSN_A(*pc)), constInt(LUAU_INSN_C(*pc) - 1), constUint(pc[1]));
        break;
    case LOP_GETUPVAL:
        translateInstGetUpval(*this, pc, i);
        break;
    case LOP_SETUPVAL:
        translateInstSetUpval(*this, pc, i);
        break;
    case LOP_CLOSEUPVALS:
        translateInstCloseUpvals(*this, pc);
        break;
    case LOP_FASTCALL:
    {
        int skip = LUAU_INSN_C(*pc);

        IrOp fallback = block(IrBlockKind::Fallback);
        IrOp next = blockAtInst(i + skip + 2);

        Instruction call = pc[skip + 1];
        LUAU_ASSERT(LUAU_INSN_OP(call) == LOP_CALL);

        inst(IrCmd::LOP_FASTCALL, constUint(i), vmReg(LUAU_INSN_A(call)), constInt(LUAU_INSN_B(call) - 1), fallback);
        inst(IrCmd::JUMP, next);

        beginBlock(fallback);

        activeFastcallFallback = true;
        fastcallFallbackReturn = next;
        break;
    }
    case LOP_FASTCALL1:
    {
        int skip = LUAU_INSN_C(*pc);

        IrOp fallback = block(IrBlockKind::Fallback);
        IrOp next = blockAtInst(i + skip + 2);

        Instruction call = pc[skip + 1];
        LUAU_ASSERT(LUAU_INSN_OP(call) == LOP_CALL);

        inst(IrCmd::LOP_FASTCALL1, constUint(i), vmReg(LUAU_INSN_A(call)), vmReg(LUAU_INSN_B(*pc)), fallback);
        inst(IrCmd::JUMP, next);

        beginBlock(fallback);

        activeFastcallFallback = true;
        fastcallFallbackReturn = next;
        break;
    }
    case LOP_FASTCALL2:
    {
        int skip = LUAU_INSN_C(*pc);

        IrOp fallback = block(IrBlockKind::Fallback);
        IrOp next = blockAtInst(i + skip + 2);

        Instruction call = pc[skip + 1];
        LUAU_ASSERT(LUAU_INSN_OP(call) == LOP_CALL);

        inst(IrCmd::LOP_FASTCALL2, constUint(i), vmReg(LUAU_INSN_A(call)), vmReg(LUAU_INSN_B(*pc)), vmReg(pc[1]), fallback);
        inst(IrCmd::JUMP, next);

        beginBlock(fallback);

        activeFastcallFallback = true;
        fastcallFallbackReturn = next;
        break;
    }
    case LOP_FASTCALL2K:
    {
        int skip = LUAU_INSN_C(*pc);

        IrOp fallback = block(IrBlockKind::Fallback);
        IrOp next = blockAtInst(i + skip + 2);

        Instruction call = pc[skip + 1];
        LUAU_ASSERT(LUAU_INSN_OP(call) == LOP_CALL);

        inst(IrCmd::LOP_FASTCALL2K, constUint(i), vmReg(LUAU_INSN_A(call)), vmReg(LUAU_INSN_B(*pc)), vmConst(pc[1]), fallback);
        inst(IrCmd::JUMP, next);

        beginBlock(fallback);

        activeFastcallFallback = true;
        fastcallFallbackReturn = next;
        break;
    }
    case LOP_FORNPREP:
        translateInstForNPrep(*this, pc, i);
        break;
    case LOP_FORNLOOP:
        translateInstForNLoop(*this, pc, i);
        break;
    case LOP_FORGLOOP:
    {
        // We have a translation for ipairs-style traversal, general loop iteration is still too complex
        if (int(pc[1]) < 0)
        {
            translateInstForGLoopIpairs(*this, pc, i);
        }
        else
        {
            IrOp loopRepeat = blockAtInst(i + 1 + LUAU_INSN_D(*pc));
            IrOp loopExit = blockAtInst(i + getOpLength(LOP_FORGLOOP));
            IrOp fallback = block(IrBlockKind::Fallback);

            inst(IrCmd::LOP_FORGLOOP, constUint(i), loopRepeat, loopExit, fallback);

            beginBlock(fallback);
            inst(IrCmd::LOP_FORGLOOP_FALLBACK, constUint(i), loopRepeat, loopExit);

            beginBlock(loopExit);
        }
        break;
    }
    case LOP_FORGPREP_NEXT:
        translateInstForGPrepNext(*this, pc, i);
        break;
    case LOP_FORGPREP_INEXT:
        translateInstForGPrepInext(*this, pc, i);
        break;
    case LOP_AND:
        inst(IrCmd::LOP_AND, constUint(i), vmReg(LUAU_INSN_A(*pc)), vmReg(LUAU_INSN_B(*pc)), vmReg(LUAU_INSN_C(*pc)));
        break;
    case LOP_ANDK:
        inst(IrCmd::LOP_ANDK, constUint(i), vmReg(LUAU_INSN_A(*pc)), vmReg(LUAU_INSN_B(*pc)), vmConst(LUAU_INSN_C(*pc)));
        break;
    case LOP_OR:
        inst(IrCmd::LOP_OR, constUint(i), vmReg(LUAU_INSN_A(*pc)), vmReg(LUAU_INSN_B(*pc)), vmReg(LUAU_INSN_C(*pc)));
        break;
    case LOP_ORK:
        inst(IrCmd::LOP_ORK, constUint(i), vmReg(LUAU_INSN_A(*pc)), vmReg(LUAU_INSN_B(*pc)), vmConst(LUAU_INSN_C(*pc)));
        break;
    case LOP_COVERAGE:
        inst(IrCmd::LOP_COVERAGE, constUint(i));
        break;
    case LOP_GETIMPORT:
        translateInstGetImport(*this, pc, i);
        break;
    case LOP_CONCAT:
        translateInstConcat(*this, pc, i);
        break;
    case LOP_CAPTURE:
        translateInstCapture(*this, pc, i);
        break;
    case LOP_NAMECALL:
    {
        IrOp next = blockAtInst(i + getOpLength(LOP_NAMECALL));
        IrOp fallback = block(IrBlockKind::Fallback);

        inst(IrCmd::LOP_NAMECALL, constUint(i), vmReg(LUAU_INSN_A(*pc)), vmReg(LUAU_INSN_B(*pc)), next, fallback);

        beginBlock(fallback);
        inst(IrCmd::FALLBACK_NAMECALL, constUint(i), vmReg(LUAU_INSN_A(*pc)), vmReg(LUAU_INSN_B(*pc)), vmConst(pc[1]));
        inst(IrCmd::JUMP, next);

        beginBlock(next);
        break;
    }
    case LOP_PREPVARARGS:
        inst(IrCmd::FALLBACK_PREPVARARGS, constUint(i), constInt(LUAU_INSN_A(*pc)));
        break;
    case LOP_GETVARARGS:
        inst(IrCmd::FALLBACK_GETVARARGS, constUint(i), vmReg(LUAU_INSN_A(*pc)), constInt(LUAU_INSN_B(*pc) - 1));
        break;
    case LOP_NEWCLOSURE:
        inst(IrCmd::FALLBACK_NEWCLOSURE, constUint(i), vmReg(LUAU_INSN_A(*pc)), constUint(LUAU_INSN_D(*pc)));
        break;
    case LOP_DUPCLOSURE:
        inst(IrCmd::FALLBACK_DUPCLOSURE, constUint(i), vmReg(LUAU_INSN_A(*pc)), vmConst(LUAU_INSN_D(*pc)));
        break;
    case LOP_FORGPREP:
    {
        IrOp loopStart = blockAtInst(i + 1 + LUAU_INSN_D(*pc));

        inst(IrCmd::FALLBACK_FORGPREP, constUint(i), vmReg(LUAU_INSN_A(*pc)), loopStart);
        break;
    }
    default:
        LUAU_ASSERT(!"unknown instruction");
        break;
    }
}

bool IrBuilder::isInternalBlock(IrOp block)
{
    IrBlock& target = function.blocks[block.index];

    return target.kind == IrBlockKind::Internal;
}

void IrBuilder::beginBlock(IrOp block)
{
    IrBlock& target = function.blocks[block.index];

    LUAU_ASSERT(target.start == ~0u || target.start == uint32_t(function.instructions.size()));

    target.start = uint32_t(function.instructions.size());

    inTerminatedBlock = false;
}

IrOp IrBuilder::constBool(bool value)
{
    IrConst constant;
    constant.kind = IrConstKind::Bool;
    constant.valueBool = value;
    return constAny(constant);
}

IrOp IrBuilder::constInt(int value)
{
    IrConst constant;
    constant.kind = IrConstKind::Int;
    constant.valueInt = value;
    return constAny(constant);
}

IrOp IrBuilder::constUint(unsigned value)
{
    IrConst constant;
    constant.kind = IrConstKind::Uint;
    constant.valueUint = value;
    return constAny(constant);
}

IrOp IrBuilder::constDouble(double value)
{
    IrConst constant;
    constant.kind = IrConstKind::Double;
    constant.valueDouble = value;
    return constAny(constant);
}

IrOp IrBuilder::constTag(uint8_t value)
{
    IrConst constant;
    constant.kind = IrConstKind::Tag;
    constant.valueTag = value;
    return constAny(constant);
}

IrOp IrBuilder::constAny(IrConst constant)
{
    uint32_t index = uint32_t(function.constants.size());
    function.constants.push_back(constant);
    return {IrOpKind::Constant, index};
}

IrOp IrBuilder::cond(IrCondition cond)
{
    return {IrOpKind::Condition, uint32_t(cond)};
}

IrOp IrBuilder::inst(IrCmd cmd)
{
    return inst(cmd, {}, {}, {}, {}, {});
}

IrOp IrBuilder::inst(IrCmd cmd, IrOp a)
{
    return inst(cmd, a, {}, {}, {}, {});
}

IrOp IrBuilder::inst(IrCmd cmd, IrOp a, IrOp b)
{
    return inst(cmd, a, b, {}, {}, {});
}

IrOp IrBuilder::inst(IrCmd cmd, IrOp a, IrOp b, IrOp c)
{
    return inst(cmd, a, b, c, {}, {});
}

IrOp IrBuilder::inst(IrCmd cmd, IrOp a, IrOp b, IrOp c, IrOp d)
{
    return inst(cmd, a, b, c, d, {});
}

IrOp IrBuilder::inst(IrCmd cmd, IrOp a, IrOp b, IrOp c, IrOp d, IrOp e)
{
    uint32_t index = uint32_t(function.instructions.size());
    function.instructions.push_back({cmd, a, b, c, d, e});

    if (isBlockTerminator(cmd))
        inTerminatedBlock = true;

    return {IrOpKind::Inst, index};
}

IrOp IrBuilder::block(IrBlockKind kind)
{
    if (kind == IrBlockKind::Internal && activeFastcallFallback)
        kind = IrBlockKind::Fallback;

    uint32_t index = uint32_t(function.blocks.size());
    function.blocks.push_back(IrBlock{kind});
    return IrOp{IrOpKind::Block, index};
}

IrOp IrBuilder::blockAtInst(uint32_t index)
{
    uint32_t blockIndex = instIndexToBlock[index];

    if (blockIndex != kNoAssociatedBlockIndex)
        return IrOp{IrOpKind::Block, blockIndex};

    return block(IrBlockKind::Internal);
}

IrOp IrBuilder::vmReg(uint8_t index)
{
    return {IrOpKind::VmReg, index};
}

IrOp IrBuilder::vmConst(uint32_t index)
{
    return {IrOpKind::VmConst, index};
}

IrOp IrBuilder::vmUpvalue(uint8_t index)
{
    return {IrOpKind::VmUpvalue, index};
}

} // namespace CodeGen
} // namespace Luau
