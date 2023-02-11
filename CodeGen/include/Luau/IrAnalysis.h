// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

namespace Luau
{
namespace CodeGen
{

struct IrFunction;

void updateUseCounts(IrFunction& function);

void updateLastUseLocations(IrFunction& function);

} // namespace CodeGen
} // namespace Luau
