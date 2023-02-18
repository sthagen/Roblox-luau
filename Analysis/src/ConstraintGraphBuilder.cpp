// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/ConstraintGraphBuilder.h"

#include "Luau/Ast.h"
#include "Luau/Common.h"
#include "Luau/Constraint.h"
#include "Luau/DcrLogger.h"
#include "Luau/ModuleResolver.h"
#include "Luau/RecursionCounter.h"
#include "Luau/Scope.h"
#include "Luau/TypeUtils.h"
#include "Luau/Type.h"

#include <algorithm>

LUAU_FASTINT(LuauCheckRecursionLimit);
LUAU_FASTFLAG(DebugLuauLogSolverToJson);
LUAU_FASTFLAG(DebugLuauMagicTypes);
LUAU_FASTFLAG(LuauNegatedClassTypes);
LUAU_FASTFLAG(SupportTypeAliasGoToDeclaration);

namespace Luau
{

const AstStat* getFallthrough(const AstStat* node); // TypeInfer.cpp

static std::optional<AstExpr*> matchRequire(const AstExprCall& call)
{
    const char* require = "require";

    if (call.args.size != 1)
        return std::nullopt;

    const AstExprGlobal* funcAsGlobal = call.func->as<AstExprGlobal>();
    if (!funcAsGlobal || funcAsGlobal->name != require)
        return std::nullopt;

    if (call.args.size != 1)
        return std::nullopt;

    return call.args.data[0];
}

static bool matchSetmetatable(const AstExprCall& call)
{
    const char* smt = "setmetatable";

    if (call.args.size != 2)
        return false;

    const AstExprGlobal* funcAsGlobal = call.func->as<AstExprGlobal>();
    if (!funcAsGlobal || funcAsGlobal->name != smt)
        return false;

    return true;
}

struct TypeGuard
{
    bool isTypeof;
    AstExpr* target;
    std::string type;
};

static std::optional<TypeGuard> matchTypeGuard(const AstExprBinary* binary)
{
    if (binary->op != AstExprBinary::CompareEq && binary->op != AstExprBinary::CompareNe)
        return std::nullopt;

    AstExpr* left = binary->left;
    AstExpr* right = binary->right;
    if (right->is<AstExprCall>())
        std::swap(left, right);

    if (!right->is<AstExprConstantString>())
        return std::nullopt;

    AstExprCall* call = left->as<AstExprCall>();
    AstExprConstantString* string = right->as<AstExprConstantString>();
    if (!call || !string)
        return std::nullopt;

    AstExprGlobal* callee = call->func->as<AstExprGlobal>();
    if (!callee)
        return std::nullopt;

    if (callee->name != "type" && callee->name != "typeof")
        return std::nullopt;

    if (call->args.size != 1)
        return std::nullopt;

    return TypeGuard{
        /*isTypeof*/ callee->name == "typeof",
        /*target*/ call->args.data[0],
        /*type*/ std::string(string->value.data, string->value.size),
    };
}

static bool matchAssert(const AstExprCall& call)
{
    if (call.args.size < 1)
        return false;

    const AstExprGlobal* funcAsGlobal = call.func->as<AstExprGlobal>();
    if (!funcAsGlobal || funcAsGlobal->name != "assert")
        return false;

    return true;
}

namespace
{

struct Checkpoint
{
    size_t offset;
};

Checkpoint checkpoint(const ConstraintGraphBuilder* cgb)
{
    return Checkpoint{cgb->constraints.size()};
}

template<typename F>
void forEachConstraint(const Checkpoint& start, const Checkpoint& end, const ConstraintGraphBuilder* cgb, F f)
{
    for (size_t i = start.offset; i < end.offset; ++i)
        f(cgb->constraints[i]);
}

} // namespace

ConstraintGraphBuilder::ConstraintGraphBuilder(const ModuleName& moduleName, ModulePtr module, TypeArena* arena,
    NotNull<ModuleResolver> moduleResolver, NotNull<BuiltinTypes> builtinTypes, NotNull<InternalErrorReporter> ice, const ScopePtr& globalScope,
    DcrLogger* logger, NotNull<DataFlowGraph> dfg)
    : moduleName(moduleName)
    , module(module)
    , builtinTypes(builtinTypes)
    , arena(arena)
    , rootScope(nullptr)
    , dfg(dfg)
    , moduleResolver(moduleResolver)
    , ice(ice)
    , globalScope(globalScope)
    , logger(logger)
{
    if (FFlag::DebugLuauLogSolverToJson)
        LUAU_ASSERT(logger);

    LUAU_ASSERT(module);
}

TypeId ConstraintGraphBuilder::freshType(const ScopePtr& scope)
{
    return arena->addType(FreeType{scope.get()});
}

TypePackId ConstraintGraphBuilder::freshTypePack(const ScopePtr& scope)
{
    FreeTypePack f{scope.get()};
    return arena->addTypePack(TypePackVar{std::move(f)});
}

ScopePtr ConstraintGraphBuilder::childScope(AstNode* node, const ScopePtr& parent)
{
    auto scope = std::make_shared<Scope>(parent);
    scopes.emplace_back(node->location, scope);

    scope->returnType = parent->returnType;
    scope->varargPack = parent->varargPack;

    parent->children.push_back(NotNull{scope.get()});
    module->astScopes[node] = scope.get();

    return scope;
}

NotNull<Constraint> ConstraintGraphBuilder::addConstraint(const ScopePtr& scope, const Location& location, ConstraintV cv)
{
    return NotNull{constraints.emplace_back(new Constraint{NotNull{scope.get()}, location, std::move(cv)}).get()};
}

NotNull<Constraint> ConstraintGraphBuilder::addConstraint(const ScopePtr& scope, std::unique_ptr<Constraint> c)
{
    return NotNull{constraints.emplace_back(std::move(c)).get()};
}

static void unionRefinements(const std::unordered_map<DefId, TypeId>& lhs, const std::unordered_map<DefId, TypeId>& rhs,
    std::unordered_map<DefId, TypeId>& dest, NotNull<TypeArena> arena)
{
    for (auto [def, ty] : lhs)
    {
        auto rhsIt = rhs.find(def);
        if (rhsIt == rhs.end())
            continue;

        std::vector<TypeId> discriminants{{ty, rhsIt->second}};

        if (auto destIt = dest.find(def); destIt != dest.end())
            discriminants.push_back(destIt->second);

        dest[def] = arena->addType(UnionType{std::move(discriminants)});
    }
}

static void computeRefinement(const ScopePtr& scope, RefinementId refinement, std::unordered_map<DefId, TypeId>* refis, bool sense,
    NotNull<TypeArena> arena, bool eq, std::vector<ConstraintV>* constraints)
{
    using RefinementMap = std::unordered_map<DefId, TypeId>;

    if (!refinement)
        return;
    else if (auto variadic = get<Variadic>(refinement))
    {
        for (RefinementId refi : variadic->refinements)
            computeRefinement(scope, refi, refis, sense, arena, eq, constraints);
    }
    else if (auto negation = get<Negation>(refinement))
        return computeRefinement(scope, negation->refinement, refis, !sense, arena, eq, constraints);
    else if (auto conjunction = get<Conjunction>(refinement))
    {
        RefinementMap lhsRefis;
        RefinementMap rhsRefis;

        computeRefinement(scope, conjunction->lhs, sense ? refis : &lhsRefis, sense, arena, eq, constraints);
        computeRefinement(scope, conjunction->rhs, sense ? refis : &rhsRefis, sense, arena, eq, constraints);

        if (!sense)
            unionRefinements(lhsRefis, rhsRefis, *refis, arena);
    }
    else if (auto disjunction = get<Disjunction>(refinement))
    {
        RefinementMap lhsRefis;
        RefinementMap rhsRefis;

        computeRefinement(scope, disjunction->lhs, sense ? &lhsRefis : refis, sense, arena, eq, constraints);
        computeRefinement(scope, disjunction->rhs, sense ? &rhsRefis : refis, sense, arena, eq, constraints);

        if (sense)
            unionRefinements(lhsRefis, rhsRefis, *refis, arena);
    }
    else if (auto equivalence = get<Equivalence>(refinement))
    {
        computeRefinement(scope, equivalence->lhs, refis, sense, arena, true, constraints);
        computeRefinement(scope, equivalence->rhs, refis, sense, arena, true, constraints);
    }
    else if (auto proposition = get<Proposition>(refinement))
    {
        TypeId discriminantTy = proposition->discriminantTy;
        if (!sense && !eq)
            discriminantTy = arena->addType(NegationType{proposition->discriminantTy});
        else if (eq)
        {
            discriminantTy = arena->addType(BlockedType{});
            constraints->push_back(SingletonOrTopTypeConstraint{discriminantTy, proposition->discriminantTy, !sense});
        }

        if (auto it = refis->find(proposition->def); it != refis->end())
            (*refis)[proposition->def] = arena->addType(IntersectionType{{discriminantTy, it->second}});
        else
            (*refis)[proposition->def] = discriminantTy;
    }
}

static std::pair<DefId, TypeId> computeDiscriminantType(NotNull<TypeArena> arena, const ScopePtr& scope, DefId def, TypeId discriminantTy)
{
    LUAU_ASSERT(get<Cell>(def));

    while (const Cell* current = get<Cell>(def))
    {
        if (!current->field)
            break;

        TableType::Props props{{current->field->propName, Property{discriminantTy}}};
        discriminantTy = arena->addType(TableType{std::move(props), std::nullopt, TypeLevel{}, scope.get(), TableState::Sealed});

        def = current->field->parent;
        current = get<Cell>(def);
    }

    return {def, discriminantTy};
}

void ConstraintGraphBuilder::applyRefinements(const ScopePtr& scope, Location location, RefinementId refinement)
{
    if (!refinement)
        return;

    std::unordered_map<DefId, TypeId> refinements;
    std::vector<ConstraintV> constraints;
    computeRefinement(scope, refinement, &refinements, /*sense*/ true, arena, /*eq*/ false, &constraints);

    for (auto [def, discriminantTy] : refinements)
    {
        auto [def2, discriminantTy2] = computeDiscriminantType(arena, scope, def, discriminantTy);
        std::optional<TypeId> defTy = scope->lookup(def2);
        if (!defTy)
            ice->ice("Every DefId must map to a type!");

        TypeId resultTy = arena->addType(IntersectionType{{*defTy, discriminantTy2}});
        scope->dcrRefinements[def2] = resultTy;
    }

    for (auto& c : constraints)
        addConstraint(scope, location, c);
}

void ConstraintGraphBuilder::visit(AstStatBlock* block)
{
    LUAU_ASSERT(scopes.empty());
    LUAU_ASSERT(rootScope == nullptr);
    ScopePtr scope = std::make_shared<Scope>(globalScope);
    rootScope = scope.get();
    scopes.emplace_back(block->location, scope);
    module->astScopes[block] = NotNull{scope.get()};

    rootScope->returnType = freshTypePack(scope);

    prepopulateGlobalScope(scope, block);

    visitBlockWithoutChildScope(scope, block);
}

void ConstraintGraphBuilder::visitBlockWithoutChildScope(const ScopePtr& scope, AstStatBlock* block)
{
    RecursionCounter counter{&recursionCount};

    if (recursionCount >= FInt::LuauCheckRecursionLimit)
    {
        reportCodeTooComplex(block->location);
        return;
    }

    std::unordered_map<Name, Location> aliasDefinitionLocations;

    // In order to enable mutually-recursive type aliases, we need to
    // populate the type bindings before we actually check any of the
    // alias statements.
    for (AstStat* stat : block->body)
    {
        if (auto alias = stat->as<AstStatTypeAlias>())
        {
            if (scope->exportedTypeBindings.count(alias->name.value) || scope->privateTypeBindings.count(alias->name.value))
            {
                auto it = aliasDefinitionLocations.find(alias->name.value);
                LUAU_ASSERT(it != aliasDefinitionLocations.end());
                reportError(alias->location, DuplicateTypeDefinition{alias->name.value, it->second});
                continue;
            }

            ScopePtr defnScope = childScope(alias, scope);

            TypeId initialType = arena->addType(BlockedType{});
            TypeFun initialFun{initialType};

            for (const auto& [name, gen] : createGenerics(defnScope, alias->generics, /* useCache */ true))
            {
                initialFun.typeParams.push_back(gen);
                defnScope->privateTypeBindings[name] = TypeFun{gen.ty};
            }

            for (const auto& [name, genPack] : createGenericPacks(defnScope, alias->genericPacks, /* useCache */ true))
            {
                initialFun.typePackParams.push_back(genPack);
                defnScope->privateTypePackBindings[name] = genPack.tp;
            }

            if (alias->exported)
                scope->exportedTypeBindings[alias->name.value] = std::move(initialFun);
            else
                scope->privateTypeBindings[alias->name.value] = std::move(initialFun);

            astTypeAliasDefiningScopes[alias] = defnScope;
            aliasDefinitionLocations[alias->name.value] = alias->location;
        }
    }

    for (AstStat* stat : block->body)
        visit(scope, stat);
}

void ConstraintGraphBuilder::visit(const ScopePtr& scope, AstStat* stat)
{
    RecursionLimiter limiter{&recursionCount, FInt::LuauCheckRecursionLimit};

    if (auto s = stat->as<AstStatBlock>())
        visit(scope, s);
    else if (auto i = stat->as<AstStatIf>())
        visit(scope, i);
    else if (auto s = stat->as<AstStatWhile>())
        visit(scope, s);
    else if (auto s = stat->as<AstStatRepeat>())
        visit(scope, s);
    else if (stat->is<AstStatBreak>() || stat->is<AstStatContinue>())
    {
        // Nothing
    }
    else if (auto r = stat->as<AstStatReturn>())
        visit(scope, r);
    else if (auto e = stat->as<AstStatExpr>())
        checkPack(scope, e->expr);
    else if (auto s = stat->as<AstStatLocal>())
        visit(scope, s);
    else if (auto s = stat->as<AstStatFor>())
        visit(scope, s);
    else if (auto s = stat->as<AstStatForIn>())
        visit(scope, s);
    else if (auto a = stat->as<AstStatAssign>())
        visit(scope, a);
    else if (auto a = stat->as<AstStatCompoundAssign>())
        visit(scope, a);
    else if (auto f = stat->as<AstStatFunction>())
        visit(scope, f);
    else if (auto f = stat->as<AstStatLocalFunction>())
        visit(scope, f);
    else if (auto a = stat->as<AstStatTypeAlias>())
        visit(scope, a);
    else if (auto s = stat->as<AstStatDeclareGlobal>())
        visit(scope, s);
    else if (auto s = stat->as<AstStatDeclareFunction>())
        visit(scope, s);
    else if (auto s = stat->as<AstStatDeclareClass>())
        visit(scope, s);
    else if (auto s = stat->as<AstStatError>())
        visit(scope, s);
    else
        LUAU_ASSERT(0 && "Internal error: Unknown AstStat type");
}

void ConstraintGraphBuilder::visit(const ScopePtr& scope, AstStatLocal* local)
{
    std::vector<TypeId> varTypes;
    varTypes.reserve(local->vars.size);

    // Used to name the first value type, even if it's not placed in varTypes,
    // for the purpose of synthetic name attribution.
    std::optional<TypeId> firstValueType;

    for (AstLocal* local : local->vars)
    {
        TypeId ty = nullptr;

        if (local->annotation)
            ty = resolveType(scope, local->annotation, /* inTypeArguments */ false);

        varTypes.push_back(ty);
    }

    for (size_t i = 0; i < local->values.size; ++i)
    {
        AstExpr* value = local->values.data[i];
        const bool hasAnnotation = i < local->vars.size && nullptr != local->vars.data[i]->annotation;

        if (value->is<AstExprConstantNil>())
        {
            // HACK: we leave nil-initialized things floating under the
            // assumption that they will later be populated.
            //
            // See the test TypeInfer/infer_locals_with_nil_value. Better flow
            // awareness should make this obsolete.

            if (!varTypes[i])
                varTypes[i] = freshType(scope);
        }
        // Only function calls and vararg expressions can produce packs.  All
        // other expressions produce exactly one value.
        else if (i != local->values.size - 1 || (!value->is<AstExprCall>() && !value->is<AstExprVarargs>()))
        {
            std::optional<TypeId> expectedType;
            if (hasAnnotation)
                expectedType = varTypes.at(i);

            TypeId exprType = check(scope, value, expectedType).ty;
            if (i < varTypes.size())
            {
                if (varTypes[i])
                    addConstraint(scope, local->location, SubtypeConstraint{exprType, varTypes[i]});
                else
                    varTypes[i] = exprType;
            }

            if (i == 0)
                firstValueType = exprType;
        }
        else
        {
            std::vector<std::optional<TypeId>> expectedTypes;
            if (hasAnnotation)
                expectedTypes.insert(begin(expectedTypes), begin(varTypes) + i, end(varTypes));

            TypePackId exprPack = checkPack(scope, value, expectedTypes).tp;

            if (i < local->vars.size)
            {
                TypePack packTypes = extendTypePack(*arena, builtinTypes, exprPack, varTypes.size() - i);

                // fill out missing values in varTypes with values from exprPack
                for (size_t j = i; j < varTypes.size(); ++j)
                {
                    if (!varTypes[j])
                    {
                        if (j - i < packTypes.head.size())
                            varTypes[j] = packTypes.head[j - i];
                        else
                            varTypes[j] = freshType(scope);
                    }
                }

                std::vector<TypeId> tailValues{varTypes.begin() + i, varTypes.end()};
                TypePackId tailPack = arena->addTypePack(std::move(tailValues));
                addConstraint(scope, local->location, PackSubtypeConstraint{exprPack, tailPack});
            }
        }
    }

    if (local->vars.size == 1 && local->values.size == 1 && firstValueType)
    {
        AstLocal* var = local->vars.data[0];
        AstExpr* value = local->values.data[0];

        if (value->is<AstExprTable>())
            addConstraint(scope, value->location, NameConstraint{*firstValueType, var->name.value, /*synthetic*/ true});
        else if (const AstExprCall* call = value->as<AstExprCall>())
        {
            if (const AstExprGlobal* global = call->func->as<AstExprGlobal>(); global && global->name == "setmetatable")
            {
                addConstraint(scope, value->location, NameConstraint{*firstValueType, var->name.value, /*synthetic*/ true});
            }
        }
    }

    for (size_t i = 0; i < local->vars.size; ++i)
    {
        AstLocal* l = local->vars.data[i];
        Location location = l->location;

        if (!varTypes[i])
            varTypes[i] = freshType(scope);

        scope->bindings[l] = Binding{varTypes[i], location};

        // HACK: In the greedy solver, we say the type state of a variable is the type annotation itself, but
        // the actual type state is the corresponding initializer expression (if it exists) or nil otherwise.
        if (auto def = dfg->getDef(l))
            scope->dcrRefinements[*def] = varTypes[i];
    }

    if (local->values.size > 0)
    {
        // To correctly handle 'require', we need to import the exported type bindings into the variable 'namespace'.
        for (size_t i = 0; i < local->values.size && i < local->vars.size; ++i)
        {
            const AstExprCall* call = local->values.data[i]->as<AstExprCall>();
            if (!call)
                continue;

            if (auto maybeRequire = matchRequire(*call))
            {
                AstExpr* require = *maybeRequire;

                if (auto moduleInfo = moduleResolver->resolveModuleInfo(moduleName, *require))
                {
                    const Name name{local->vars.data[i]->name.value};

                    if (ModulePtr module = moduleResolver->getModule(moduleInfo->name))
                    {
                        scope->importedTypeBindings[name] = module->exportedTypeBindings;
                        if (FFlag::SupportTypeAliasGoToDeclaration)
                            scope->importedModules[name] = moduleName;
                    }
                }
            }
        }
    }
}

void ConstraintGraphBuilder::visit(const ScopePtr& scope, AstStatFor* for_)
{
    if (for_->var->annotation)
        resolveType(scope, for_->var->annotation, /* inTypeArguments */ false);

    auto inferNumber = [&](AstExpr* expr) {
        if (!expr)
            return;

        TypeId t = check(scope, expr).ty;
        addConstraint(scope, expr->location, SubtypeConstraint{t, builtinTypes->numberType});
    };

    inferNumber(for_->from);
    inferNumber(for_->to);
    inferNumber(for_->step);

    ScopePtr forScope = childScope(for_, scope);
    forScope->bindings[for_->var] = Binding{builtinTypes->numberType, for_->var->location};

    visit(forScope, for_->body);
}

void ConstraintGraphBuilder::visit(const ScopePtr& scope, AstStatForIn* forIn)
{
    ScopePtr loopScope = childScope(forIn, scope);

    TypePackId iterator = checkPack(scope, forIn->values).tp;

    std::vector<TypeId> variableTypes;
    variableTypes.reserve(forIn->vars.size);
    for (AstLocal* var : forIn->vars)
    {
        TypeId ty = freshType(loopScope);
        loopScope->bindings[var] = Binding{ty, var->location};
        variableTypes.push_back(ty);

        if (auto def = dfg->getDef(var))
            loopScope->dcrRefinements[*def] = ty;
    }

    // It is always ok to provide too few variables, so we give this pack a free tail.
    TypePackId variablePack = arena->addTypePack(std::move(variableTypes), arena->addTypePack(FreeTypePack{loopScope.get()}));

    addConstraint(loopScope, getLocation(forIn->values), IterableConstraint{iterator, variablePack});

    visit(loopScope, forIn->body);
}

void ConstraintGraphBuilder::visit(const ScopePtr& scope, AstStatWhile* while_)
{
    check(scope, while_->condition);

    ScopePtr whileScope = childScope(while_, scope);

    visit(whileScope, while_->body);
}

void ConstraintGraphBuilder::visit(const ScopePtr& scope, AstStatRepeat* repeat)
{
    ScopePtr repeatScope = childScope(repeat, scope);

    visit(repeatScope, repeat->body);

    // The condition does indeed have access to bindings from within the body of
    // the loop.
    check(repeatScope, repeat->condition);
}

void ConstraintGraphBuilder::visit(const ScopePtr& scope, AstStatLocalFunction* function)
{
    // Local
    // Global
    // Dotted path
    // Self?

    TypeId functionType = nullptr;
    auto ty = scope->lookup(function->name);
    LUAU_ASSERT(!ty.has_value()); // The parser ensures that every local function has a distinct Symbol for its name.

    functionType = arena->addType(BlockedType{});
    scope->bindings[function->name] = Binding{functionType, function->name->location};

    FunctionSignature sig = checkFunctionSignature(scope, function->func);
    sig.bodyScope->bindings[function->name] = Binding{sig.signature, function->func->location};

    Checkpoint start = checkpoint(this);
    checkFunctionBody(sig.bodyScope, function->func);
    Checkpoint end = checkpoint(this);

    NotNull<Scope> constraintScope{sig.signatureScope ? sig.signatureScope.get() : sig.bodyScope.get()};
    std::unique_ptr<Constraint> c =
        std::make_unique<Constraint>(constraintScope, function->name->location, GeneralizationConstraint{functionType, sig.signature});

    forEachConstraint(start, end, this, [&c](const ConstraintPtr& constraint) {
        c->dependencies.push_back(NotNull{constraint.get()});
    });

    addConstraint(scope, std::move(c));
}

void ConstraintGraphBuilder::visit(const ScopePtr& scope, AstStatFunction* function)
{
    // Name could be AstStatLocal, AstStatGlobal, AstStatIndexName.
    // With or without self

    TypeId generalizedType = arena->addType(BlockedType{});

    Checkpoint start = checkpoint(this);
    FunctionSignature sig = checkFunctionSignature(scope, function->func);

    if (AstExprLocal* localName = function->name->as<AstExprLocal>())
    {
        std::optional<TypeId> existingFunctionTy = scope->lookup(localName->local);
        if (existingFunctionTy)
        {
            addConstraint(scope, function->name->location, SubtypeConstraint{generalizedType, *existingFunctionTy});

            Symbol sym{localName->local};
            std::optional<DefId> def = dfg->getDef(sym);
            LUAU_ASSERT(def);
            scope->bindings[sym].typeId = generalizedType;
            scope->dcrRefinements[*def] = generalizedType;
        }
        else
            scope->bindings[localName->local] = Binding{generalizedType, localName->location};

        sig.bodyScope->bindings[localName->local] = Binding{sig.signature, localName->location};
    }
    else if (AstExprGlobal* globalName = function->name->as<AstExprGlobal>())
    {
        std::optional<TypeId> existingFunctionTy = scope->lookup(globalName->name);
        if (!existingFunctionTy)
            ice->ice("prepopulateGlobalScope did not populate a global name", globalName->location);

        generalizedType = *existingFunctionTy;

        sig.bodyScope->bindings[globalName->name] = Binding{sig.signature, globalName->location};
    }
    else if (AstExprIndexName* indexName = function->name->as<AstExprIndexName>())
    {
        TypeId lvalueType = checkLValue(scope, indexName);
        // TODO figure out how to populate the location field of the table Property.
        addConstraint(scope, indexName->location, SubtypeConstraint{lvalueType, generalizedType});
    }
    else if (AstExprError* err = function->name->as<AstExprError>())
    {
        generalizedType = builtinTypes->errorRecoveryType();
    }

    if (generalizedType == nullptr)
        ice->ice("generalizedType == nullptr", function->location);

    checkFunctionBody(sig.bodyScope, function->func);
    Checkpoint end = checkpoint(this);

    NotNull<Scope> constraintScope{sig.signatureScope ? sig.signatureScope.get() : sig.bodyScope.get()};
    std::unique_ptr<Constraint> c =
        std::make_unique<Constraint>(constraintScope, function->name->location, GeneralizationConstraint{generalizedType, sig.signature});

    forEachConstraint(start, end, this, [&c](const ConstraintPtr& constraint) {
        c->dependencies.push_back(NotNull{constraint.get()});
    });

    addConstraint(scope, std::move(c));
}

void ConstraintGraphBuilder::visit(const ScopePtr& scope, AstStatReturn* ret)
{
    // At this point, the only way scope->returnType should have anything
    // interesting in it is if the function has an explicit return annotation.
    // If this is the case, then we can expect that the return expression
    // conforms to that.
    std::vector<std::optional<TypeId>> expectedTypes;
    for (TypeId ty : scope->returnType)
        expectedTypes.push_back(ty);

    TypePackId exprTypes = checkPack(scope, ret->list, expectedTypes).tp;
    addConstraint(scope, ret->location, PackSubtypeConstraint{exprTypes, scope->returnType});
}

void ConstraintGraphBuilder::visit(const ScopePtr& scope, AstStatBlock* block)
{
    ScopePtr innerScope = childScope(block, scope);

    visitBlockWithoutChildScope(innerScope, block);
}

void ConstraintGraphBuilder::visit(const ScopePtr& scope, AstStatAssign* assign)
{
    TypePackId varPackId = checkLValues(scope, assign->vars);

    TypePack expectedPack = extendTypePack(*arena, builtinTypes, varPackId, assign->values.size);

    std::vector<std::optional<TypeId>> expectedTypes;
    expectedTypes.reserve(expectedPack.head.size());

    for (TypeId ty : expectedPack.head)
    {
        ty = follow(ty);
        if (get<FreeType>(ty))
            expectedTypes.push_back(std::nullopt);
        else
            expectedTypes.push_back(ty);
    }

    TypePackId valuePack = checkPack(scope, assign->values, expectedTypes).tp;

    addConstraint(scope, assign->location, PackSubtypeConstraint{valuePack, varPackId});
}

void ConstraintGraphBuilder::visit(const ScopePtr& scope, AstStatCompoundAssign* assign)
{
    // We need to tweak the BinaryConstraint that we emit, so we cannot use the
    // strategy of falsifying an AST fragment.
    TypeId varId = checkLValue(scope, assign->var);
    Inference valueInf = check(scope, assign->value);

    TypeId resultType = arena->addType(BlockedType{});
    addConstraint(scope, assign->location,
        BinaryConstraint{assign->op, varId, valueInf.ty, resultType, assign, &module->astOriginalCallTypes, &module->astOverloadResolvedTypes});
    addConstraint(scope, assign->location, SubtypeConstraint{resultType, varId});
}

void ConstraintGraphBuilder::visit(const ScopePtr& scope, AstStatIf* ifStatement)
{
    ScopePtr condScope = childScope(ifStatement->condition, scope);
    auto [_, refinement] = check(condScope, ifStatement->condition, std::nullopt);

    ScopePtr thenScope = childScope(ifStatement->thenbody, scope);
    applyRefinements(thenScope, ifStatement->condition->location, refinement);
    visit(thenScope, ifStatement->thenbody);

    if (ifStatement->elsebody)
    {
        ScopePtr elseScope = childScope(ifStatement->elsebody, scope);
        applyRefinements(elseScope, ifStatement->elseLocation.value_or(ifStatement->condition->location), refinementArena.negation(refinement));
        visit(elseScope, ifStatement->elsebody);
    }
}

static bool occursCheck(TypeId needle, TypeId haystack)
{
    LUAU_ASSERT(get<BlockedType>(needle));
    haystack = follow(haystack);

    auto checkHaystack = [needle](TypeId haystack) {
        return occursCheck(needle, haystack);
    };

    if (needle == haystack)
        return true;
    else if (auto ut = get<UnionType>(haystack))
        return std::any_of(begin(ut), end(ut), checkHaystack);
    else if (auto it = get<IntersectionType>(haystack))
        return std::any_of(begin(it), end(it), checkHaystack);

    return false;
}

void ConstraintGraphBuilder::visit(const ScopePtr& scope, AstStatTypeAlias* alias)
{
    ScopePtr* defnScope = astTypeAliasDefiningScopes.find(alias);

    std::unordered_map<Name, TypeFun>* typeBindings;
    if (alias->exported)
        typeBindings = &scope->exportedTypeBindings;
    else
        typeBindings = &scope->privateTypeBindings;

    // These will be undefined if the alias was a duplicate definition, in which
    // case we just skip over it.
    auto bindingIt = typeBindings->find(alias->name.value);
    if (bindingIt == typeBindings->end() || defnScope == nullptr)
        return;

    TypeId ty = resolveType(*defnScope, alias->type, /* inTypeArguments */ false);

    TypeId aliasTy = bindingIt->second.type;
    LUAU_ASSERT(get<BlockedType>(aliasTy));

    if (occursCheck(aliasTy, ty))
    {
        asMutable(aliasTy)->ty.emplace<BoundType>(builtinTypes->anyType);
        reportError(alias->nameLocation, OccursCheckFailed{});
    }
    else
        asMutable(aliasTy)->ty.emplace<BoundType>(ty);

    std::vector<TypeId> typeParams;
    for (auto tyParam : createGenerics(*defnScope, alias->generics, /* useCache */ true))
        typeParams.push_back(tyParam.second.ty);

    std::vector<TypePackId> typePackParams;
    for (auto tpParam : createGenericPacks(*defnScope, alias->genericPacks, /* useCache */ true))
        typePackParams.push_back(tpParam.second.tp);

    addConstraint(scope, alias->type->location,
        NameConstraint{
            ty,
            alias->name.value,
            /*synthetic=*/false,
            std::move(typeParams),
            std::move(typePackParams),
        });
}

void ConstraintGraphBuilder::visit(const ScopePtr& scope, AstStatDeclareGlobal* global)
{
    LUAU_ASSERT(global->type);

    TypeId globalTy = resolveType(scope, global->type, /* inTypeArguments */ false);
    Name globalName(global->name.value);

    module->declaredGlobals[globalName] = globalTy;
    scope->bindings[global->name] = Binding{globalTy, global->location};
}

static bool isMetamethod(const Name& name)
{
    return name == "__index" || name == "__newindex" || name == "__call" || name == "__concat" || name == "__unm" || name == "__add" ||
           name == "__sub" || name == "__mul" || name == "__div" || name == "__mod" || name == "__pow" || name == "__tostring" ||
           name == "__metatable" || name == "__eq" || name == "__lt" || name == "__le" || name == "__mode" || name == "__iter" || name == "__len";
}

void ConstraintGraphBuilder::visit(const ScopePtr& scope, AstStatDeclareClass* declaredClass)
{
    std::optional<TypeId> superTy = FFlag::LuauNegatedClassTypes ? std::make_optional(builtinTypes->classType) : std::nullopt;
    if (declaredClass->superName)
    {
        Name superName = Name(declaredClass->superName->value);
        std::optional<TypeFun> lookupType = scope->lookupType(superName);

        if (!lookupType)
        {
            reportError(declaredClass->location, UnknownSymbol{superName, UnknownSymbol::Type});
            return;
        }

        // We don't have generic classes, so this assertion _should_ never be hit.
        LUAU_ASSERT(lookupType->typeParams.size() == 0 && lookupType->typePackParams.size() == 0);
        superTy = lookupType->type;

        if (!get<ClassType>(follow(*superTy)))
        {
            reportError(declaredClass->location,
                GenericError{format("Cannot use non-class type '%s' as a superclass of class '%s'", superName.c_str(), declaredClass->name.value)});

            return;
        }
    }

    Name className(declaredClass->name.value);

    TypeId classTy = arena->addType(ClassType(className, {}, superTy, std::nullopt, {}, {}, moduleName));
    ClassType* ctv = getMutable<ClassType>(classTy);

    TypeId metaTy = arena->addType(TableType{TableState::Sealed, scope->level, scope.get()});
    TableType* metatable = getMutable<TableType>(metaTy);

    ctv->metatable = metaTy;

    scope->exportedTypeBindings[className] = TypeFun{{}, classTy};

    for (const AstDeclaredClassProp& prop : declaredClass->props)
    {
        Name propName(prop.name.value);
        TypeId propTy = resolveType(scope, prop.ty, /* inTypeArguments */ false);

        bool assignToMetatable = isMetamethod(propName);

        // Function types always take 'self', but this isn't reflected in the
        // parsed annotation. Add it here.
        if (prop.isMethod)
        {
            if (FunctionType* ftv = getMutable<FunctionType>(propTy))
            {
                ftv->argNames.insert(ftv->argNames.begin(), FunctionArgument{"self", {}});
                ftv->argTypes = arena->addTypePack(TypePack{{classTy}, ftv->argTypes});

                ftv->hasSelf = true;
            }
        }

        if (ctv->props.count(propName) == 0)
        {
            if (assignToMetatable)
                metatable->props[propName] = {propTy};
            else
                ctv->props[propName] = {propTy};
        }
        else
        {
            TypeId currentTy = assignToMetatable ? metatable->props[propName].type : ctv->props[propName].type;

            // We special-case this logic to keep the intersection flat; otherwise we
            // would create a ton of nested intersection types.
            if (const IntersectionType* itv = get<IntersectionType>(currentTy))
            {
                std::vector<TypeId> options = itv->parts;
                options.push_back(propTy);
                TypeId newItv = arena->addType(IntersectionType{std::move(options)});

                if (assignToMetatable)
                    metatable->props[propName] = {newItv};
                else
                    ctv->props[propName] = {newItv};
            }
            else if (get<FunctionType>(currentTy))
            {
                TypeId intersection = arena->addType(IntersectionType{{currentTy, propTy}});

                if (assignToMetatable)
                    metatable->props[propName] = {intersection};
                else
                    ctv->props[propName] = {intersection};
            }
            else
            {
                reportError(declaredClass->location, GenericError{format("Cannot overload non-function class member '%s'", propName.c_str())});
            }
        }
    }
}

void ConstraintGraphBuilder::visit(const ScopePtr& scope, AstStatDeclareFunction* global)
{
    std::vector<std::pair<Name, GenericTypeDefinition>> generics = createGenerics(scope, global->generics);
    std::vector<std::pair<Name, GenericTypePackDefinition>> genericPacks = createGenericPacks(scope, global->genericPacks);

    std::vector<TypeId> genericTys;
    genericTys.reserve(generics.size());
    for (auto& [name, generic] : generics)
    {
        genericTys.push_back(generic.ty);
        scope->privateTypeBindings[name] = TypeFun{generic.ty};
    }

    std::vector<TypePackId> genericTps;
    genericTps.reserve(genericPacks.size());
    for (auto& [name, generic] : genericPacks)
    {
        genericTps.push_back(generic.tp);
        scope->privateTypePackBindings[name] = generic.tp;
    }

    ScopePtr funScope = scope;
    if (!generics.empty() || !genericPacks.empty())
        funScope = childScope(global, scope);

    TypePackId paramPack = resolveTypePack(funScope, global->params, /* inTypeArguments */ false);
    TypePackId retPack = resolveTypePack(funScope, global->retTypes, /* inTypeArguments */ false);
    TypeId fnType = arena->addType(FunctionType{TypeLevel{}, funScope.get(), std::move(genericTys), std::move(genericTps), paramPack, retPack});
    FunctionType* ftv = getMutable<FunctionType>(fnType);

    ftv->argNames.reserve(global->paramNames.size);
    for (const auto& el : global->paramNames)
        ftv->argNames.push_back(FunctionArgument{el.first.value, el.second});

    Name fnName(global->name.value);

    module->declaredGlobals[fnName] = fnType;
    scope->bindings[global->name] = Binding{fnType, global->location};
}

void ConstraintGraphBuilder::visit(const ScopePtr& scope, AstStatError* error)
{
    for (AstStat* stat : error->statements)
        visit(scope, stat);
    for (AstExpr* expr : error->expressions)
        check(scope, expr);
}

InferencePack ConstraintGraphBuilder::checkPack(
    const ScopePtr& scope, AstArray<AstExpr*> exprs, const std::vector<std::optional<TypeId>>& expectedTypes)
{
    std::vector<TypeId> head;
    std::optional<TypePackId> tail;

    for (size_t i = 0; i < exprs.size; ++i)
    {
        AstExpr* expr = exprs.data[i];
        if (i < exprs.size - 1)
        {
            std::optional<TypeId> expectedType;
            if (i < expectedTypes.size())
                expectedType = expectedTypes[i];
            head.push_back(check(scope, expr, expectedType).ty);
        }
        else
        {
            std::vector<std::optional<TypeId>> expectedTailTypes;
            if (i < expectedTypes.size())
                expectedTailTypes.assign(begin(expectedTypes) + i, end(expectedTypes));
            tail = checkPack(scope, expr, expectedTailTypes).tp;
        }
    }

    if (head.empty() && tail)
        return InferencePack{*tail};
    else
        return InferencePack{arena->addTypePack(TypePack{std::move(head), tail})};
}

InferencePack ConstraintGraphBuilder::checkPack(const ScopePtr& scope, AstExpr* expr, const std::vector<std::optional<TypeId>>& expectedTypes)
{
    RecursionCounter counter{&recursionCount};

    if (recursionCount >= FInt::LuauCheckRecursionLimit)
    {
        reportCodeTooComplex(expr->location);
        return InferencePack{builtinTypes->errorRecoveryTypePack()};
    }

    InferencePack result;

    if (AstExprCall* call = expr->as<AstExprCall>())
        result = checkPack(scope, call);
    else if (AstExprVarargs* varargs = expr->as<AstExprVarargs>())
    {
        if (scope->varargPack)
            result = InferencePack{*scope->varargPack};
        else
            result = InferencePack{builtinTypes->errorRecoveryTypePack()};
    }
    else
    {
        std::optional<TypeId> expectedType;
        if (!expectedTypes.empty())
            expectedType = expectedTypes[0];
        TypeId t = check(scope, expr, expectedType).ty;
        result = InferencePack{arena->addTypePack({t})};
    }

    LUAU_ASSERT(result.tp);
    module->astTypePacks[expr] = result.tp;
    return result;
}

InferencePack ConstraintGraphBuilder::checkPack(const ScopePtr& scope, AstExprCall* call)
{
    std::vector<AstExpr*> exprArgs;

    std::vector<RefinementId> returnRefinements;
    std::vector<std::optional<TypeId>> discriminantTypes;

    if (call->self)
    {
        AstExprIndexName* indexExpr = call->func->as<AstExprIndexName>();
        if (!indexExpr)
            ice->ice("method call expression has no 'self'");

        exprArgs.push_back(indexExpr->expr);

        if (auto def = dfg->getDef(indexExpr->expr))
        {
            TypeId discriminantTy = arena->addType(BlockedType{});
            returnRefinements.push_back(refinementArena.proposition(*def, discriminantTy));
            discriminantTypes.push_back(discriminantTy);
        }
        else
            discriminantTypes.push_back(std::nullopt);
    }

    for (AstExpr* arg : call->args)
    {
        exprArgs.push_back(arg);

        if (auto def = dfg->getDef(arg))
        {
            TypeId discriminantTy = arena->addType(BlockedType{});
            returnRefinements.push_back(refinementArena.proposition(*def, discriminantTy));
            discriminantTypes.push_back(discriminantTy);
        }
        else
            discriminantTypes.push_back(std::nullopt);
    }

    Checkpoint startCheckpoint = checkpoint(this);
    TypeId fnType = check(scope, call->func).ty;
    Checkpoint fnEndCheckpoint = checkpoint(this);

    module->astOriginalCallTypes[call->func] = fnType;

    TypePackId expectedArgPack = arena->freshTypePack(scope.get());
    TypePackId expectedRetPack = arena->freshTypePack(scope.get());
    TypeId expectedFunctionType = arena->addType(FunctionType{expectedArgPack, expectedRetPack});

    TypeId instantiatedFnType = arena->addType(BlockedType{});
    addConstraint(scope, call->location, InstantiationConstraint{instantiatedFnType, fnType});

    NotNull<Constraint> extractArgsConstraint = addConstraint(scope, call->location, SubtypeConstraint{instantiatedFnType, expectedFunctionType});

    // Fully solve fnType, then extract its argument list as expectedArgPack.
    forEachConstraint(startCheckpoint, fnEndCheckpoint, this, [extractArgsConstraint](const ConstraintPtr& constraint) {
        extractArgsConstraint->dependencies.emplace_back(constraint.get());
    });

    const AstExpr* lastArg = exprArgs.size() ? exprArgs[exprArgs.size() - 1] : nullptr;
    const bool needTail = lastArg && (lastArg->is<AstExprCall>() || lastArg->is<AstExprVarargs>());

    TypePack expectedArgs;

    if (!needTail)
        expectedArgs = extendTypePack(*arena, builtinTypes, expectedArgPack, exprArgs.size());
    else
        expectedArgs = extendTypePack(*arena, builtinTypes, expectedArgPack, exprArgs.size() - 1);

    std::vector<TypeId> args;
    std::optional<TypePackId> argTail;
    std::vector<RefinementId> argumentRefinements;

    Checkpoint argCheckpoint = checkpoint(this);

    for (size_t i = 0; i < exprArgs.size(); ++i)
    {
        AstExpr* arg = exprArgs[i];
        std::optional<TypeId> expectedType;
        if (i < expectedArgs.head.size())
            expectedType = expectedArgs.head[i];

        if (i == 0 && call->self)
        {
            // The self type has already been computed as a side effect of
            // computing fnType.  If computing that did not cause us to exceed a
            // recursion limit, we can fetch it from astTypes rather than
            // recomputing it.
            TypeId* selfTy = module->astTypes.find(exprArgs[0]);
            if (selfTy)
                args.push_back(*selfTy);
            else
                args.push_back(arena->freshType(scope.get()));
        }
        else if (i < exprArgs.size() - 1 || !(arg->is<AstExprCall>() || arg->is<AstExprVarargs>()))
        {
            auto [ty, refinement] = check(scope, arg, expectedType);
            args.push_back(ty);
            argumentRefinements.push_back(refinement);
        }
        else
        {
            auto [tp, refis] = checkPack(scope, arg, {});
            argTail = tp;
            argumentRefinements.insert(argumentRefinements.end(), refis.begin(), refis.end());
        }
    }

    Checkpoint argEndCheckpoint = checkpoint(this);

    // Do not solve argument constraints until after we have extracted the
    // expected types from the callable.
    forEachConstraint(argCheckpoint, argEndCheckpoint, this, [extractArgsConstraint](const ConstraintPtr& constraint) {
        constraint->dependencies.push_back(extractArgsConstraint);
    });

    if (matchSetmetatable(*call))
    {
        TypePack argTailPack;
        if (argTail && args.size() < 2)
            argTailPack = extendTypePack(*arena, builtinTypes, *argTail, 2 - args.size());

        LUAU_ASSERT(args.size() + argTailPack.head.size() == 2);

        TypeId target = args.size() > 0 ? args[0] : argTailPack.head[0];
        TypeId mt = args.size() > 1 ? args[1] : argTailPack.head[args.size() == 0 ? 1 : 0];

        AstExpr* targetExpr = call->args.data[0];

        MetatableType mtv{target, mt};
        TypeId resultTy = arena->addType(mtv);

        if (AstExprLocal* targetLocal = targetExpr->as<AstExprLocal>())
        {
            scope->bindings[targetLocal->local].typeId = resultTy;
            auto def = dfg->getDef(targetLocal->local);
            if (def)
                scope->dcrRefinements[*def] = resultTy; // TODO: typestates: track this as an assignment
        }

        return InferencePack{arena->addTypePack({resultTy}), {refinementArena.variadic(returnRefinements)}};
    }
    else
    {
        if (matchAssert(*call) && !argumentRefinements.empty())
            applyRefinements(scope, call->args.data[0]->location, argumentRefinements[0]);

        // TODO: How do expectedTypes play into this?  Do they?
        TypePackId rets = arena->addTypePack(BlockedTypePack{});
        TypePackId argPack = arena->addTypePack(TypePack{args, argTail});
        FunctionType ftv(TypeLevel{}, scope.get(), argPack, rets);

        NotNull<Constraint> fcc = addConstraint(scope, call->func->location,
            FunctionCallConstraint{
                fnType,
                argPack,
                rets,
                call,
                std::move(discriminantTypes),
            });

        // We force constraints produced by checking function arguments to wait
        // until after we have resolved the constraint on the function itself.
        // This ensures, for instance, that we start inferring the contents of
        // lambdas under the assumption that their arguments and return types
        // will be compatible with the enclosing function call.
        forEachConstraint(fnEndCheckpoint, argEndCheckpoint, this, [fcc](const ConstraintPtr& constraint) {
            fcc->dependencies.emplace_back(constraint.get());
        });

        return InferencePack{rets, {refinementArena.variadic(returnRefinements)}};
    }
}

Inference ConstraintGraphBuilder::check(const ScopePtr& scope, AstExpr* expr, std::optional<TypeId> expectedType, bool forceSingleton)
{
    RecursionCounter counter{&recursionCount};

    if (recursionCount >= FInt::LuauCheckRecursionLimit)
    {
        reportCodeTooComplex(expr->location);
        return Inference{builtinTypes->errorRecoveryType()};
    }

    Inference result;

    if (auto group = expr->as<AstExprGroup>())
        result = check(scope, group->expr, expectedType, forceSingleton);
    else if (auto stringExpr = expr->as<AstExprConstantString>())
        result = check(scope, stringExpr, expectedType, forceSingleton);
    else if (expr->is<AstExprConstantNumber>())
        result = Inference{builtinTypes->numberType};
    else if (auto boolExpr = expr->as<AstExprConstantBool>())
        result = check(scope, boolExpr, expectedType, forceSingleton);
    else if (expr->is<AstExprConstantNil>())
        result = Inference{builtinTypes->nilType};
    else if (auto local = expr->as<AstExprLocal>())
        result = check(scope, local);
    else if (auto global = expr->as<AstExprGlobal>())
        result = check(scope, global);
    else if (expr->is<AstExprVarargs>())
        result = flattenPack(scope, expr->location, checkPack(scope, expr));
    else if (auto call = expr->as<AstExprCall>())
        result = flattenPack(scope, expr->location, checkPack(scope, call)); // TODO: needs predicates too
    else if (auto a = expr->as<AstExprFunction>())
    {
        Checkpoint startCheckpoint = checkpoint(this);
        FunctionSignature sig = checkFunctionSignature(scope, a, expectedType);
        checkFunctionBody(sig.bodyScope, a);
        Checkpoint endCheckpoint = checkpoint(this);

        TypeId generalizedTy = arena->addType(BlockedType{});
        NotNull<Constraint> gc = addConstraint(scope, expr->location, GeneralizationConstraint{generalizedTy, sig.signature});

        forEachConstraint(startCheckpoint, endCheckpoint, this, [gc](const ConstraintPtr& constraint) {
            gc->dependencies.emplace_back(constraint.get());
        });

        result = Inference{generalizedTy};
    }
    else if (auto indexName = expr->as<AstExprIndexName>())
        result = check(scope, indexName);
    else if (auto indexExpr = expr->as<AstExprIndexExpr>())
        result = check(scope, indexExpr);
    else if (auto table = expr->as<AstExprTable>())
        result = check(scope, table, expectedType);
    else if (auto unary = expr->as<AstExprUnary>())
        result = check(scope, unary);
    else if (auto binary = expr->as<AstExprBinary>())
        result = check(scope, binary, expectedType);
    else if (auto ifElse = expr->as<AstExprIfElse>())
        result = check(scope, ifElse, expectedType);
    else if (auto typeAssert = expr->as<AstExprTypeAssertion>())
        result = check(scope, typeAssert);
    else if (auto interpString = expr->as<AstExprInterpString>())
        result = check(scope, interpString);
    else if (auto err = expr->as<AstExprError>())
    {
        // Open question: Should we traverse into this?
        for (AstExpr* subExpr : err->expressions)
            check(scope, subExpr);

        result = Inference{builtinTypes->errorRecoveryType()};
    }
    else
    {
        LUAU_ASSERT(0);
        result = Inference{freshType(scope)};
    }

    LUAU_ASSERT(result.ty);
    module->astTypes[expr] = result.ty;
    if (expectedType)
        module->astExpectedTypes[expr] = *expectedType;
    return result;
}

Inference ConstraintGraphBuilder::check(const ScopePtr& scope, AstExprConstantString* string, std::optional<TypeId> expectedType, bool forceSingleton)
{
    if (forceSingleton)
        return Inference{arena->addType(SingletonType{StringSingleton{std::string{string->value.data, string->value.size}}})};

    if (expectedType)
    {
        const TypeId expectedTy = follow(*expectedType);
        if (get<BlockedType>(expectedTy) || get<PendingExpansionType>(expectedTy))
        {
            TypeId ty = arena->addType(BlockedType{});
            TypeId singletonType = arena->addType(SingletonType(StringSingleton{std::string(string->value.data, string->value.size)}));
            addConstraint(scope, string->location, PrimitiveTypeConstraint{ty, expectedTy, singletonType, builtinTypes->stringType});
            return Inference{ty};
        }
        else if (maybeSingleton(expectedTy))
            return Inference{arena->addType(SingletonType{StringSingleton{std::string{string->value.data, string->value.size}}})};

        return Inference{builtinTypes->stringType};
    }

    return Inference{builtinTypes->stringType};
}

Inference ConstraintGraphBuilder::check(const ScopePtr& scope, AstExprConstantBool* boolExpr, std::optional<TypeId> expectedType, bool forceSingleton)
{
    const TypeId singletonType = boolExpr->value ? builtinTypes->trueType : builtinTypes->falseType;
    if (forceSingleton)
        return Inference{singletonType};

    if (expectedType)
    {
        const TypeId expectedTy = follow(*expectedType);

        if (get<BlockedType>(expectedTy) || get<PendingExpansionType>(expectedTy))
        {
            TypeId ty = arena->addType(BlockedType{});
            addConstraint(scope, boolExpr->location, PrimitiveTypeConstraint{ty, expectedTy, singletonType, builtinTypes->booleanType});
            return Inference{ty};
        }
        else if (maybeSingleton(expectedTy))
            return Inference{singletonType};

        return Inference{builtinTypes->booleanType};
    }

    return Inference{builtinTypes->booleanType};
}

Inference ConstraintGraphBuilder::check(const ScopePtr& scope, AstExprLocal* local)
{
    std::optional<TypeId> resultTy;
    auto def = dfg->getDef(local);
    if (def)
        resultTy = scope->lookup(*def);

    if (!resultTy)
    {
        if (auto ty = scope->lookup(local->local))
            resultTy = *ty;
    }

    if (!resultTy)
        return Inference{builtinTypes->errorRecoveryType()}; // TODO: replace with ice, locals should never exist before its definition.

    if (def)
        return Inference{*resultTy, refinementArena.proposition(*def, builtinTypes->truthyType)};
    else
        return Inference{*resultTy};
}

Inference ConstraintGraphBuilder::check(const ScopePtr& scope, AstExprGlobal* global)
{
    if (std::optional<TypeId> ty = scope->lookup(global->name))
        return Inference{*ty};

    /* prepopulateGlobalScope() has already added all global functions to the environment by this point, so any
     * global that is not already in-scope is definitely an unknown symbol.
     */
    reportError(global->location, UnknownSymbol{global->name.value});
    return Inference{builtinTypes->errorRecoveryType()};
}

Inference ConstraintGraphBuilder::check(const ScopePtr& scope, AstExprIndexName* indexName)
{
    TypeId obj = check(scope, indexName->expr).ty;
    TypeId result = freshType(scope);

    std::optional<DefId> def = dfg->getDef(indexName);
    if (def)
    {
        if (auto ty = scope->lookup(*def))
            return Inference{*ty, refinementArena.proposition(*def, builtinTypes->truthyType)};
        else
            scope->dcrRefinements[*def] = result;
    }

    TableType::Props props{{indexName->index.value, Property{result}}};
    const std::optional<TableIndexer> indexer;
    TableType ttv{std::move(props), indexer, TypeLevel{}, scope.get(), TableState::Free};

    TypeId expectedTableType = arena->addType(std::move(ttv));

    addConstraint(scope, indexName->expr->location, SubtypeConstraint{obj, expectedTableType});

    if (def)
        return Inference{result, refinementArena.proposition(*def, builtinTypes->truthyType)};
    else
        return Inference{result};
}

Inference ConstraintGraphBuilder::check(const ScopePtr& scope, AstExprIndexExpr* indexExpr)
{
    TypeId obj = check(scope, indexExpr->expr).ty;
    TypeId indexType = check(scope, indexExpr->index).ty;

    TypeId result = freshType(scope);

    TableIndexer indexer{indexType, result};
    TypeId tableType = arena->addType(TableType{TableType::Props{}, TableIndexer{indexType, result}, TypeLevel{}, scope.get(), TableState::Free});

    addConstraint(scope, indexExpr->expr->location, SubtypeConstraint{obj, tableType});

    return Inference{result};
}

Inference ConstraintGraphBuilder::check(const ScopePtr& scope, AstExprUnary* unary)
{
    auto [operandType, refinement] = check(scope, unary->expr);
    TypeId resultType = arena->addType(BlockedType{});
    addConstraint(scope, unary->location, UnaryConstraint{unary->op, operandType, resultType});

    if (unary->op == AstExprUnary::Not)
        return Inference{resultType, refinementArena.negation(refinement)};
    else
        return Inference{resultType};
}

Inference ConstraintGraphBuilder::check(const ScopePtr& scope, AstExprBinary* binary, std::optional<TypeId> expectedType)
{
    auto [leftType, rightType, refinement] = checkBinary(scope, binary, expectedType);

    TypeId resultType = arena->addType(BlockedType{});
    addConstraint(scope, binary->location,
        BinaryConstraint{binary->op, leftType, rightType, resultType, binary, &module->astOriginalCallTypes, &module->astOverloadResolvedTypes});
    return Inference{resultType, std::move(refinement)};
}

Inference ConstraintGraphBuilder::check(const ScopePtr& scope, AstExprIfElse* ifElse, std::optional<TypeId> expectedType)
{
    ScopePtr condScope = childScope(ifElse->condition, scope);
    auto [_, refinement] = check(scope, ifElse->condition);

    ScopePtr thenScope = childScope(ifElse->trueExpr, scope);
    applyRefinements(thenScope, ifElse->trueExpr->location, refinement);
    TypeId thenType = check(thenScope, ifElse->trueExpr, expectedType).ty;

    ScopePtr elseScope = childScope(ifElse->falseExpr, scope);
    applyRefinements(elseScope, ifElse->falseExpr->location, refinementArena.negation(refinement));
    TypeId elseType = check(elseScope, ifElse->falseExpr, expectedType).ty;

    return Inference{expectedType ? *expectedType : arena->addType(UnionType{{thenType, elseType}})};
}

Inference ConstraintGraphBuilder::check(const ScopePtr& scope, AstExprTypeAssertion* typeAssert)
{
    check(scope, typeAssert->expr, std::nullopt);
    return Inference{resolveType(scope, typeAssert->annotation, /* inTypeArguments */ false)};
}

Inference ConstraintGraphBuilder::check(const ScopePtr& scope, AstExprInterpString* interpString)
{
    for (AstExpr* expr : interpString->expressions)
        check(scope, expr);

    return Inference{builtinTypes->stringType};
}

std::tuple<TypeId, TypeId, RefinementId> ConstraintGraphBuilder::checkBinary(
    const ScopePtr& scope, AstExprBinary* binary, std::optional<TypeId> expectedType)
{
    if (binary->op == AstExprBinary::And)
    {
        auto [leftType, leftRefinement] = check(scope, binary->left, expectedType);

        ScopePtr rightScope = childScope(binary->right, scope);
        applyRefinements(rightScope, binary->right->location, leftRefinement);
        auto [rightType, rightRefinement] = check(rightScope, binary->right, expectedType);

        return {leftType, rightType, refinementArena.conjunction(leftRefinement, rightRefinement)};
    }
    else if (binary->op == AstExprBinary::Or)
    {
        auto [leftType, leftRefinement] = check(scope, binary->left, expectedType);

        ScopePtr rightScope = childScope(binary->right, scope);
        applyRefinements(rightScope, binary->right->location, refinementArena.negation(leftRefinement));
        auto [rightType, rightRefinement] = check(rightScope, binary->right, expectedType);

        return {leftType, rightType, refinementArena.disjunction(leftRefinement, rightRefinement)};
    }
    else if (auto typeguard = matchTypeGuard(binary))
    {
        TypeId leftType = check(scope, binary->left).ty;
        TypeId rightType = check(scope, binary->right).ty;

        std::optional<DefId> def = dfg->getDef(typeguard->target);
        if (!def)
            return {leftType, rightType, nullptr};

        TypeId discriminantTy = builtinTypes->neverType;
        if (typeguard->type == "nil")
            discriminantTy = builtinTypes->nilType;
        else if (typeguard->type == "string")
            discriminantTy = builtinTypes->stringType;
        else if (typeguard->type == "number")
            discriminantTy = builtinTypes->numberType;
        else if (typeguard->type == "boolean")
            discriminantTy = builtinTypes->threadType;
        else if (typeguard->type == "table")
            discriminantTy = builtinTypes->tableType;
        else if (typeguard->type == "function")
            discriminantTy = builtinTypes->functionType;
        else if (typeguard->type == "userdata")
        {
            // For now, we don't really care about being accurate with userdata if the typeguard was using typeof
            discriminantTy = builtinTypes->neverType; // TODO: replace with top class type
        }
        else if (!typeguard->isTypeof && typeguard->type == "vector")
            discriminantTy = builtinTypes->neverType; // TODO: figure out a way to deal with this quirky type
        else if (!typeguard->isTypeof)
            discriminantTy = builtinTypes->neverType;
        else if (auto typeFun = globalScope->lookupType(typeguard->type); typeFun && typeFun->typeParams.empty() && typeFun->typePackParams.empty())
        {
            TypeId ty = follow(typeFun->type);

            // We're only interested in the root class of any classes.
            if (auto ctv = get<ClassType>(ty); !ctv || (FFlag::LuauNegatedClassTypes ? (ctv->parent == builtinTypes->classType) : !ctv->parent))
                discriminantTy = ty;
        }

        RefinementId proposition = refinementArena.proposition(*def, discriminantTy);
        if (binary->op == AstExprBinary::CompareEq)
            return {leftType, rightType, proposition};
        else if (binary->op == AstExprBinary::CompareNe)
            return {leftType, rightType, refinementArena.negation(proposition)};
        else
            ice->ice("matchTypeGuard should only return a Some under `==` or `~=`!");
    }
    else if (binary->op == AstExprBinary::CompareEq || binary->op == AstExprBinary::CompareNe)
    {
        TypeId leftType = check(scope, binary->left, expectedType, true).ty;
        TypeId rightType = check(scope, binary->right, expectedType, true).ty;

        RefinementId leftRefinement = nullptr;
        if (auto def = dfg->getDef(binary->left))
            leftRefinement = refinementArena.proposition(*def, rightType);

        RefinementId rightRefinement = nullptr;
        if (auto def = dfg->getDef(binary->right))
            rightRefinement = refinementArena.proposition(*def, leftType);

        if (binary->op == AstExprBinary::CompareNe)
        {
            leftRefinement = refinementArena.negation(leftRefinement);
            rightRefinement = refinementArena.negation(rightRefinement);
        }

        return {leftType, rightType, refinementArena.equivalence(leftRefinement, rightRefinement)};
    }
    else
    {
        TypeId leftType = check(scope, binary->left, expectedType).ty;
        TypeId rightType = check(scope, binary->right, expectedType).ty;
        return {leftType, rightType, nullptr};
    }
}

TypePackId ConstraintGraphBuilder::checkLValues(const ScopePtr& scope, AstArray<AstExpr*> exprs)
{
    std::vector<TypeId> types;
    types.reserve(exprs.size);

    for (size_t i = 0; i < exprs.size; ++i)
    {
        AstExpr* const expr = exprs.data[i];
        types.push_back(checkLValue(scope, expr));
    }

    return arena->addTypePack(std::move(types));
}

/**
 * This function is mostly about identifying properties that are being inserted into unsealed tables.
 *
 * If expr has the form name.a.b.c
 */
TypeId ConstraintGraphBuilder::checkLValue(const ScopePtr& scope, AstExpr* expr)
{
    if (auto indexExpr = expr->as<AstExprIndexExpr>())
    {
        if (auto constantString = indexExpr->index->as<AstExprConstantString>())
        {
            AstName syntheticIndex{constantString->value.data};
            AstExprIndexName synthetic{
                indexExpr->location, indexExpr->expr, syntheticIndex, constantString->location, indexExpr->expr->location.end, '.'};
            return checkLValue(scope, &synthetic);
        }
    }
    else if (!expr->is<AstExprIndexName>())
        return check(scope, expr).ty;

    Symbol sym;
    std::vector<std::string> segments;
    std::vector<AstExpr*> exprs;

    AstExpr* e = expr;
    while (e)
    {
        if (auto global = e->as<AstExprGlobal>())
        {
            sym = global->name;
            break;
        }
        else if (auto local = e->as<AstExprLocal>())
        {
            sym = local->local;
            break;
        }
        else if (auto indexName = e->as<AstExprIndexName>())
        {
            segments.push_back(indexName->index.value);
            exprs.push_back(e);
            e = indexName->expr;
        }
        else
            return check(scope, expr).ty;
    }

    LUAU_ASSERT(!segments.empty());

    std::reverse(begin(segments), end(segments));
    std::reverse(begin(exprs), end(exprs));

    auto lookupResult = scope->lookupEx(sym);
    if (!lookupResult)
        return check(scope, expr).ty;
    const auto [subjectType, symbolScope] = std::move(*lookupResult);

    TypeId propTy = freshType(scope);

    std::vector<std::string> segmentStrings(begin(segments), end(segments));

    TypeId updatedType = arena->addType(BlockedType{});
    addConstraint(scope, expr->location, SetPropConstraint{updatedType, subjectType, std::move(segmentStrings), propTy});

    TypeId prevSegmentTy = updatedType;
    for (size_t i = 0; i < segments.size(); ++i)
    {
        TypeId segmentTy = arena->addType(BlockedType{});
        module->astTypes[exprs[i]] = segmentTy;
        addConstraint(scope, expr->location, HasPropConstraint{segmentTy, prevSegmentTy, segments[i]});
        prevSegmentTy = segmentTy;
    }

    module->astTypes[expr] = prevSegmentTy;
    module->astTypes[e] = updatedType;

    symbolScope->bindings[sym].typeId = updatedType;

    std::optional<DefId> def = dfg->getDef(sym);
    if (def)
    {
        // This can fail if the user is erroneously trying to augment a builtin
        // table like os or string.
        symbolScope->dcrRefinements[*def] = updatedType;
    }

    return propTy;
}

Inference ConstraintGraphBuilder::check(const ScopePtr& scope, AstExprTable* expr, std::optional<TypeId> expectedType)
{
    TypeId ty = arena->addType(TableType{});
    TableType* ttv = getMutable<TableType>(ty);
    LUAU_ASSERT(ttv);

    ttv->state = TableState::Unsealed;
    ttv->scope = scope.get();

    auto createIndexer = [this, scope, ttv](const Location& location, TypeId currentIndexType, TypeId currentResultType) {
        if (!ttv->indexer)
        {
            TypeId indexType = this->freshType(scope);
            TypeId resultType = this->freshType(scope);
            ttv->indexer = TableIndexer{indexType, resultType};
        }

        addConstraint(scope, location, SubtypeConstraint{ttv->indexer->indexType, currentIndexType});
        addConstraint(scope, location, SubtypeConstraint{ttv->indexer->indexResultType, currentResultType});
    };

    std::optional<TypeId> annotatedKeyType;
    std::optional<TypeId> annotatedIndexResultType;

    if (expectedType)
    {
        if (const TableType* ttv = get<TableType>(follow(*expectedType)))
        {
            if (ttv->indexer)
            {
                annotatedKeyType.emplace(follow(ttv->indexer->indexType));
                annotatedIndexResultType.emplace(ttv->indexer->indexResultType);
            }
        }
    }

    bool isIndexedResultType = false;
    std::optional<TypeId> pinnedIndexResultType;


    for (const AstExprTable::Item& item : expr->items)
    {
        std::optional<TypeId> expectedValueType;
        if (item.kind == AstExprTable::Item::Kind::General || item.kind == AstExprTable::Item::Kind::List)
            isIndexedResultType = true;

        if (item.key && expectedType)
        {
            if (auto stringKey = item.key->as<AstExprConstantString>())
            {
                ErrorVec errorVec;
                std::optional<TypeId> propTy =
                    findTablePropertyRespectingMeta(builtinTypes, errorVec, follow(*expectedType), stringKey->value.data, item.value->location);
                if (propTy)
                    expectedValueType = propTy;
                else
                {
                    expectedValueType = arena->addType(BlockedType{});
                    addConstraint(scope, item.value->location, HasPropConstraint{*expectedValueType, *expectedType, stringKey->value.data});
                }
            }
        }


        // We'll resolve the expected index result type here with the following priority:
        // 1. Record table types - in which key, value pairs must be handled on a k,v pair basis.
        // In this case, the above if-statement will populate expectedValueType
        // 2. Someone places an annotation on a General or List table
        // Trust the annotation and have the solver inform them if they get it wrong
        // 3. Someone omits the annotation on a general or List table
        // Use the type of the first indexResultType as the expected type
        std::optional<TypeId> checkExpectedIndexResultType;
        if (expectedValueType)
        {
            checkExpectedIndexResultType = expectedValueType;
        }
        else if (annotatedIndexResultType)
        {
            checkExpectedIndexResultType = annotatedIndexResultType;
        }
        else if (pinnedIndexResultType)
        {
            checkExpectedIndexResultType = pinnedIndexResultType;
        }

        TypeId itemTy = check(scope, item.value, checkExpectedIndexResultType).ty;

        if (isIndexedResultType && !pinnedIndexResultType)
            pinnedIndexResultType = itemTy;

        if (item.key)
        {
            // Even though we don't need to use the type of the item's key if
            // it's a string constant, we still want to check it to populate
            // astTypes.
            TypeId keyTy = check(scope, item.key, annotatedKeyType).ty;

            if (AstExprConstantString* key = item.key->as<AstExprConstantString>())
            {
                ttv->props[key->value.begin()] = {itemTy};
            }
            else
            {
                createIndexer(item.key->location, keyTy, itemTy);
            }
        }
        else
        {
            TypeId numberType = builtinTypes->numberType;
            // FIXME?  The location isn't quite right here.  Not sure what is
            // right.
            createIndexer(item.value->location, numberType, itemTy);
        }
    }

    return Inference{ty};
}

ConstraintGraphBuilder::FunctionSignature ConstraintGraphBuilder::checkFunctionSignature(
    const ScopePtr& parent, AstExprFunction* fn, std::optional<TypeId> expectedType)
{
    ScopePtr signatureScope = nullptr;
    ScopePtr bodyScope = nullptr;
    TypePackId returnType = nullptr;

    std::vector<TypeId> genericTypes;
    std::vector<TypePackId> genericTypePacks;

    if (expectedType)
        expectedType = follow(*expectedType);

    bool hasGenerics = fn->generics.size > 0 || fn->genericPacks.size > 0;

    signatureScope = childScope(fn, parent);

    // We need to assign returnType before creating bodyScope so that the
    // return type gets propogated to bodyScope.
    returnType = freshTypePack(signatureScope);
    signatureScope->returnType = returnType;

    bodyScope = childScope(fn->body, signatureScope);

    if (hasGenerics)
    {
        std::vector<std::pair<Name, GenericTypeDefinition>> genericDefinitions = createGenerics(signatureScope, fn->generics);
        std::vector<std::pair<Name, GenericTypePackDefinition>> genericPackDefinitions = createGenericPacks(signatureScope, fn->genericPacks);

        // We do not support default values on function generics, so we only
        // care about the types involved.
        for (const auto& [name, g] : genericDefinitions)
        {
            genericTypes.push_back(g.ty);
            signatureScope->privateTypeBindings[name] = TypeFun{g.ty};
        }

        for (const auto& [name, g] : genericPackDefinitions)
        {
            genericTypePacks.push_back(g.tp);
            signatureScope->privateTypePackBindings[name] = g.tp;
        }

        // Local variable works around an odd gcc 11.3 warning: <anonymous> may be used uninitialized
        std::optional<TypeId> none = std::nullopt;
        expectedType = none;
    }

    std::vector<TypeId> argTypes;
    std::vector<std::optional<FunctionArgument>> argNames;
    TypePack expectedArgPack;

    const FunctionType* expectedFunction = expectedType ? get<FunctionType>(*expectedType) : nullptr;

    if (expectedFunction)
    {
        expectedArgPack = extendTypePack(*arena, builtinTypes, expectedFunction->argTypes, fn->args.size);

        genericTypes = expectedFunction->generics;
        genericTypePacks = expectedFunction->genericPacks;
    }

    if (fn->self)
    {
        TypeId selfType = freshType(signatureScope);
        argTypes.push_back(selfType);
        argNames.emplace_back(FunctionArgument{fn->self->name.value, fn->self->location});
        signatureScope->bindings[fn->self] = Binding{selfType, fn->self->location};
    }

    for (size_t i = 0; i < fn->args.size; ++i)
    {
        AstLocal* local = fn->args.data[i];

        TypeId t = freshType(signatureScope);
        argTypes.push_back(t);
        argNames.emplace_back(FunctionArgument{local->name.value, local->location});
        signatureScope->bindings[local] = Binding{t, local->location};

        auto def = dfg->getDef(local);
        LUAU_ASSERT(def);
        signatureScope->dcrRefinements[*def] = t;

        TypeId annotationTy = t;

        if (local->annotation)
        {
            annotationTy = resolveType(signatureScope, local->annotation, /* inTypeArguments */ false);
            // If we provide an annotation that is wrong, type inference should ignore the annotation
            // and try to infer a fresh type, like in the old solver
            if (get<ErrorType>(follow(annotationTy)))
                annotationTy = freshType(signatureScope);
            addConstraint(signatureScope, local->annotation->location, SubtypeConstraint{t, annotationTy});
        }
        else if (i < expectedArgPack.head.size())
        {
            addConstraint(signatureScope, local->location, SubtypeConstraint{t, expectedArgPack.head[i]});
        }
    }

    TypePackId varargPack = nullptr;

    if (fn->vararg)
    {
        if (fn->varargAnnotation)
        {
            TypePackId annotationType = resolveTypePack(signatureScope, fn->varargAnnotation, /* inTypeArguments */ false);
            varargPack = annotationType;
        }
        else if (expectedArgPack.tail && get<VariadicTypePack>(*expectedArgPack.tail))
            varargPack = *expectedArgPack.tail;
        else
            varargPack = builtinTypes->anyTypePack;

        signatureScope->varargPack = varargPack;
        bodyScope->varargPack = varargPack;
    }
    else
    {
        varargPack = arena->addTypePack(VariadicTypePack{builtinTypes->anyType, /*hidden*/ true});
        // We do not add to signatureScope->varargPack because ... is not valid
        // in functions without an explicit ellipsis.

        signatureScope->varargPack = std::nullopt;
        bodyScope->varargPack = std::nullopt;
    }

    LUAU_ASSERT(nullptr != varargPack);

    // If there is both an annotation and an expected type, the annotation wins.
    // Type checking will sort out any discrepancies later.
    if (fn->returnAnnotation)
    {
        TypePackId annotatedRetType = resolveTypePack(signatureScope, *fn->returnAnnotation, /* inTypeArguments */ false);

        // We bind the annotated type directly here so that, when we need to
        // generate constraints for return types, we have a guarantee that we
        // know the annotated return type already, if one was provided.
        LUAU_ASSERT(get<FreeTypePack>(returnType));
        asMutable(returnType)->ty.emplace<BoundTypePack>(annotatedRetType);
    }
    else if (expectedFunction)
    {
        asMutable(returnType)->ty.emplace<BoundTypePack>(expectedFunction->retTypes);
    }

    // TODO: Preserve argument names in the function's type.

    FunctionType actualFunction{TypeLevel{}, parent.get(), arena->addTypePack(argTypes, varargPack), returnType};
    actualFunction.hasNoGenerics = !hasGenerics;
    actualFunction.generics = std::move(genericTypes);
    actualFunction.genericPacks = std::move(genericTypePacks);
    actualFunction.argNames = std::move(argNames);

    TypeId actualFunctionType = arena->addType(std::move(actualFunction));
    LUAU_ASSERT(actualFunctionType);
    module->astTypes[fn] = actualFunctionType;

    if (expectedType && get<FreeType>(*expectedType))
    {
        asMutable(*expectedType)->ty.emplace<BoundType>(actualFunctionType);
    }

    return {
        /* signature */ actualFunctionType,
        /* signatureScope */ signatureScope,
        /* bodyScope */ bodyScope,
    };
}

void ConstraintGraphBuilder::checkFunctionBody(const ScopePtr& scope, AstExprFunction* fn)
{
    visitBlockWithoutChildScope(scope, fn->body);

    // If it is possible for execution to reach the end of the function, the return type must be compatible with ()

    if (nullptr != getFallthrough(fn->body))
    {
        TypePackId empty = arena->addTypePack({}); // TODO we could have CSG retain one of these forever
        addConstraint(scope, fn->location, PackSubtypeConstraint{scope->returnType, empty});
    }
}

TypeId ConstraintGraphBuilder::resolveType(const ScopePtr& scope, AstType* ty, bool inTypeArguments)
{
    TypeId result = nullptr;

    if (auto ref = ty->as<AstTypeReference>())
    {
        if (FFlag::DebugLuauMagicTypes)
        {
            if (ref->name == "_luau_ice")
                ice->ice("_luau_ice encountered", ty->location);
            else if (ref->name == "_luau_print")
            {
                if (ref->parameters.size != 1 || !ref->parameters.data[0].type)
                {
                    reportError(ty->location, GenericError{"_luau_print requires one generic parameter"});
                    return builtinTypes->errorRecoveryType();
                }
                else
                    return resolveType(scope, ref->parameters.data[0].type, inTypeArguments);
            }
        }

        std::optional<TypeFun> alias;

        if (ref->prefix.has_value())
        {
            alias = scope->lookupImportedType(ref->prefix->value, ref->name.value);
        }
        else
        {
            alias = scope->lookupType(ref->name.value);
        }

        if (alias.has_value())
        {
            // If the alias is not generic, we don't need to set up a blocked
            // type and an instantiation constraint.
            if (alias.has_value() && alias->typeParams.empty() && alias->typePackParams.empty())
            {
                result = alias->type;
            }
            else
            {
                std::vector<TypeId> parameters;
                std::vector<TypePackId> packParameters;

                for (const AstTypeOrPack& p : ref->parameters)
                {
                    // We do not enforce the ordering of types vs. type packs here;
                    // that is done in the parser.
                    if (p.type)
                    {
                        parameters.push_back(resolveType(scope, p.type, /* inTypeArguments */ true));
                    }
                    else if (p.typePack)
                    {
                        packParameters.push_back(resolveTypePack(scope, p.typePack, /* inTypeArguments */ true));
                    }
                    else
                    {
                        // This indicates a parser bug: one of these two pointers
                        // should be set.
                        LUAU_ASSERT(false);
                    }
                }

                result = arena->addType(PendingExpansionType{ref->prefix, ref->name, parameters, packParameters});

                // If we're not in a type argument context, we need to create a constraint that expands this.
                // The dispatching of the above constraint will queue up additional constraints for nested
                // type function applications.
                if (!inTypeArguments)
                    addConstraint(scope, ty->location, TypeAliasExpansionConstraint{/* target */ result});
            }
        }
        else
        {
            result = builtinTypes->errorRecoveryType();
        }
    }
    else if (auto tab = ty->as<AstTypeTable>())
    {
        TableType::Props props;
        std::optional<TableIndexer> indexer;

        for (const AstTableProp& prop : tab->props)
        {
            std::string name = prop.name.value;
            // TODO: Recursion limit.
            TypeId propTy = resolveType(scope, prop.type, inTypeArguments);
            // TODO: Fill in location.
            props[name] = {propTy};
        }

        if (tab->indexer)
        {
            // TODO: Recursion limit.
            indexer = TableIndexer{
                resolveType(scope, tab->indexer->indexType, inTypeArguments),
                resolveType(scope, tab->indexer->resultType, inTypeArguments),
            };
        }

        result = arena->addType(TableType{props, indexer, scope->level, scope.get(), TableState::Sealed});
    }
    else if (auto fn = ty->as<AstTypeFunction>())
    {
        // TODO: Recursion limit.
        bool hasGenerics = fn->generics.size > 0 || fn->genericPacks.size > 0;
        ScopePtr signatureScope = nullptr;

        std::vector<TypeId> genericTypes;
        std::vector<TypePackId> genericTypePacks;

        // If we don't have generics, we do not need to generate a child scope
        // for the generic bindings to live on.
        if (hasGenerics)
        {
            signatureScope = childScope(fn, scope);

            std::vector<std::pair<Name, GenericTypeDefinition>> genericDefinitions = createGenerics(signatureScope, fn->generics);
            std::vector<std::pair<Name, GenericTypePackDefinition>> genericPackDefinitions = createGenericPacks(signatureScope, fn->genericPacks);

            for (const auto& [name, g] : genericDefinitions)
            {
                genericTypes.push_back(g.ty);
                signatureScope->privateTypeBindings[name] = TypeFun{g.ty};
            }

            for (const auto& [name, g] : genericPackDefinitions)
            {
                genericTypePacks.push_back(g.tp);
                signatureScope->privateTypePackBindings[name] = g.tp;
            }
        }
        else
        {
            // To eliminate the need to branch on hasGenerics below, we say that
            // the signature scope is the parent scope if we don't have
            // generics.
            signatureScope = scope;
        }

        TypePackId argTypes = resolveTypePack(signatureScope, fn->argTypes, inTypeArguments);
        TypePackId returnTypes = resolveTypePack(signatureScope, fn->returnTypes, inTypeArguments);

        // TODO: FunctionType needs a pointer to the scope so that we know
        // how to quantify/instantiate it.
        FunctionType ftv{TypeLevel{}, scope.get(), {}, {}, argTypes, returnTypes};

        // This replicates the behavior of the appropriate FunctionType
        // constructors.
        ftv.hasNoGenerics = !hasGenerics;
        ftv.generics = std::move(genericTypes);
        ftv.genericPacks = std::move(genericTypePacks);

        ftv.argNames.reserve(fn->argNames.size);
        for (const auto& el : fn->argNames)
        {
            if (el)
            {
                const auto& [name, location] = *el;
                ftv.argNames.push_back(FunctionArgument{name.value, location});
            }
            else
            {
                ftv.argNames.push_back(std::nullopt);
            }
        }

        result = arena->addType(std::move(ftv));
    }
    else if (auto tof = ty->as<AstTypeTypeof>())
    {
        // TODO: Recursion limit.
        TypeId exprType = check(scope, tof->expr).ty;
        result = exprType;
    }
    else if (auto unionAnnotation = ty->as<AstTypeUnion>())
    {
        std::vector<TypeId> parts;
        for (AstType* part : unionAnnotation->types)
        {
            // TODO: Recursion limit.
            parts.push_back(resolveType(scope, part, inTypeArguments));
        }

        result = arena->addType(UnionType{parts});
    }
    else if (auto intersectionAnnotation = ty->as<AstTypeIntersection>())
    {
        std::vector<TypeId> parts;
        for (AstType* part : intersectionAnnotation->types)
        {
            // TODO: Recursion limit.
            parts.push_back(resolveType(scope, part, inTypeArguments));
        }

        result = arena->addType(IntersectionType{parts});
    }
    else if (auto boolAnnotation = ty->as<AstTypeSingletonBool>())
    {
        result = arena->addType(SingletonType(BooleanSingleton{boolAnnotation->value}));
    }
    else if (auto stringAnnotation = ty->as<AstTypeSingletonString>())
    {
        result = arena->addType(SingletonType(StringSingleton{std::string(stringAnnotation->value.data, stringAnnotation->value.size)}));
    }
    else if (ty->is<AstTypeError>())
    {
        result = builtinTypes->errorRecoveryType();
    }
    else
    {
        LUAU_ASSERT(0);
        result = builtinTypes->errorRecoveryType();
    }

    module->astResolvedTypes[ty] = result;
    return result;
}

TypePackId ConstraintGraphBuilder::resolveTypePack(const ScopePtr& scope, AstTypePack* tp, bool inTypeArgument)
{
    TypePackId result;
    if (auto expl = tp->as<AstTypePackExplicit>())
    {
        result = resolveTypePack(scope, expl->typeList, inTypeArgument);
    }
    else if (auto var = tp->as<AstTypePackVariadic>())
    {
        TypeId ty = resolveType(scope, var->variadicType, inTypeArgument);
        if (get<ErrorType>(follow(ty)))
            ty = freshType(scope);
        result = arena->addTypePack(TypePackVar{VariadicTypePack{ty}});
    }
    else if (auto gen = tp->as<AstTypePackGeneric>())
    {
        if (std::optional<TypePackId> lookup = scope->lookupPack(gen->genericName.value))
        {
            result = *lookup;
        }
        else
        {
            reportError(tp->location, UnknownSymbol{gen->genericName.value, UnknownSymbol::Context::Type});
            result = builtinTypes->errorRecoveryTypePack();
        }
    }
    else
    {
        LUAU_ASSERT(0);
        result = builtinTypes->errorRecoveryTypePack();
    }

    module->astResolvedTypePacks[tp] = result;
    return result;
}

TypePackId ConstraintGraphBuilder::resolveTypePack(const ScopePtr& scope, const AstTypeList& list, bool inTypeArguments)
{
    std::vector<TypeId> head;

    for (AstType* headTy : list.types)
    {
        head.push_back(resolveType(scope, headTy, inTypeArguments));
    }

    std::optional<TypePackId> tail = std::nullopt;
    if (list.tailType)
    {
        tail = resolveTypePack(scope, list.tailType, inTypeArguments);
    }

    return arena->addTypePack(TypePack{head, tail});
}

std::vector<std::pair<Name, GenericTypeDefinition>> ConstraintGraphBuilder::createGenerics(
    const ScopePtr& scope, AstArray<AstGenericType> generics, bool useCache)
{
    std::vector<std::pair<Name, GenericTypeDefinition>> result;
    for (const auto& generic : generics)
    {
        TypeId genericTy = nullptr;

        if (auto it = scope->parent->typeAliasTypeParameters.find(generic.name.value); useCache && it != scope->parent->typeAliasTypeParameters.end())
            genericTy = it->second;
        else
        {
            genericTy = arena->addType(GenericType{scope.get(), generic.name.value});
            scope->parent->typeAliasTypeParameters[generic.name.value] = genericTy;
        }

        std::optional<TypeId> defaultTy = std::nullopt;

        if (generic.defaultValue)
            defaultTy = resolveType(scope, generic.defaultValue, /* inTypeArguments */ false);

        result.push_back({generic.name.value, GenericTypeDefinition{genericTy, defaultTy}});
    }

    return result;
}

std::vector<std::pair<Name, GenericTypePackDefinition>> ConstraintGraphBuilder::createGenericPacks(
    const ScopePtr& scope, AstArray<AstGenericTypePack> generics, bool useCache)
{
    std::vector<std::pair<Name, GenericTypePackDefinition>> result;
    for (const auto& generic : generics)
    {
        TypePackId genericTy;

        if (auto it = scope->parent->typeAliasTypePackParameters.find(generic.name.value);
            useCache && it != scope->parent->typeAliasTypePackParameters.end())
            genericTy = it->second;
        else
        {
            genericTy = arena->addTypePack(TypePackVar{GenericTypePack{scope.get(), generic.name.value}});
            scope->parent->typeAliasTypePackParameters[generic.name.value] = genericTy;
        }

        std::optional<TypePackId> defaultTy = std::nullopt;

        if (generic.defaultValue)
            defaultTy = resolveTypePack(scope, generic.defaultValue, /* inTypeArguments */ false);

        result.push_back({generic.name.value, GenericTypePackDefinition{genericTy, defaultTy}});
    }

    return result;
}

Inference ConstraintGraphBuilder::flattenPack(const ScopePtr& scope, Location location, InferencePack pack)
{
    const auto& [tp, refinements] = pack;
    RefinementId refinement = nullptr;
    if (!refinements.empty())
        refinement = refinements[0];

    if (auto f = first(tp))
        return Inference{*f, refinement};

    TypeId typeResult = freshType(scope);
    TypePack onePack{{typeResult}, freshTypePack(scope)};
    TypePackId oneTypePack = arena->addTypePack(std::move(onePack));

    addConstraint(scope, location, PackSubtypeConstraint{tp, oneTypePack});

    return Inference{typeResult, refinement};
}

void ConstraintGraphBuilder::reportError(Location location, TypeErrorData err)
{
    errors.push_back(TypeError{location, moduleName, std::move(err)});

    if (FFlag::DebugLuauLogSolverToJson)
        logger->captureGenerationError(errors.back());
}

void ConstraintGraphBuilder::reportCodeTooComplex(Location location)
{
    errors.push_back(TypeError{location, moduleName, CodeTooComplex{}});

    if (FFlag::DebugLuauLogSolverToJson)
        logger->captureGenerationError(errors.back());
}

struct GlobalPrepopulator : AstVisitor
{
    const NotNull<Scope> globalScope;
    const NotNull<TypeArena> arena;

    GlobalPrepopulator(NotNull<Scope> globalScope, NotNull<TypeArena> arena)
        : globalScope(globalScope)
        , arena(arena)
    {
    }

    bool visit(AstStatFunction* function) override
    {
        if (AstExprGlobal* g = function->name->as<AstExprGlobal>())
            globalScope->bindings[g->name] = Binding{arena->addType(BlockedType{})};

        return true;
    }
};

void ConstraintGraphBuilder::prepopulateGlobalScope(const ScopePtr& globalScope, AstStatBlock* program)
{
    GlobalPrepopulator gp{NotNull{globalScope.get()}, arena};

    program->visit(&gp);
}

std::vector<NotNull<Constraint>> borrowConstraints(const std::vector<ConstraintPtr>& constraints)
{
    std::vector<NotNull<Constraint>> result;
    result.reserve(constraints.size());

    for (const auto& c : constraints)
        result.emplace_back(c.get());

    return result;
}

} // namespace Luau
