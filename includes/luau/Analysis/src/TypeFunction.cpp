// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details

#include "Luau/TypeFunction.h"

#include "Luau/BytecodeBuilder.h"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include "Luau/ConstraintSolver.h"
#include "Luau/DenseHash.h"
#include "Luau/Instantiation.h"
#include "Luau/Normalize.h"
#include "Luau/NotNull.h"
#include "Luau/OverloadResolution.h"
#include "Luau/Set.h"
#include "Luau/Simplify.h"
#include "Luau/Subtyping.h"
#include "Luau/TimeTrace.h"
#include "Luau/ToString.h"
#include "Luau/TxnLog.h"
#include "Luau/Type.h"
#include "Luau/TypeChecker2.h"
#include "Luau/TypeFunctionReductionGuesser.h"
#include "Luau/TypeFunctionRuntime.h"
#include "Luau/TypeFunctionRuntimeBuilder.h"
#include "Luau/TypeFwd.h"
#include "Luau/TypeUtils.h"
#include "Luau/Unifier2.h"
#include "Luau/VecDeque.h"
#include "Luau/VisitType.h"
#include "Luau/ApplyTypeFunction.h"

#include "lua.h"
#include "lualib.h"

#include <iterator>
#include <memory>
#include <unordered_map>

// used to control emitting CodeTooComplex warnings on type function reduction
LUAU_DYNAMIC_FASTINTVARIABLE(LuauTypeFamilyGraphReductionMaximumSteps, 1'000'000);

// used to control the limits of type function application over union type arguments
// e.g. `mul<a | b, c | d>` blows up into `mul<a, c> | mul<a, d> | mul<b, c> | mul<b, d>`
LUAU_DYNAMIC_FASTINTVARIABLE(LuauTypeFamilyApplicationCartesianProductLimit, 5'000);

// used to control falling back to a more conservative reduction based on guessing
// when this value is set to a negative value, guessing will be totally disabled.
LUAU_DYNAMIC_FASTINTVARIABLE(LuauTypeFamilyUseGuesserDepth, -1);

LUAU_FASTFLAG(DebugLuauEqSatSimplification)
LUAU_FASTFLAG(LuauEagerGeneralization4)
LUAU_FASTFLAG(LuauEagerGeneralization4)

LUAU_FASTFLAGVARIABLE(DebugLuauLogTypeFamilies)
LUAU_FASTFLAGVARIABLE(LuauNotAllBinaryTypeFunsHaveDefaults)
LUAU_FASTFLAG(LuauUserTypeFunctionAliases)
LUAU_FASTFLAG(LuauUpdateGetMetatableTypeSignature)
LUAU_FASTFLAG(LuauRemoveTypeCallsForReadWriteProps)
LUAU_FASTFLAGVARIABLE(LuauOccursCheckForRefinement)
LUAU_FASTFLAGVARIABLE(LuauStuckTypeFunctionsStillDispatch)
LUAU_FASTFLAG(LuauRefineTablesWithReadType)
LUAU_FASTFLAGVARIABLE(LuauEmptyStringInKeyOf)
LUAU_FASTFLAGVARIABLE(LuauAvoidExcessiveTypeCopying)

namespace Luau
{

using TypeOrTypePackIdSet = DenseHashSet<const void*>;

struct InstanceCollector : TypeOnceVisitor
{
    DenseHashSet<TypeId> recordedTys{nullptr};
    VecDeque<TypeId> tys;
    DenseHashSet<TypePackId> recordedTps{nullptr};
    VecDeque<TypePackId> tps;
    TypeOrTypePackIdSet shouldGuess{nullptr};
    std::vector<const void*> typeFunctionInstanceStack;
    std::vector<TypeId> cyclicInstance;

    bool visit(TypeId ty, const TypeFunctionInstanceType& tfit) override
    {
        // TypeVisitor performs a depth-first traversal in the absence of
        // cycles. This means that by pushing to the front of the queue, we will
        // try to reduce deeper instances first if we start with the first thing
        // in the queue. Consider Add<Add<Add<number, number>, number>, number>:
        // we want to reduce the innermost Add<number, number> instantiation
        // first.

        typeFunctionInstanceStack.push_back(ty);

        if (DFInt::LuauTypeFamilyUseGuesserDepth >= 0 && int(typeFunctionInstanceStack.size()) > DFInt::LuauTypeFamilyUseGuesserDepth)
            shouldGuess.insert(ty);

        if (!recordedTys.contains(ty))
        {
            recordedTys.insert(ty);
            tys.push_front(ty);
        }

        for (TypeId p : tfit.typeArguments)
            traverse(p);

        for (TypePackId p : tfit.packArguments)
            traverse(p);

        typeFunctionInstanceStack.pop_back();

        return false;
    }

    void cycle(TypeId ty) override
    {
        TypeId t = follow(ty);

        if (get<TypeFunctionInstanceType>(t))
        {
            // If we see a type a second time and it's in the type function stack, it's a real cycle
            if (std::find(typeFunctionInstanceStack.begin(), typeFunctionInstanceStack.end(), t) != typeFunctionInstanceStack.end())
                cyclicInstance.push_back(t);
        }
    }

    bool visit(TypeId ty, const ExternType&) override
    {
        return false;
    }

    bool visit(TypePackId tp, const TypeFunctionInstanceTypePack& tfitp) override
    {
        // TypeVisitor performs a depth-first traversal in the absence of
        // cycles. This means that by pushing to the front of the queue, we will
        // try to reduce deeper instances first if we start with the first thing
        // in the queue. Consider Add<Add<Add<number, number>, number>, number>:
        // we want to reduce the innermost Add<number, number> instantiation
        // first.

        typeFunctionInstanceStack.push_back(tp);

        if (DFInt::LuauTypeFamilyUseGuesserDepth >= 0 && int(typeFunctionInstanceStack.size()) > DFInt::LuauTypeFamilyUseGuesserDepth)
            shouldGuess.insert(tp);

        if (!recordedTps.contains(tp))
        {
            recordedTps.insert(tp);
            tps.push_front(tp);
        }

        for (TypeId p : tfitp.typeArguments)
            traverse(p);

        for (TypePackId p : tfitp.packArguments)
            traverse(p);

        typeFunctionInstanceStack.pop_back();

        return false;
    }
};

struct UnscopedGenericFinder : TypeOnceVisitor
{
    std::vector<TypeId> scopeGenTys;
    std::vector<TypePackId> scopeGenTps;
    bool foundUnscoped = false;

    bool visit(TypeId ty) override
    {
        // Once we have found an unscoped generic, we will stop the traversal
        return !foundUnscoped;
    }

    bool visit(TypePackId tp) override
    {
        // Once we have found an unscoped generic, we will stop the traversal
        return !foundUnscoped;
    }

    bool visit(TypeId ty, const GenericType&) override
    {
        if (std::find(scopeGenTys.begin(), scopeGenTys.end(), ty) == scopeGenTys.end())
            foundUnscoped = true;

        return false;
    }

    bool visit(TypePackId tp, const GenericTypePack&) override
    {
        if (std::find(scopeGenTps.begin(), scopeGenTps.end(), tp) == scopeGenTps.end())
            foundUnscoped = true;

        return false;
    }

    bool visit(TypeId ty, const FunctionType& ftv) override
    {
        size_t startTyCount = scopeGenTys.size();
        size_t startTpCount = scopeGenTps.size();

        scopeGenTys.insert(scopeGenTys.end(), ftv.generics.begin(), ftv.generics.end());
        scopeGenTps.insert(scopeGenTps.end(), ftv.genericPacks.begin(), ftv.genericPacks.end());

        traverse(ftv.argTypes);
        traverse(ftv.retTypes);

        scopeGenTys.resize(startTyCount);
        scopeGenTps.resize(startTpCount);

        return false;
    }

    bool visit(TypeId ty, const ExternType&) override
    {
        return false;
    }
};

struct TypeFunctionReducer
{
    TypeFunctionContext ctx;

    VecDeque<TypeId> queuedTys;
    VecDeque<TypePackId> queuedTps;
    TypeOrTypePackIdSet shouldGuess;
    std::vector<TypeId> cyclicTypeFunctions;
    TypeOrTypePackIdSet irreducible{nullptr};
    FunctionGraphReductionResult result;
    bool force = false;

    // Local to the constraint being reduced.
    Location location;

    TypeFunctionReducer(
        VecDeque<TypeId> queuedTys,
        VecDeque<TypePackId> queuedTps,
        TypeOrTypePackIdSet shouldGuess,
        std::vector<TypeId> cyclicTypes,
        Location location,
        TypeFunctionContext ctx,
        bool force = false
    )
        : ctx(ctx)
        , queuedTys(std::move(queuedTys))
        , queuedTps(std::move(queuedTps))
        , shouldGuess(std::move(shouldGuess))
        , cyclicTypeFunctions(std::move(cyclicTypes))
        , force(force)
        , location(location)
    {
    }

    enum class SkipTestResult
    {
        /// If a type function is cyclic, it cannot be reduced, but maybe we can
        /// make a guess and offer a suggested annotation to the user.
        CyclicTypeFunction,

        /// Indicase that we will not be able to reduce this type function this
        /// time. Constraint resolution may cause this type function to become
        /// reducible later.
        Irreducible,

        /// A type function that cannot be reduced any further because it has no valid reduction.
        /// eg add<number, string>
        Stuck,

        /// Some type functions can operate on generic parameters
        Generic,

        /// We might be able to reduce this type function, but not yet.
        Defer,

        /// We can attempt to reduce this type function right now.
        Okay,
    };

    SkipTestResult DEPRECATED_testForSkippability(TypeId ty)
    {
        ty = follow(ty);

        if (is<TypeFunctionInstanceType>(ty))
        {
            for (auto t : cyclicTypeFunctions)
            {
                if (ty == t)
                    return SkipTestResult::CyclicTypeFunction;
            }

            if (!irreducible.contains(ty))
                return SkipTestResult::Defer;

            return SkipTestResult::Irreducible;
        }
        else if (is<GenericType>(ty))
        {
            if (FFlag::LuauEagerGeneralization4)
                return SkipTestResult::Generic;
            else
                return SkipTestResult::Irreducible;
        }

        return SkipTestResult::Okay;
    }

    SkipTestResult testForSkippability(TypeId ty)
    {
        if (!FFlag::LuauEagerGeneralization4)
            return DEPRECATED_testForSkippability(ty);

        VecDeque<TypeId> queue;
        DenseHashSet<TypeId> seen{nullptr};

        queue.push_back(follow(ty));

        while (!queue.empty())
        {
            TypeId t = queue.front();
            queue.pop_front();

            if (seen.contains(t))
                continue;

            if (auto tfit = get<TypeFunctionInstanceType>(t))
            {
                if (FFlag::LuauStuckTypeFunctionsStillDispatch)
                {
                    if (tfit->state == TypeFunctionInstanceState::Stuck)
                        return SkipTestResult::Stuck;
                    else if (tfit->state == TypeFunctionInstanceState::Solved)
                        return SkipTestResult::Generic;
                }
                for (auto cyclicTy : cyclicTypeFunctions)
                {
                    if (t == cyclicTy)
                        return SkipTestResult::CyclicTypeFunction;
                }

                if (!irreducible.contains(t))
                    return SkipTestResult::Defer;

                return SkipTestResult::Irreducible;
            }
            else if (is<GenericType>(t))
                return SkipTestResult::Generic;
            else if (auto it = get<IntersectionType>(t))
            {
                for (TypeId part : it->parts)
                    queue.push_back(follow(part));
            }

            seen.insert(t);
        }

        return SkipTestResult::Okay;
    }

    SkipTestResult testForSkippability(TypePackId ty) const
    {
        ty = follow(ty);

        if (is<TypeFunctionInstanceTypePack>(ty))
        {
            if (!irreducible.contains(ty))
                return SkipTestResult::Defer;
            else
                return SkipTestResult::Irreducible;
        }
        else if (is<GenericTypePack>(ty))
        {
            if (FFlag::LuauEagerGeneralization4)
                return SkipTestResult::Generic;
            else
                return SkipTestResult::Irreducible;
        }

        return SkipTestResult::Okay;
    }

    template<typename T>
    void replace(T subject, T replacement)
    {
        if (subject->owningArena != ctx.arena.get())
        {
            result.errors.emplace_back(location, InternalError{"Attempting to modify a type function instance from another arena"});
            return;
        }

        if (FFlag::DebugLuauLogTypeFamilies)
            printf("%s => %s\n", toString(subject, {true}).c_str(), toString(replacement, {true}).c_str());

        asMutable(subject)->ty.template emplace<Unifiable::Bound<T>>(replacement);

        if constexpr (std::is_same_v<T, TypeId>)
            result.reducedTypes.insert(subject);
        else if constexpr (std::is_same_v<T, TypePackId>)
            result.reducedPacks.insert(subject);
    }

    TypeFunctionInstanceState getState(TypeId ty) const
    {
        auto tfit = get<TypeFunctionInstanceType>(ty);
        LUAU_ASSERT(tfit);
        return tfit->state;
    }

    void setState(TypeId ty, TypeFunctionInstanceState state) const
    {
        if (ty->owningArena != ctx.arena)
            return;

        TypeFunctionInstanceType* tfit = getMutable<TypeFunctionInstanceType>(ty);
        LUAU_ASSERT(tfit);
        tfit->state = state;
    }

    TypeFunctionInstanceState getState(TypePackId tp) const
    {
        return TypeFunctionInstanceState::Unsolved;
    }

    void setState(TypePackId tp, TypeFunctionInstanceState state) const
    {
        // We do not presently have any type pack functions at all.
        (void)tp;
        (void)state;
    }

    template<typename T>
    void handleTypeFunctionReduction(T subject, TypeFunctionReductionResult<T> reduction)
    {
        for (auto& message : reduction.messages)
            result.messages.emplace_back(location, UserDefinedTypeFunctionError{std::move(message)});

        if (reduction.result)
            replace(subject, *reduction.result);
        else
        {
            irreducible.insert(subject);

            if (reduction.error.has_value())
                result.errors.emplace_back(location, UserDefinedTypeFunctionError{*reduction.error});

            if (reduction.reductionStatus != Reduction::MaybeOk || force)
            {
                if (FFlag::DebugLuauLogTypeFamilies)
                    printf("%s is uninhabited\n", toString(subject, {true}).c_str());

                if (FFlag::LuauStuckTypeFunctionsStillDispatch)
                {
                    if (getState(subject) == TypeFunctionInstanceState::Unsolved)
                    {
                        if (reduction.reductionStatus == Reduction::Erroneous)
                            setState(subject, TypeFunctionInstanceState::Stuck);
                        else if (reduction.reductionStatus == Reduction::Irreducible)
                            setState(subject, TypeFunctionInstanceState::Solved);
                        else if (reduction.reductionStatus == Reduction::MaybeOk)
                        {
                            // We cannot make progress because something is unsolved, but we're also forcing.
                            setState(subject, TypeFunctionInstanceState::Stuck);
                        }
                        else
                            ctx.ice->ice("Unexpected TypeFunctionInstanceState");
                    }
                }

                if constexpr (std::is_same_v<T, TypeId>)
                    result.errors.emplace_back(location, UninhabitedTypeFunction{subject});
                else if constexpr (std::is_same_v<T, TypePackId>)
                    result.errors.emplace_back(location, UninhabitedTypePackFunction{subject});
            }
            else if (reduction.reductionStatus == Reduction::MaybeOk && !force)
            {
                // We're not forcing and the reduction couldn't proceed, but it isn't obviously busted.
                // Report that this type blocks further reduction.

                if (FFlag::DebugLuauLogTypeFamilies)
                    printf(
                        "%s is irreducible; blocked on %zu types, %zu packs\n",
                        toString(subject, {true}).c_str(),
                        reduction.blockedTypes.size(),
                        reduction.blockedPacks.size()
                    );

                for (TypeId b : reduction.blockedTypes)
                    result.blockedTypes.insert(b);

                for (TypePackId b : reduction.blockedPacks)
                    result.blockedPacks.insert(b);
            }
            else
                LUAU_ASSERT(!"Unreachable");
        }
    }

    bool done() const
    {
        return queuedTys.empty() && queuedTps.empty();
    }

    template<typename T, typename I>
    bool testParameters(T subject, const I* tfit)
    {
        for (TypeId p : tfit->typeArguments)
        {
            SkipTestResult skip = testForSkippability(p);

            if (skip == SkipTestResult::Stuck)
            {
                // SkipTestResult::Stuck cannot happen when this flag is unset.
                LUAU_ASSERT(FFlag::LuauStuckTypeFunctionsStillDispatch);
                if (FFlag::DebugLuauLogTypeFamilies)
                    printf("%s is stuck!\n", toString(subject, {true}).c_str());

                irreducible.insert(subject);
                setState(subject, TypeFunctionInstanceState::Stuck);

                return false;
            }
            if (skip == SkipTestResult::Irreducible || (skip == SkipTestResult::Generic && !tfit->function->canReduceGenerics))
            {
                if (FFlag::DebugLuauLogTypeFamilies)
                {
                    if (skip == SkipTestResult::Generic)
                        printf("%s is solved due to a dependency on %s\n", toString(subject, {true}).c_str(), toString(p, {true}).c_str());
                    else
                        printf("%s is irreducible due to a dependency on %s\n", toString(subject, {true}).c_str(), toString(p, {true}).c_str());
                }

                irreducible.insert(subject);

                if (skip == SkipTestResult::Generic)
                    setState(subject, TypeFunctionInstanceState::Solved);

                return false;
            }
            else if (skip == SkipTestResult::Defer)
            {
                if (FFlag::DebugLuauLogTypeFamilies)
                    printf("Deferring %s until %s is solved\n", toString(subject, {true}).c_str(), toString(p, {true}).c_str());

                if constexpr (std::is_same_v<T, TypeId>)
                    queuedTys.push_back(subject);
                else if constexpr (std::is_same_v<T, TypePackId>)
                    queuedTps.push_back(subject);

                return false;
            }
        }

        for (TypePackId p : tfit->packArguments)
        {
            SkipTestResult skip = testForSkippability(p);

            if (skip == SkipTestResult::Irreducible || (skip == SkipTestResult::Generic && !tfit->function->canReduceGenerics))
            {
                if (FFlag::DebugLuauLogTypeFamilies)
                    printf("%s is irreducible due to a dependency on %s\n", toString(subject, {true}).c_str(), toString(p, {true}).c_str());

                irreducible.insert(subject);
                return false;
            }
            else if (skip == SkipTestResult::Defer)
            {
                if (FFlag::DebugLuauLogTypeFamilies)
                    printf("Deferring %s until %s is solved\n", toString(subject, {true}).c_str(), toString(p, {true}).c_str());

                if constexpr (std::is_same_v<T, TypeId>)
                    queuedTys.push_back(subject);
                else if constexpr (std::is_same_v<T, TypePackId>)
                    queuedTps.push_back(subject);

                return false;
            }
        }

        return true;
    }

    template<typename TID>
    inline bool tryGuessing(TID subject)
    {
        if (shouldGuess.contains(subject))
        {
            if (FFlag::DebugLuauLogTypeFamilies)
                printf("Flagged %s for reduction with guesser.\n", toString(subject, {true}).c_str());

            TypeFunctionReductionGuesser guesser{ctx.arena, ctx.builtins, ctx.normalizer};
            auto guessed = guesser.guess(subject);

            if (guessed)
            {
                if (FFlag::DebugLuauLogTypeFamilies)
                    printf("Selected %s as the guessed result type.\n", toString(*guessed, {true}).c_str());

                replace(subject, *guessed);
                return true;
            }

            if (FFlag::DebugLuauLogTypeFamilies)
                printf("Failed to produce a guess for the result of %s.\n", toString(subject, {true}).c_str());
        }

        return false;
    }

    void stepType()
    {
        TypeId subject = follow(queuedTys.front());
        queuedTys.pop_front();

        if (irreducible.contains(subject))
            return;

        if (FFlag::DebugLuauLogTypeFamilies)
            printf("Trying to %sreduce %s\n", force ? "force " : "", toString(subject, {true}).c_str());

        if (const TypeFunctionInstanceType* tfit = get<TypeFunctionInstanceType>(subject))
        {
            if (tfit->function->name == "user")
            {
                UnscopedGenericFinder finder;
                finder.traverse(subject);

                if (finder.foundUnscoped)
                {
                    // Do not step into this type again
                    irreducible.insert(subject);

                    // Let the caller know this type will not become reducible
                    result.irreducibleTypes.insert(subject);

                    if (FFlag::DebugLuauLogTypeFamilies)
                        printf("Irreducible due to an unscoped generic type\n");

                    return;
                }
            }

            SkipTestResult testCyclic = testForSkippability(subject);

            if (!testParameters(subject, tfit) && testCyclic != SkipTestResult::CyclicTypeFunction)
            {
                if (FFlag::DebugLuauLogTypeFamilies)
                    printf("Irreducible due to irreducible/pending and a non-cyclic function\n");

                if (tfit->state == TypeFunctionInstanceState::Stuck || tfit->state == TypeFunctionInstanceState::Solved)
                    tryGuessing(subject);

                return;
            }

            if (tryGuessing(subject))
                return;

            ctx.userFuncName = tfit->userFuncName;

            TypeFunctionReductionResult<TypeId> result = tfit->function->reducer(subject, tfit->typeArguments, tfit->packArguments, NotNull{&ctx});
            handleTypeFunctionReduction(subject, std::move(result));
        }
    }

    void stepPack()
    {
        TypePackId subject = follow(queuedTps.front());
        queuedTps.pop_front();

        if (irreducible.contains(subject))
            return;

        if (FFlag::DebugLuauLogTypeFamilies)
            printf("Trying to reduce %s\n", toString(subject, {true}).c_str());

        if (const TypeFunctionInstanceTypePack* tfit = get<TypeFunctionInstanceTypePack>(subject))
        {
            if (!testParameters(subject, tfit))
                return;

            if (tryGuessing(subject))
                return;

            TypeFunctionReductionResult<TypePackId> result =
                tfit->function->reducer(subject, tfit->typeArguments, tfit->packArguments, NotNull{&ctx});
            handleTypeFunctionReduction(subject, std::move(result));
        }
    }

    void step()
    {
        if (!queuedTys.empty())
            stepType();
        else if (!queuedTps.empty())
            stepPack();
    }
};

struct LuauTempThreadPopper
{
    explicit LuauTempThreadPopper(lua_State* L)
        : L(L)
    {
    }
    ~LuauTempThreadPopper()
    {
        lua_pop(L, 1);
    }

    lua_State* L = nullptr;
};

template<typename T>
class ScopedAssign
{
public:
    ScopedAssign(T& target, const T& value)
        : target(&target)
        , oldValue(target)
    {
        target = value;
    }

    ~ScopedAssign()
    {
        *target = oldValue;
    }

private:
    T* target = nullptr;
    T oldValue;
};

static FunctionGraphReductionResult reduceFunctionsInternal(
    VecDeque<TypeId> queuedTys,
    VecDeque<TypePackId> queuedTps,
    TypeOrTypePackIdSet shouldGuess,
    std::vector<TypeId> cyclics,
    Location location,
    TypeFunctionContext ctx,
    bool force
)
{
    TypeFunctionReducer reducer{std::move(queuedTys), std::move(queuedTps), std::move(shouldGuess), std::move(cyclics), location, ctx, force};
    int iterationCount = 0;

    // If we are reducing a type function while reducing a type function,
    // we're probably doing something clowny. One known place this can
    // occur is type function reduction => overload selection => subtyping
    // => back to type function reduction. At worst, if there's a reduction
    // that _doesn't_ loop forever and _needs_ reentrancy, we'll fail to
    // handle that and potentially emit an error when we didn't need to.
    if (ctx.normalizer->sharedState->reentrantTypeReduction)
        return {};

    TypeReductionRentrancyGuard _{ctx.normalizer->sharedState};
    while (!reducer.done())
    {
        reducer.step();

        ++iterationCount;
        if (iterationCount > DFInt::LuauTypeFamilyGraphReductionMaximumSteps)
        {
            reducer.result.errors.emplace_back(location, CodeTooComplex{});
            break;
        }
    }

    return std::move(reducer.result);
}

FunctionGraphReductionResult reduceTypeFunctions(TypeId entrypoint, Location location, TypeFunctionContext ctx, bool force)
{
    InstanceCollector collector;

    try
    {
        collector.traverse(entrypoint);
    }
    catch (RecursionLimitException&)
    {
        return FunctionGraphReductionResult{};
    }

    if (collector.tys.empty() && collector.tps.empty())
        return {};

    return reduceFunctionsInternal(
        std::move(collector.tys),
        std::move(collector.tps),
        std::move(collector.shouldGuess),
        std::move(collector.cyclicInstance),
        location,
        ctx,
        force
    );
}

FunctionGraphReductionResult reduceTypeFunctions(TypePackId entrypoint, Location location, TypeFunctionContext ctx, bool force)
{
    InstanceCollector collector;

    try
    {
        collector.traverse(entrypoint);
    }
    catch (RecursionLimitException&)
    {
        return FunctionGraphReductionResult{};
    }

    if (collector.tys.empty() && collector.tps.empty())
        return {};

    return reduceFunctionsInternal(
        std::move(collector.tys),
        std::move(collector.tps),
        std::move(collector.shouldGuess),
        std::move(collector.cyclicInstance),
        location,
        ctx,
        force
    );
}

bool isPending(TypeId ty, ConstraintSolver* solver)
{
    if (FFlag::LuauStuckTypeFunctionsStillDispatch)
    {
        if (auto tfit = get<TypeFunctionInstanceType>(ty); tfit && tfit->state == TypeFunctionInstanceState::Unsolved)
            return true;
        return is<BlockedType, PendingExpansionType>(ty) || (solver && solver->hasUnresolvedConstraints(ty));
    }
    else
        return is<BlockedType, PendingExpansionType, TypeFunctionInstanceType>(ty) || (solver && solver->hasUnresolvedConstraints(ty));
}

template<typename F, typename... Args>
static std::optional<TypeFunctionReductionResult<TypeId>> tryDistributeTypeFunctionApp(
    F f,
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx,
    Args&&... args
)
{
    // op (a | b) (c | d) ~ (op a (c | d)) | (op b (c | d)) ~ (op a c) | (op a d) | (op b c) | (op b d)
    Reduction reductionStatus = Reduction::MaybeOk;
    std::vector<TypeId> blockedTypes;
    std::vector<TypeId> results;
    size_t cartesianProductSize = 1;

    const UnionType* firstUnion = nullptr;
    size_t unionIndex = 0;

    std::vector<TypeId> arguments = typeParams;
    for (size_t i = 0; i < arguments.size(); ++i)
    {
        const UnionType* ut = get<UnionType>(follow(arguments[i]));
        if (!ut)
            continue;

        // We want to find the first union type in the set of arguments to distribute that one and only that one union.
        // The function `f` we have is recursive, so `arguments[unionIndex]` will be updated in-place for each option in
        // the union we've found in this context, so that index will no longer be a union type. Any other arguments at
        // index + 1 or after will instead be distributed, if those are a union, which will be subjected to the same rules.
        if (!firstUnion && ut)
        {
            firstUnion = ut;
            unionIndex = i;
        }

        cartesianProductSize *= std::distance(begin(ut), end(ut));

        // TODO: We'd like to report that the type function application is too complex here.
        if (size_t(DFInt::LuauTypeFamilyApplicationCartesianProductLimit) <= cartesianProductSize)
            return {{std::nullopt, Reduction::Erroneous, {}, {}}};
    }

    if (!firstUnion)
    {
        // If we couldn't find any union type argument, we're not distributing.
        return std::nullopt;
    }

    for (TypeId option : firstUnion)
    {
        arguments[unionIndex] = option;

        TypeFunctionReductionResult<TypeId> result = f(instance, arguments, packParams, ctx, args...); // NOLINT
        blockedTypes.insert(blockedTypes.end(), result.blockedTypes.begin(), result.blockedTypes.end());
        if (result.reductionStatus != Reduction::MaybeOk)
            reductionStatus = result.reductionStatus;

        if (reductionStatus != Reduction::MaybeOk || !result.result)
            break;
        else
            results.push_back(*result.result);
    }

    if (reductionStatus != Reduction::MaybeOk || !blockedTypes.empty())
        return {{std::nullopt, reductionStatus, std::move(blockedTypes), {}}};

    if (!results.empty())
    {
        if (results.size() == 1)
            return {{results[0], Reduction::MaybeOk, {}, {}}};

        TypeId resultTy = ctx->arena->addType(TypeFunctionInstanceType{
            NotNull{&builtinTypeFunctions().unionFunc},
            std::move(results),
            {},
        });

        if (ctx->solver)
            ctx->pushConstraint(ReduceConstraint{resultTy});

        return {{resultTy, Reduction::MaybeOk, {}, {}}};
    }

    return std::nullopt;
}

struct FindUserTypeFunctionBlockers : TypeOnceVisitor
{
    NotNull<TypeFunctionContext> ctx;
    DenseHashSet<TypeId> blockingTypeMap{nullptr};
    std::vector<TypeId> blockingTypes;

    explicit FindUserTypeFunctionBlockers(NotNull<TypeFunctionContext> ctx)
        : TypeOnceVisitor(/* skipBoundTypes */ true)
        , ctx(ctx)
    {
    }

    bool visit(TypeId ty) override
    {
        if (isPending(ty, ctx->solver))
        {
            if (!blockingTypeMap.contains(ty))
            {
                blockingTypeMap.insert(ty);
                blockingTypes.push_back(ty);
            }
        }
        return true;
    }

    bool visit(TypePackId tp) override
    {
        return true;
    }

    bool visit(TypeId ty, const ExternType&) override
    {
        return false;
    }
};

static int evaluateTypeAliasCall(lua_State* L)
{
    TypeFun* tf = (TypeFun*)lua_tolightuserdata(L, lua_upvalueindex(1));

    TypeFunctionRuntime* runtime = getTypeFunctionRuntime(L);
    TypeFunctionRuntimeBuilderState* runtimeBuilder = runtime->runtimeBuilder;

    ApplyTypeFunction applyTypeFunction{runtimeBuilder->ctx->arena};

    int argumentCount = lua_gettop(L);
    std::vector<TypeId> rawTypeArguments;

    for (int i = 0; i < argumentCount; i++)
    {
        TypeFunctionTypeId tfty = getTypeUserData(L, i + 1);
        TypeId ty = deserialize(tfty, runtimeBuilder);

        if (!runtimeBuilder->errors.empty())
            luaL_error(L, "failed to deserialize type at argument %d", i + 1);

        rawTypeArguments.push_back(ty);
    }

    // Check if we have enough arguments, by typical typechecking rules
    size_t typesRequired = tf->typeParams.size();
    size_t packsRequired = tf->typePackParams.size();

    size_t typesProvided = rawTypeArguments.size() > typesRequired ? typesRequired : rawTypeArguments.size();
    size_t extraTypes = rawTypeArguments.size() > typesRequired ? rawTypeArguments.size() - typesRequired : 0;
    size_t packsProvided = 0;

    if (extraTypes != 0 && packsProvided == 0)
    {
        // Extra types are only collected into a pack if a pack is expected
        if (packsRequired != 0)
            packsProvided += 1;
        else
            typesProvided += extraTypes;
    }

    for (size_t i = typesProvided; i < typesRequired; ++i)
    {
        if (tf->typeParams[i].defaultValue)
            typesProvided += 1;
    }

    for (size_t i = packsProvided; i < packsRequired; ++i)
    {
        if (tf->typePackParams[i].defaultValue)
            packsProvided += 1;
    }

    if (extraTypes == 0 && packsProvided + 1 == packsRequired)
        packsProvided += 1;

    if (typesProvided != typesRequired || packsProvided != packsRequired)
        luaL_error(L, "not enough arguments to call");

    // Prepare final types and packs
    auto [types, packs] = saturateArguments(runtimeBuilder->ctx->arena, runtimeBuilder->ctx->builtins, *tf, rawTypeArguments, {});

    for (size_t i = 0; i < types.size(); ++i)
        applyTypeFunction.typeArguments[tf->typeParams[i].ty] = types[i];

    for (size_t i = 0; i < packs.size(); ++i)
        applyTypeFunction.typePackArguments[tf->typePackParams[i].tp] = packs[i];

    std::optional<TypeId> maybeInstantiated = applyTypeFunction.substitute(tf->type);

    if (!maybeInstantiated.has_value())
    {
        luaL_error(L, "failed to instantiate type alias");
        return true;
    }

    TypeId target = follow(*maybeInstantiated);

    FunctionGraphReductionResult result = reduceTypeFunctions(target, Location{}, *runtimeBuilder->ctx);

    if (!result.errors.empty())
        luaL_error(L, "failed to reduce type function with: %s", toString(result.errors.front()).c_str());

    TypeFunctionTypeId serializedTy = serialize(follow(target), runtimeBuilder);

    if (!runtimeBuilder->errors.empty())
        luaL_error(L, "%s", runtimeBuilder->errors.front().c_str());

    allocTypeUserData(L, serializedTy->type);
    return 1;
}

TypeFunctionReductionResult<TypeId> userDefinedTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    auto typeFunction = getMutable<TypeFunctionInstanceType>(instance);

    if (typeFunction->userFuncData.owner.expired())
    {
        ctx->ice->ice("user-defined type function module has expired");
        return {std::nullopt, Reduction::Erroneous, {}, {}};
    }

    if (!typeFunction->userFuncName || !typeFunction->userFuncData.definition)
    {
        ctx->ice->ice("all user-defined type functions must have an associated function definition");
        return {std::nullopt, Reduction::Erroneous, {}, {}};
    }

    // If type functions cannot be evaluated because of errors in the code, we do not generate any additional ones
    if (!ctx->typeFunctionRuntime->allowEvaluation || typeFunction->userFuncData.definition->hasErrors)
        return {ctx->builtins->errorType, Reduction::MaybeOk, {}, {}};

    FindUserTypeFunctionBlockers check{ctx};

    for (auto typeParam : typeParams)
        check.traverse(follow(typeParam));

    if (FFlag::LuauUserTypeFunctionAliases)
    {
        // Check that our environment doesn't depend on any type aliases that are blocked
        for (auto& [name, definition] : typeFunction->userFuncData.environmentAlias)
        {
            if (definition.first->typeParams.empty() && definition.first->typePackParams.empty())
                check.traverse(follow(definition.first->type));
        }
    }

    if (!check.blockingTypes.empty())
        return {std::nullopt, Reduction::MaybeOk, check.blockingTypes, {}};

    // Ensure that whole type function environment is registered
    for (auto& [name, definition] : typeFunction->userFuncData.environmentFunction)
    {
        // Cannot evaluate if a potential dependency couldn't be parsed
        if (definition.first->hasErrors)
            return {ctx->builtins->errorType, Reduction::MaybeOk, {}, {}};

        if (std::optional<std::string> error = ctx->typeFunctionRuntime->registerFunction(definition.first))
        {
            // Failure to register at this point means that original definition had to error out and should not have been present in the
            // environment
            ctx->ice->ice("user-defined type function reference cannot be registered");
            return {std::nullopt, Reduction::Erroneous, {}, {}};
        }
    }

    AstName name = typeFunction->userFuncData.definition->name;

    lua_State* global = ctx->typeFunctionRuntime->state.get();

    if (global == nullptr)
        return {std::nullopt, Reduction::Erroneous, {}, {}, format("'%s' type function: cannot be evaluated in this context", name.value)};

    // Separate sandboxed thread for individual execution and private globals
    lua_State* L = lua_newthread(global);
    LuauTempThreadPopper popper(global);

    std::unique_ptr<TypeFunctionRuntimeBuilderState> runtimeBuilder = std::make_unique<TypeFunctionRuntimeBuilderState>(ctx);

    ScopedAssign setRuntimeBuilder(ctx->typeFunctionRuntime->runtimeBuilder, runtimeBuilder.get());
    ScopedAssign enableReduction(ctx->normalizer->sharedState->reentrantTypeReduction, false);

    // Build up the environment table of each function we have visible
    for (auto& [_, curr] : typeFunction->userFuncData.environmentFunction)
    {
        // Environment table has to be filled only once in the current execution context
        if (ctx->typeFunctionRuntime->initialized.find(curr.first))
            continue;
        ctx->typeFunctionRuntime->initialized.insert(curr.first);

        lua_pushlightuserdata(L, curr.first);
        lua_gettable(L, LUA_REGISTRYINDEX);

        if (!lua_isfunction(L, -1))
        {
            ctx->ice->ice("user-defined type function reference cannot be found in the registry");
            return {std::nullopt, Reduction::Erroneous, {}, {}};
        }

        // Build up the environment of the current function, where some might not be visible
        lua_getfenv(L, -1);
        lua_setreadonly(L, -1, false);

        for (auto& [name, definition] : typeFunction->userFuncData.environmentFunction)
        {
            // Filter visibility based on original scope depth
            if (definition.second >= curr.second)
            {
                lua_pushlightuserdata(L, definition.first);
                lua_gettable(L, LUA_REGISTRYINDEX);

                if (!lua_isfunction(L, -1))
                    break; // Don't have to report an error here, we will visit each function in outer loop

                lua_setfield(L, -2, name.c_str());
            }
        }

        if (FFlag::LuauUserTypeFunctionAliases)
        {
            for (auto& [name, definition] : typeFunction->userFuncData.environmentAlias)
            {
                // Filter visibility based on original scope depth
                if (definition.second >= curr.second)
                {
                    if (definition.first->typeParams.empty() && definition.first->typePackParams.empty())
                    {
                        TypeId ty = follow(definition.first->type);

                        // This is checked at the top of the function, and should still be true.
                        LUAU_ASSERT(!isPending(ty, ctx->solver));

                        TypeFunctionTypeId serializedTy = serialize(ty, runtimeBuilder.get());

                        // Only register aliases that are representable in type environment
                        if (runtimeBuilder->errors.empty())
                        {
                            allocTypeUserData(L, serializedTy->type);
                            lua_setfield(L, -2, name.c_str());
                        }
                    }
                    else
                    {
                        lua_pushlightuserdata(L, definition.first);
                        lua_pushcclosure(L, evaluateTypeAliasCall, name.c_str(), 1);
                        lua_setfield(L, -2, name.c_str());
                    }
                }
            }
        }

        lua_setreadonly(L, -1, true);
        lua_pop(L, 2);
    }

    // Fetch the function we want to evaluate
    lua_pushlightuserdata(L, typeFunction->userFuncData.definition);
    lua_gettable(L, LUA_REGISTRYINDEX);

    if (!lua_isfunction(L, -1))
    {
        ctx->ice->ice("user-defined type function reference cannot be found in the registry");
        return {std::nullopt, Reduction::Erroneous, {}, {}};
    }

    resetTypeFunctionState(L);

    // Push serialized arguments onto the stack
    for (auto typeParam : typeParams)
    {
        TypeId ty = follow(typeParam);
        // This is checked at the top of the function, and should still be true.
        LUAU_ASSERT(!isPending(ty, ctx->solver));

        TypeFunctionTypeId serializedTy = serialize(ty, runtimeBuilder.get());
        // Check if there were any errors while serializing
        if (runtimeBuilder->errors.size() != 0)
            return {std::nullopt, Reduction::Erroneous, {}, {}, runtimeBuilder->errors.front()};

        allocTypeUserData(L, serializedTy->type);
    }

    // Set up an interrupt handler for type functions to respect type checking limits and LSP cancellation requests.
    lua_callbacks(L)->interrupt = [](lua_State* L, int gc)
    {
        auto ctx = static_cast<const TypeFunctionRuntime*>(lua_getthreaddata(lua_mainthread(L)));
        if (ctx->limits->finishTime && TimeTrace::getClock() > *ctx->limits->finishTime)
            throw TimeLimitError(ctx->ice->moduleName);

        if (ctx->limits->cancellationToken && ctx->limits->cancellationToken->requested())
            throw UserCancelError(ctx->ice->moduleName);
    };

    ctx->typeFunctionRuntime->messages.clear();

    if (auto error = checkResultForError(L, name.value, lua_pcall(L, int(typeParams.size()), 1, 0)))
        return {std::nullopt, Reduction::Erroneous, {}, {}, std::move(error), ctx->typeFunctionRuntime->messages};

    // If the return value is not a type userdata, return with error message
    if (!isTypeUserData(L, 1))
    {
        return {
            std::nullopt,
            Reduction::Erroneous,
            {},
            {},
            format("'%s' type function: returned a non-type value", name.value),
            ctx->typeFunctionRuntime->messages
        };
    }

    TypeFunctionTypeId retTypeFunctionTypeId = getTypeUserData(L, 1);

    // No errors should be present here since we should've returned already if any were raised during serialization.
    LUAU_ASSERT(runtimeBuilder->errors.size() == 0);

    TypeId retTypeId = deserialize(retTypeFunctionTypeId, runtimeBuilder.get());

    // At least 1 error occurred while deserializing
    if (runtimeBuilder->errors.size() > 0)
        return {std::nullopt, Reduction::Erroneous, {}, {}, runtimeBuilder->errors.front(), ctx->typeFunctionRuntime->messages};

    return {retTypeId, Reduction::MaybeOk, {}, {}, std::nullopt, ctx->typeFunctionRuntime->messages};
}

TypeFunctionReductionResult<TypeId> notTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (typeParams.size() != 1 || !packParams.empty())
    {
        ctx->ice->ice("not type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    TypeId ty = follow(typeParams.at(0));

    if (ty == instance)
        return {ctx->builtins->neverType, Reduction::MaybeOk, {}, {}};

    if (isPending(ty, ctx->solver))
        return {std::nullopt, Reduction::MaybeOk, {ty}, {}};

    if (auto result = tryDistributeTypeFunctionApp(notTypeFunction, instance, typeParams, packParams, ctx))
        return *result;

    // `not` operates on anything and returns a `boolean` always.
    return {ctx->builtins->booleanType, Reduction::MaybeOk, {}, {}};
}

TypeFunctionReductionResult<TypeId> lenTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (typeParams.size() != 1 || !packParams.empty())
    {
        ctx->ice->ice("len type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    TypeId operandTy = follow(typeParams.at(0));

    if (operandTy == instance)
        return {ctx->builtins->neverType, Reduction::MaybeOk, {}, {}};

    // check to see if the operand type is resolved enough, and wait to reduce if not
    // the use of `typeFromNormal` later necessitates blocking on local types.
    if (isPending(operandTy, ctx->solver))
        return {std::nullopt, Reduction::MaybeOk, {operandTy}, {}};

    std::shared_ptr<const NormalizedType> normTy = ctx->normalizer->normalize(operandTy);
    NormalizationResult inhabited = ctx->normalizer->isInhabited(normTy.get());

    // if the type failed to normalize, we can't reduce, but know nothing about inhabitance.
    if (!normTy || inhabited == NormalizationResult::HitLimits)
        return {std::nullopt, Reduction::MaybeOk, {}, {}};

    // if the operand type is error suppressing, we can immediately reduce to `number`.
    if (normTy->shouldSuppressErrors())
        return {ctx->builtins->numberType, Reduction::MaybeOk, {}, {}};

    // # always returns a number, even if its operand is never.
    // if we're checking the length of a string, that works!
    if (inhabited == NormalizationResult::False || normTy->isSubtypeOfString())
        return {ctx->builtins->numberType, Reduction::MaybeOk, {}, {}};

    // we use the normalized operand here in case there was an intersection or union.
    TypeId normalizedOperand = follow(ctx->normalizer->typeFromNormal(*normTy));
    if (normTy->hasTopTable() || get<TableType>(normalizedOperand))
        return {ctx->builtins->numberType, Reduction::MaybeOk, {}, {}};

    if (auto result = tryDistributeTypeFunctionApp(lenTypeFunction, instance, typeParams, packParams, ctx))
        return *result;

    // findMetatableEntry demands the ability to emit errors, so we must give it
    // the necessary state to do that, even if we intend to just eat the errors.
    ErrorVec dummy;

    std::optional<TypeId> mmType = findMetatableEntry(ctx->builtins, dummy, operandTy, "__len", Location{});
    if (!mmType)
    {
        // If we have a metatable type with no __len, this means we still have a table with default length function
        if (get<MetatableType>(normalizedOperand))
            return {ctx->builtins->numberType, Reduction::MaybeOk, {}, {}};

        return {std::nullopt, Reduction::Erroneous, {}, {}};
    }

    mmType = follow(*mmType);
    if (isPending(*mmType, ctx->solver))
        return {std::nullopt, Reduction::MaybeOk, {*mmType}, {}};

    const FunctionType* mmFtv = get<FunctionType>(*mmType);
    if (!mmFtv)
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    std::optional<TypeId> instantiatedMmType = instantiate(ctx->builtins, ctx->arena, ctx->limits, ctx->scope, *mmType);
    if (!instantiatedMmType)
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    const FunctionType* instantiatedMmFtv = get<FunctionType>(*instantiatedMmType);
    if (!instantiatedMmFtv)
        return {ctx->builtins->errorType, Reduction::MaybeOk, {}, {}};

    TypePackId inferredArgPack = ctx->arena->addTypePack({operandTy});
    Unifier2 u2{ctx->arena, ctx->builtins, ctx->scope, ctx->ice};
    if (!u2.unify(inferredArgPack, instantiatedMmFtv->argTypes))
        return {std::nullopt, Reduction::Erroneous, {}, {}}; // occurs check failed

    Subtyping subtyping{ctx->builtins, ctx->arena, ctx->simplifier, ctx->normalizer, ctx->typeFunctionRuntime, ctx->ice};
    if (!subtyping.isSubtype(inferredArgPack, instantiatedMmFtv->argTypes, ctx->scope).isSubtype) // TODO: is this the right variance?
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    // `len` must return a `number`.
    return {ctx->builtins->numberType, Reduction::MaybeOk, {}, {}};
}

TypeFunctionReductionResult<TypeId> unmTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (typeParams.size() != 1 || !packParams.empty())
    {
        ctx->ice->ice("unm type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    TypeId operandTy = follow(typeParams.at(0));

    if (operandTy == instance)
        return {ctx->builtins->neverType, Reduction::MaybeOk, {}, {}};

    // check to see if the operand type is resolved enough, and wait to reduce if not
    if (isPending(operandTy, ctx->solver))
        return {std::nullopt, Reduction::MaybeOk, {operandTy}, {}};

    if (FFlag::LuauEagerGeneralization4)
        operandTy = follow(operandTy);

    std::shared_ptr<const NormalizedType> normTy = ctx->normalizer->normalize(operandTy);

    // if the operand failed to normalize, we can't reduce, but know nothing about inhabitance.
    if (!normTy)
        return {std::nullopt, Reduction::MaybeOk, {}, {}};

    // if the operand is error suppressing, we can just go ahead and reduce.
    if (normTy->shouldSuppressErrors())
        return {operandTy, Reduction::MaybeOk, {}, {}};

    // if we have a `never`, we can never observe that the operation didn't work.
    if (is<NeverType>(operandTy))
        return {ctx->builtins->neverType, Reduction::MaybeOk, {}, {}};

    // If the type is exactly `number`, we can reduce now.
    if (normTy->isExactlyNumber())
        return {ctx->builtins->numberType, Reduction::MaybeOk, {}, {}};

    if (auto result = tryDistributeTypeFunctionApp(unmTypeFunction, instance, typeParams, packParams, ctx))
        return *result;

    // findMetatableEntry demands the ability to emit errors, so we must give it
    // the necessary state to do that, even if we intend to just eat the errors.
    ErrorVec dummy;

    std::optional<TypeId> mmType = findMetatableEntry(ctx->builtins, dummy, operandTy, "__unm", Location{});
    if (!mmType)
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    mmType = follow(*mmType);
    if (isPending(*mmType, ctx->solver))
        return {std::nullopt, Reduction::MaybeOk, {*mmType}, {}};

    const FunctionType* mmFtv = get<FunctionType>(*mmType);
    if (!mmFtv)
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    std::optional<TypeId> instantiatedMmType = instantiate(ctx->builtins, ctx->arena, ctx->limits, ctx->scope, *mmType);
    if (!instantiatedMmType)
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    const FunctionType* instantiatedMmFtv = get<FunctionType>(*instantiatedMmType);
    if (!instantiatedMmFtv)
        return {ctx->builtins->errorType, Reduction::MaybeOk, {}, {}};

    TypePackId inferredArgPack = ctx->arena->addTypePack({operandTy});
    Unifier2 u2{ctx->arena, ctx->builtins, ctx->scope, ctx->ice};
    if (!u2.unify(inferredArgPack, instantiatedMmFtv->argTypes))
        return {std::nullopt, Reduction::Erroneous, {}, {}}; // occurs check failed

    Subtyping subtyping{ctx->builtins, ctx->arena, ctx->simplifier, ctx->normalizer, ctx->typeFunctionRuntime, ctx->ice};
    if (!subtyping.isSubtype(inferredArgPack, instantiatedMmFtv->argTypes, ctx->scope).isSubtype) // TODO: is this the right variance?
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    if (std::optional<TypeId> ret = first(instantiatedMmFtv->retTypes))
        return {ret, Reduction::MaybeOk, {}, {}};
    else
        return {std::nullopt, Reduction::Erroneous, {}, {}};
}

void dummyStateClose(lua_State*) {}

TypeFunctionRuntime::TypeFunctionRuntime(NotNull<InternalErrorReporter> ice, NotNull<TypeCheckLimits> limits)
    : ice(ice)
    , limits(limits)
    , state(nullptr, dummyStateClose)
{
}

TypeFunctionRuntime::~TypeFunctionRuntime() {}

std::optional<std::string> TypeFunctionRuntime::registerFunction(AstStatTypeFunction* function)
{
    // If evaluation is disabled, we do not generate additional error messages
    if (!allowEvaluation)
        return std::nullopt;

    // Do not evaluate type functions with parse errors inside
    if (function->hasErrors)
        return std::nullopt;

    prepareState();

    lua_State* global = state.get();

    // Fetch to check if function is already registered
    lua_pushlightuserdata(global, function);
    lua_gettable(global, LUA_REGISTRYINDEX);

    if (!lua_isnil(global, -1))
    {
        lua_pop(global, 1);
        return std::nullopt;
    }

    lua_pop(global, 1);

    AstName name = function->name;

    // Construct ParseResult containing the type function
    Allocator allocator;
    AstNameTable names(allocator);

    AstExpr* exprFunction = function->body;
    AstArray<AstExpr*> exprReturns{&exprFunction, 1};
    AstStatReturn stmtReturn{Location{}, exprReturns};
    AstStat* stmtArray[] = {&stmtReturn};
    AstArray<AstStat*> stmts{stmtArray, 1};
    AstStatBlock exec{Location{}, stmts};
    ParseResult parseResult{&exec, 1, {}, {}, {}, CstNodeMap{nullptr}};

    BytecodeBuilder builder;
    try
    {
        compileOrThrow(builder, parseResult, names);
    }
    catch (CompileError& e)
    {
        return format("'%s' type function failed to compile with error message: %s", name.value, e.what());
    }

    std::string bytecode = builder.getBytecode();

    // Separate sandboxed thread for individual execution and private globals
    lua_State* L = lua_newthread(global);
    LuauTempThreadPopper popper(global);

    // Create individual environment for the type function
    luaL_sandboxthread(L);

    // Do not allow global writes to that environment
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    lua_setreadonly(L, -1, true);
    lua_pop(L, 1);

    // Load bytecode into Luau state
    if (auto error = checkResultForError(L, name.value, luau_load(L, name.value, bytecode.data(), bytecode.size(), 0)))
        return error;

    // Execute the global function which should return our user-defined type function
    if (auto error = checkResultForError(L, name.value, lua_resume(L, nullptr, 0)))
        return error;

    if (!lua_isfunction(L, -1))
    {
        lua_pop(L, 1);
        return format("Could not find '%s' type function in the global scope", name.value);
    }

    // Store resulting function in the registry
    lua_pushlightuserdata(global, function);
    lua_xmove(L, global, 1);
    lua_settable(global, LUA_REGISTRYINDEX);

    return std::nullopt;
}

void TypeFunctionRuntime::prepareState()
{
    if (state)
        return;

    state = StateRef(lua_newstate(typeFunctionAlloc, nullptr), lua_close);
    lua_State* L = state.get();

    lua_setthreaddata(L, this);

    setTypeFunctionEnvironment(L);

    registerTypeUserData(L);

    registerTypesLibrary(L);

    luaL_sandbox(L);
    luaL_sandboxthread(L);
}

TypeFunctionContext::TypeFunctionContext(NotNull<ConstraintSolver> cs, NotNull<Scope> scope, NotNull<const Constraint> constraint)
    : arena(cs->arena)
    , builtins(cs->builtinTypes)
    , scope(scope)
    , simplifier(cs->simplifier)
    , normalizer(cs->normalizer)
    , typeFunctionRuntime(cs->typeFunctionRuntime)
    , ice(NotNull{&cs->iceReporter})
    , limits(NotNull{&cs->limits})
    , solver(cs.get())
    , constraint(constraint.get())
{
}

NotNull<Constraint> TypeFunctionContext::pushConstraint(ConstraintV&& c) const
{
    LUAU_ASSERT(solver);
    NotNull<Constraint> newConstraint = solver->pushConstraint(scope, constraint ? constraint->location : Location{}, std::move(c));

    // Every constraint that is blocked on the current constraint must also be
    // blocked on this new one.
    if (constraint)
        solver->inheritBlocks(NotNull{constraint}, newConstraint);

    return newConstraint;
}

TypeFunctionReductionResult<TypeId> numericBinopTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx,
    const std::string metamethod
)
{
    if (typeParams.size() != 2 || !packParams.empty())
    {
        ctx->ice->ice("encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    TypeId lhsTy = follow(typeParams.at(0));
    TypeId rhsTy = follow(typeParams.at(1));

    // isPending of `lhsTy` or `rhsTy` would return true, even if it cycles. We want a different answer for that.
    if (lhsTy == instance || rhsTy == instance)
        return {ctx->builtins->neverType, Reduction::MaybeOk, {}, {}};

    // if we have a `never`, we can never observe that the math operator is unreachable.
    if (is<NeverType>(lhsTy) || is<NeverType>(rhsTy))
        return {ctx->builtins->neverType, Reduction::MaybeOk, {}, {}};

    const Location location = ctx->constraint ? ctx->constraint->location : Location{};

    // check to see if both operand types are resolved enough, and wait to reduce if not
    if (isPending(lhsTy, ctx->solver))
        return {std::nullopt, Reduction::MaybeOk, {lhsTy}, {}};
    else if (isPending(rhsTy, ctx->solver))
        return {std::nullopt, Reduction::MaybeOk, {rhsTy}, {}};

    // TODO: Normalization needs to remove cyclic type functions from a `NormalizedType`.
    std::shared_ptr<const NormalizedType> normLhsTy = ctx->normalizer->normalize(lhsTy);
    std::shared_ptr<const NormalizedType> normRhsTy = ctx->normalizer->normalize(rhsTy);

    // if either failed to normalize, we can't reduce, but know nothing about inhabitance.
    if (!normLhsTy || !normRhsTy)
        return {std::nullopt, Reduction::MaybeOk, {}, {}};

    // if one of the types is error suppressing, we can reduce to `any` since we should suppress errors in the result of the usage.
    if (normLhsTy->shouldSuppressErrors() || normRhsTy->shouldSuppressErrors())
        return {ctx->builtins->anyType, Reduction::MaybeOk, {}, {}};

    // if we're adding two `number` types, the result is `number`.
    if (normLhsTy->isExactlyNumber() && normRhsTy->isExactlyNumber())
        return {ctx->builtins->numberType, Reduction::MaybeOk, {}, {}};

    if (auto result = tryDistributeTypeFunctionApp(numericBinopTypeFunction, instance, typeParams, packParams, ctx, metamethod))
        return *result;

    // findMetatableEntry demands the ability to emit errors, so we must give it
    // the necessary state to do that, even if we intend to just eat the errors.
    ErrorVec dummy;

    std::optional<TypeId> mmType = findMetatableEntry(ctx->builtins, dummy, lhsTy, metamethod, location);
    bool reversed = false;
    if (!mmType)
    {
        mmType = findMetatableEntry(ctx->builtins, dummy, rhsTy, metamethod, location);
        reversed = true;
    }

    if (!mmType)
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    mmType = follow(*mmType);
    if (isPending(*mmType, ctx->solver))
        return {std::nullopt, Reduction::MaybeOk, {*mmType}, {}};

    TypePackId argPack = ctx->arena->addTypePack({lhsTy, rhsTy});
    SolveResult solveResult;

    if (!reversed)
        solveResult = solveFunctionCall(
            ctx->arena,
            ctx->builtins,
            ctx->simplifier,
            ctx->normalizer,
            ctx->typeFunctionRuntime,
            ctx->ice,
            ctx->limits,
            ctx->scope,
            location,
            *mmType,
            argPack
        );
    else
    {
        TypePack* p = getMutable<TypePack>(argPack);
        std::swap(p->head.front(), p->head.back());
        solveResult = solveFunctionCall(
            ctx->arena,
            ctx->builtins,
            ctx->simplifier,
            ctx->normalizer,
            ctx->typeFunctionRuntime,
            ctx->ice,
            ctx->limits,
            ctx->scope,
            location,
            *mmType,
            argPack
        );
    }

    if (!solveResult.typePackId.has_value())
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    TypePack extracted = extendTypePack(*ctx->arena, ctx->builtins, *solveResult.typePackId, 1);
    if (extracted.head.empty())
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    return {extracted.head.front(), Reduction::MaybeOk, {}, {}};
}

TypeFunctionReductionResult<TypeId> addTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (typeParams.size() != 2 || !packParams.empty())
    {
        ctx->ice->ice("add type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    return numericBinopTypeFunction(instance, typeParams, packParams, ctx, "__add");
}

TypeFunctionReductionResult<TypeId> subTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (typeParams.size() != 2 || !packParams.empty())
    {
        ctx->ice->ice("sub type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    return numericBinopTypeFunction(instance, typeParams, packParams, ctx, "__sub");
}

TypeFunctionReductionResult<TypeId> mulTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (typeParams.size() != 2 || !packParams.empty())
    {
        ctx->ice->ice("mul type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    return numericBinopTypeFunction(instance, typeParams, packParams, ctx, "__mul");
}

TypeFunctionReductionResult<TypeId> divTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (typeParams.size() != 2 || !packParams.empty())
    {
        ctx->ice->ice("div type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    return numericBinopTypeFunction(instance, typeParams, packParams, ctx, "__div");
}

TypeFunctionReductionResult<TypeId> idivTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (typeParams.size() != 2 || !packParams.empty())
    {
        ctx->ice->ice("integer div type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    return numericBinopTypeFunction(instance, typeParams, packParams, ctx, "__idiv");
}

TypeFunctionReductionResult<TypeId> powTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (typeParams.size() != 2 || !packParams.empty())
    {
        ctx->ice->ice("pow type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    return numericBinopTypeFunction(instance, typeParams, packParams, ctx, "__pow");
}

TypeFunctionReductionResult<TypeId> modTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (typeParams.size() != 2 || !packParams.empty())
    {
        ctx->ice->ice("modulo type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    return numericBinopTypeFunction(instance, typeParams, packParams, ctx, "__mod");
}

TypeFunctionReductionResult<TypeId> concatTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (typeParams.size() != 2 || !packParams.empty())
    {
        ctx->ice->ice("concat type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    TypeId lhsTy = follow(typeParams.at(0));
    TypeId rhsTy = follow(typeParams.at(1));

    // isPending of `lhsTy` or `rhsTy` would return true, even if it cycles. We want a different answer for that.
    if (lhsTy == instance || rhsTy == instance)
        return {ctx->builtins->neverType, Reduction::MaybeOk, {}, {}};

    // check to see if both operand types are resolved enough, and wait to reduce if not
    if (isPending(lhsTy, ctx->solver))
        return {std::nullopt, Reduction::MaybeOk, {lhsTy}, {}};
    else if (isPending(rhsTy, ctx->solver))
        return {std::nullopt, Reduction::MaybeOk, {rhsTy}, {}};

    std::shared_ptr<const NormalizedType> normLhsTy = ctx->normalizer->normalize(lhsTy);
    std::shared_ptr<const NormalizedType> normRhsTy = ctx->normalizer->normalize(rhsTy);

    // if either failed to normalize, we can't reduce, but know nothing about inhabitance.
    if (!normLhsTy || !normRhsTy)
        return {std::nullopt, Reduction::MaybeOk, {}, {}};

    // if one of the types is error suppressing, we can reduce to `any` since we should suppress errors in the result of the usage.
    if (normLhsTy->shouldSuppressErrors() || normRhsTy->shouldSuppressErrors())
        return {ctx->builtins->anyType, Reduction::MaybeOk, {}, {}};

    // if we have a `never`, we can never observe that the operator didn't work.
    if (is<NeverType>(lhsTy) || is<NeverType>(rhsTy))
        return {ctx->builtins->neverType, Reduction::MaybeOk, {}, {}};

    // if we're concatenating two elements that are either strings or numbers, the result is `string`.
    if ((normLhsTy->isSubtypeOfString() || normLhsTy->isExactlyNumber()) && (normRhsTy->isSubtypeOfString() || normRhsTy->isExactlyNumber()))
        return {ctx->builtins->stringType, Reduction::MaybeOk, {}, {}};

    if (auto result = tryDistributeTypeFunctionApp(concatTypeFunction, instance, typeParams, packParams, ctx))
        return *result;

    // findMetatableEntry demands the ability to emit errors, so we must give it
    // the necessary state to do that, even if we intend to just eat the errors.
    ErrorVec dummy;

    std::optional<TypeId> mmType = findMetatableEntry(ctx->builtins, dummy, lhsTy, "__concat", Location{});
    bool reversed = false;
    if (!mmType)
    {
        mmType = findMetatableEntry(ctx->builtins, dummy, rhsTy, "__concat", Location{});
        reversed = true;
    }

    if (!mmType)
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    mmType = follow(*mmType);
    if (isPending(*mmType, ctx->solver))
        return {std::nullopt, Reduction::MaybeOk, {*mmType}, {}};

    const FunctionType* mmFtv = get<FunctionType>(*mmType);
    if (!mmFtv)
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    std::optional<TypeId> instantiatedMmType = instantiate(ctx->builtins, ctx->arena, ctx->limits, ctx->scope, *mmType);
    if (!instantiatedMmType)
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    const FunctionType* instantiatedMmFtv = get<FunctionType>(*instantiatedMmType);
    if (!instantiatedMmFtv)
        return {ctx->builtins->errorType, Reduction::MaybeOk, {}, {}};

    std::vector<TypeId> inferredArgs;
    if (!reversed)
        inferredArgs = {lhsTy, rhsTy};
    else
        inferredArgs = {rhsTy, lhsTy};

    TypePackId inferredArgPack = ctx->arena->addTypePack(std::move(inferredArgs));
    Unifier2 u2{ctx->arena, ctx->builtins, ctx->scope, ctx->ice};
    if (!u2.unify(inferredArgPack, instantiatedMmFtv->argTypes))
        return {std::nullopt, Reduction::Erroneous, {}, {}}; // occurs check failed

    Subtyping subtyping{ctx->builtins, ctx->arena, ctx->simplifier, ctx->normalizer, ctx->typeFunctionRuntime, ctx->ice};
    if (!subtyping.isSubtype(inferredArgPack, instantiatedMmFtv->argTypes, ctx->scope).isSubtype) // TODO: is this the right variance?
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    return {ctx->builtins->stringType, Reduction::MaybeOk, {}, {}};
}

TypeFunctionReductionResult<TypeId> andTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (typeParams.size() != 2 || !packParams.empty())
    {
        ctx->ice->ice("and type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    TypeId lhsTy = follow(typeParams.at(0));
    TypeId rhsTy = follow(typeParams.at(1));

    // t1 = and<lhs, t1> ~> lhs
    if (follow(rhsTy) == instance && lhsTy != rhsTy)
        return {lhsTy, Reduction::MaybeOk, {}, {}};
    // t1 = and<t1, rhs> ~> rhs
    if (follow(lhsTy) == instance && lhsTy != rhsTy)
        return {rhsTy, Reduction::MaybeOk, {}, {}};

    // check to see if both operand types are resolved enough, and wait to reduce if not
    if (isPending(lhsTy, ctx->solver))
        return {std::nullopt, Reduction::MaybeOk, {lhsTy}, {}};
    else if (isPending(rhsTy, ctx->solver))
        return {std::nullopt, Reduction::MaybeOk, {rhsTy}, {}};

    // And evalutes to a boolean if the LHS is falsey, and the RHS type if LHS is truthy.
    SimplifyResult filteredLhs = simplifyIntersection(ctx->builtins, ctx->arena, lhsTy, ctx->builtins->falsyType);
    SimplifyResult overallResult = simplifyUnion(ctx->builtins, ctx->arena, rhsTy, filteredLhs.result);
    std::vector<TypeId> blockedTypes{};
    for (auto ty : filteredLhs.blockedTypes)
        blockedTypes.push_back(ty);
    for (auto ty : overallResult.blockedTypes)
        blockedTypes.push_back(ty);
    return {overallResult.result, Reduction::MaybeOk, std::move(blockedTypes), {}};
}

TypeFunctionReductionResult<TypeId> orTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (typeParams.size() != 2 || !packParams.empty())
    {
        ctx->ice->ice("or type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    TypeId lhsTy = follow(typeParams.at(0));
    TypeId rhsTy = follow(typeParams.at(1));

    // t1 = or<lhs, t1> ~> lhs
    if (follow(rhsTy) == instance && lhsTy != rhsTy)
        return {lhsTy, Reduction::MaybeOk, {}, {}};
    // t1 = or<t1, rhs> ~> rhs
    if (follow(lhsTy) == instance && lhsTy != rhsTy)
        return {rhsTy, Reduction::MaybeOk, {}, {}};

    // check to see if both operand types are resolved enough, and wait to reduce if not
    if (FFlag::LuauEagerGeneralization4)
    {
        if (is<BlockedType, PendingExpansionType, TypeFunctionInstanceType>(lhsTy))
            return {std::nullopt, Reduction::MaybeOk, {lhsTy}, {}};
        else if (is<BlockedType, PendingExpansionType, TypeFunctionInstanceType>(rhsTy))
            return {std::nullopt, Reduction::MaybeOk, {rhsTy}, {}};
    }
    else
    {
        if (isPending(lhsTy, ctx->solver))
            return {std::nullopt, Reduction::MaybeOk, {lhsTy}, {}};
        else if (isPending(rhsTy, ctx->solver))
            return {std::nullopt, Reduction::MaybeOk, {rhsTy}, {}};
    }

    // Or evalutes to the LHS type if the LHS is truthy, and the RHS type if LHS is falsy.
    SimplifyResult filteredLhs = simplifyIntersection(ctx->builtins, ctx->arena, lhsTy, ctx->builtins->truthyType);
    SimplifyResult overallResult = simplifyUnion(ctx->builtins, ctx->arena, rhsTy, filteredLhs.result);
    std::vector<TypeId> blockedTypes{};
    for (auto ty : filteredLhs.blockedTypes)
        blockedTypes.push_back(ty);
    for (auto ty : overallResult.blockedTypes)
        blockedTypes.push_back(ty);
    return {overallResult.result, Reduction::MaybeOk, std::move(blockedTypes), {}};
}

static TypeFunctionReductionResult<TypeId> comparisonTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx,
    const std::string metamethod
)
{

    if (typeParams.size() != 2 || !packParams.empty())
    {
        ctx->ice->ice("encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    TypeId lhsTy = follow(typeParams.at(0));
    TypeId rhsTy = follow(typeParams.at(1));

    if (lhsTy == instance || rhsTy == instance)
        return {ctx->builtins->neverType, Reduction::MaybeOk, {}, {}};

    if (FFlag::LuauEagerGeneralization4)
    {
        if (is<BlockedType, PendingExpansionType, TypeFunctionInstanceType>(lhsTy))
            return {std::nullopt, Reduction::MaybeOk, {lhsTy}, {}};
        else if (is<BlockedType, PendingExpansionType, TypeFunctionInstanceType>(rhsTy))
            return {std::nullopt, Reduction::MaybeOk, {rhsTy}, {}};
    }
    else
    {
        if (isPending(lhsTy, ctx->solver))
            return {std::nullopt, Reduction::MaybeOk, {lhsTy}, {}};
        else if (isPending(rhsTy, ctx->solver))
            return {std::nullopt, Reduction::MaybeOk, {rhsTy}, {}};
    }

    // Algebra Reduction Rules for comparison type functions
    // Note that comparing to never tells you nothing about the other operand
    // lt< 'a , never> -> continue
    // lt< never, 'a>  -> continue
    // lt< 'a, t>      -> 'a is t - we'll solve the constraint, return and solve lt<t, t> -> bool
    // lt< t, 'a>      -> same as above
    bool canSubmitConstraint = ctx->solver && ctx->constraint;
    bool lhsFree = get<FreeType>(lhsTy) != nullptr;
    bool rhsFree = get<FreeType>(rhsTy) != nullptr;
    if (canSubmitConstraint)
    {
        // Implement injective type functions for comparison type functions
        // lt <number, t> implies t is number
        // lt <t, number> implies t is number
        if (lhsFree && isNumber(rhsTy))
            emplaceType<BoundType>(asMutable(lhsTy), ctx->builtins->numberType);
        else if (rhsFree && isNumber(lhsTy))
            emplaceType<BoundType>(asMutable(rhsTy), ctx->builtins->numberType);
    }

    // The above might have caused the operand types to be rebound, we need to follow them again
    lhsTy = follow(lhsTy);
    rhsTy = follow(rhsTy);

    // check to see if both operand types are resolved enough, and wait to reduce if not

    std::shared_ptr<const NormalizedType> normLhsTy = ctx->normalizer->normalize(lhsTy);
    std::shared_ptr<const NormalizedType> normRhsTy = ctx->normalizer->normalize(rhsTy);
    NormalizationResult lhsInhabited = ctx->normalizer->isInhabited(normLhsTy.get());
    NormalizationResult rhsInhabited = ctx->normalizer->isInhabited(normRhsTy.get());

    // if either failed to normalize, we can't reduce, but know nothing about inhabitance.
    if (!normLhsTy || !normRhsTy || lhsInhabited == NormalizationResult::HitLimits || rhsInhabited == NormalizationResult::HitLimits)
        return {std::nullopt, Reduction::MaybeOk, {}, {}};

    // if one of the types is error suppressing, we can just go ahead and reduce.
    if (normLhsTy->shouldSuppressErrors() || normRhsTy->shouldSuppressErrors())
        return {ctx->builtins->booleanType, Reduction::MaybeOk, {}, {}};

    // if we have an uninhabited type (e.g. `never`), we can never observe that the comparison didn't work.
    if (lhsInhabited == NormalizationResult::False || rhsInhabited == NormalizationResult::False)
        return {ctx->builtins->booleanType, Reduction::MaybeOk, {}, {}};

    // If both types are some strict subset of `string`, we can reduce now.
    if (normLhsTy->isSubtypeOfString() && normRhsTy->isSubtypeOfString())
        return {ctx->builtins->booleanType, Reduction::MaybeOk, {}, {}};

    // If both types are exactly `number`, we can reduce now.
    if (normLhsTy->isExactlyNumber() && normRhsTy->isExactlyNumber())
        return {ctx->builtins->booleanType, Reduction::MaybeOk, {}, {}};

    if (auto result = tryDistributeTypeFunctionApp(comparisonTypeFunction, instance, typeParams, packParams, ctx, metamethod))
        return *result;

    // findMetatableEntry demands the ability to emit errors, so we must give it
    // the necessary state to do that, even if we intend to just eat the errors.
    ErrorVec dummy;

    std::optional<TypeId> mmType = findMetatableEntry(ctx->builtins, dummy, lhsTy, metamethod, Location{});
    if (!mmType)
        mmType = findMetatableEntry(ctx->builtins, dummy, rhsTy, metamethod, Location{});

    if (!mmType)
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    mmType = follow(*mmType);
    if (isPending(*mmType, ctx->solver))
        return {std::nullopt, Reduction::MaybeOk, {*mmType}, {}};

    const FunctionType* mmFtv = get<FunctionType>(*mmType);
    if (!mmFtv)
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    std::optional<TypeId> instantiatedMmType = instantiate(ctx->builtins, ctx->arena, ctx->limits, ctx->scope, *mmType);
    if (!instantiatedMmType)
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    const FunctionType* instantiatedMmFtv = get<FunctionType>(*instantiatedMmType);
    if (!instantiatedMmFtv)
        return {ctx->builtins->errorType, Reduction::MaybeOk, {}, {}};

    TypePackId inferredArgPack = ctx->arena->addTypePack({lhsTy, rhsTy});
    Unifier2 u2{ctx->arena, ctx->builtins, ctx->scope, ctx->ice};
    if (!u2.unify(inferredArgPack, instantiatedMmFtv->argTypes))
        return {std::nullopt, Reduction::Erroneous, {}, {}}; // occurs check failed

    Subtyping subtyping{ctx->builtins, ctx->arena, ctx->simplifier, ctx->normalizer, ctx->typeFunctionRuntime, ctx->ice};
    if (!subtyping.isSubtype(inferredArgPack, instantiatedMmFtv->argTypes, ctx->scope).isSubtype) // TODO: is this the right variance?
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    return {ctx->builtins->booleanType, Reduction::MaybeOk, {}, {}};
}

TypeFunctionReductionResult<TypeId> ltTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (typeParams.size() != 2 || !packParams.empty())
    {
        ctx->ice->ice("lt type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    return comparisonTypeFunction(instance, typeParams, packParams, ctx, "__lt");
}

TypeFunctionReductionResult<TypeId> leTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (typeParams.size() != 2 || !packParams.empty())
    {
        ctx->ice->ice("le type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    return comparisonTypeFunction(instance, typeParams, packParams, ctx, "__le");
}

TypeFunctionReductionResult<TypeId> eqTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (typeParams.size() != 2 || !packParams.empty())
    {
        ctx->ice->ice("eq type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    TypeId lhsTy = follow(typeParams.at(0));
    TypeId rhsTy = follow(typeParams.at(1));

    // check to see if both operand types are resolved enough, and wait to reduce if not
    if (isPending(lhsTy, ctx->solver))
        return {std::nullopt, Reduction::MaybeOk, {lhsTy}, {}};
    else if (isPending(rhsTy, ctx->solver))
        return {std::nullopt, Reduction::MaybeOk, {rhsTy}, {}};

    std::shared_ptr<const NormalizedType> normLhsTy = ctx->normalizer->normalize(lhsTy);
    std::shared_ptr<const NormalizedType> normRhsTy = ctx->normalizer->normalize(rhsTy);
    NormalizationResult lhsInhabited = ctx->normalizer->isInhabited(normLhsTy.get());
    NormalizationResult rhsInhabited = ctx->normalizer->isInhabited(normRhsTy.get());

    // if either failed to normalize, we can't reduce, but know nothing about inhabitance.
    if (!normLhsTy || !normRhsTy || lhsInhabited == NormalizationResult::HitLimits || rhsInhabited == NormalizationResult::HitLimits)
        return {std::nullopt, Reduction::MaybeOk, {}, {}};

    // if one of the types is error suppressing, we can just go ahead and reduce.
    if (normLhsTy->shouldSuppressErrors() || normRhsTy->shouldSuppressErrors())
        return {ctx->builtins->booleanType, Reduction::MaybeOk, {}, {}};

    // if we have a `never`, we can never observe that the comparison didn't work.
    if (lhsInhabited == NormalizationResult::False || rhsInhabited == NormalizationResult::False)
        return {ctx->builtins->booleanType, Reduction::MaybeOk, {}, {}};

    // findMetatableEntry demands the ability to emit errors, so we must give it
    // the necessary state to do that, even if we intend to just eat the errors.
    ErrorVec dummy;

    std::optional<TypeId> mmType = findMetatableEntry(ctx->builtins, dummy, lhsTy, "__eq", Location{});
    if (!mmType)
        mmType = findMetatableEntry(ctx->builtins, dummy, rhsTy, "__eq", Location{});

    // if neither type has a metatable entry for `__eq`, then we'll check for inhabitance of the intersection!
    NormalizationResult intersectInhabited = ctx->normalizer->isIntersectionInhabited(lhsTy, rhsTy);
    if (!mmType)
    {
        if (intersectInhabited == NormalizationResult::True)
            return {ctx->builtins->booleanType, Reduction::MaybeOk, {}, {}}; // if it's inhabited, everything is okay!

        // we might be in a case where we still want to accept the comparison...
        if (intersectInhabited == NormalizationResult::False)
        {
            // if they're both subtypes of `string` but have no common intersection, the comparison is allowed but always `false`.
            if (normLhsTy->isSubtypeOfString() && normRhsTy->isSubtypeOfString())
                return {ctx->builtins->falseType, Reduction::MaybeOk, {}, {}};

            // if they're both subtypes of `boolean` but have no common intersection, the comparison is allowed but always `false`.
            if (normLhsTy->isSubtypeOfBooleans() && normRhsTy->isSubtypeOfBooleans())
                return {ctx->builtins->falseType, Reduction::MaybeOk, {}, {}};
        }

        return {std::nullopt, Reduction::Erroneous, {}, {}}; // if it's not, then this type function is irreducible!
    }

    mmType = follow(*mmType);
    if (isPending(*mmType, ctx->solver))
        return {std::nullopt, Reduction::MaybeOk, {*mmType}, {}};

    const FunctionType* mmFtv = get<FunctionType>(*mmType);
    if (!mmFtv)
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    std::optional<TypeId> instantiatedMmType = instantiate(ctx->builtins, ctx->arena, ctx->limits, ctx->scope, *mmType);
    if (!instantiatedMmType)
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    const FunctionType* instantiatedMmFtv = get<FunctionType>(*instantiatedMmType);
    if (!instantiatedMmFtv)
        return {ctx->builtins->errorType, Reduction::MaybeOk, {}, {}};

    TypePackId inferredArgPack = ctx->arena->addTypePack({lhsTy, rhsTy});
    Unifier2 u2{ctx->arena, ctx->builtins, ctx->scope, ctx->ice};
    if (!u2.unify(inferredArgPack, instantiatedMmFtv->argTypes))
        return {std::nullopt, Reduction::Erroneous, {}, {}}; // occurs check failed

    Subtyping subtyping{ctx->builtins, ctx->arena, ctx->simplifier, ctx->normalizer, ctx->typeFunctionRuntime, ctx->ice};
    if (!subtyping.isSubtype(inferredArgPack, instantiatedMmFtv->argTypes, ctx->scope).isSubtype) // TODO: is this the right variance?
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    return {ctx->builtins->booleanType, Reduction::MaybeOk, {}, {}};
}

// Collect types that prevent us from reducing a particular refinement.
struct FindRefinementBlockers : TypeOnceVisitor
{
    DenseHashSet<TypeId> found{nullptr};
    bool visit(TypeId ty, const BlockedType&) override
    {
        found.insert(ty);
        return false;
    }

    bool visit(TypeId ty, const PendingExpansionType&) override
    {
        found.insert(ty);
        return false;
    }

    bool visit(TypeId ty, const ExternType&) override
    {
        return false;
    }
};

struct ContainsRefinableType : TypeOnceVisitor
{
    bool found = false;
    ContainsRefinableType()
        : TypeOnceVisitor(/* skipBoundTypes */ true)
    {
    }


    bool visit(TypeId ty) override
    {
        // Default case: if we find *some* type that's worth refining against,
        // then we can claim that this type contains a refineable type.
        found = true;
        return false;
    }

    bool visit(TypeId Ty, const NoRefineType&) override
    {
        // No refine types aren't interesting
        return false;
    }

    bool visit(TypeId ty, const TableType&) override
    {
        return !found;
    }
    bool visit(TypeId ty, const MetatableType&) override
    {
        return !found;
    }
    bool visit(TypeId ty, const FunctionType&) override
    {
        return !found;
    }
    bool visit(TypeId ty, const UnionType&) override
    {
        return !found;
    }
    bool visit(TypeId ty, const IntersectionType&) override
    {
        return !found;
    }
    bool visit(TypeId ty, const NegationType&) override
    {
        return !found;
    }
};

namespace
{

bool isTruthyOrFalsyType(TypeId ty)
{
    ty = follow(ty);
    return isApproximatelyTruthyType(ty) || isApproximatelyFalsyType(ty);
}

struct RefineTypeScrubber : public Substitution
{
    NotNull<TypeFunctionContext> ctx;
    TypeId needle;

    explicit RefineTypeScrubber(NotNull<TypeFunctionContext> ctx, TypeId needle)
        : Substitution(ctx->arena)
        , ctx{ctx}
        , needle{needle}
    {
    }

    bool isDirty(TypePackId tp) override
    {
        return false;
    }

    bool ignoreChildren(TypePackId tp) override
    {
        return false;
    }

    TypePackId clean(TypePackId tp) override
    {
        return tp;
    }

    bool isDirty(TypeId ty) override
    {
        if (auto ut = get<UnionType>(ty))
        {
            for (auto option : ut)
            {
                if (option == needle)
                    return true;
            }
        }
        else if (auto it = get<IntersectionType>(ty))
        {
            for (auto part : it)
            {
                if (part == needle)
                    return true;
            }
        }
        return false;
    }

    bool ignoreChildren(TypeId ty) override
    {
        return !is<UnionType, IntersectionType>(ty);
    }

    TypeId clean(TypeId ty) override 
    {
        // NOTE: this feels pretty similar to other places where we try to
        // filter over a set type, may be worth combining those in the future.
        if (auto ut = get<UnionType>(ty))
        {
            TypeIds newOptions;
            for (auto option : ut)
            {
                if (option != needle && !is<NeverType>(option))
                    newOptions.insert(option);
            }
            if (newOptions.empty())
                return ctx->builtins->neverType;
            else if (newOptions.size() == 1)
                return *newOptions.begin();
            else
                return ctx->arena->addType(UnionType{newOptions.take()});
        }
        else if (auto it = get<IntersectionType>(ty))
        {
            TypeIds newParts;
            for (auto part : it)
            {
                if (part != needle && !is<UnknownType>(part))
                    newParts.insert(part);
            }
            if (newParts.empty())
                return ctx->builtins->unknownType;
            else if (newParts.size() == 1)
                return *newParts.begin();
            else
                return ctx->arena->addType(IntersectionType{newParts.take()});
        }
        return ty;
    }

};

bool occurs(TypeId haystack, TypeId needle, DenseHashSet<TypeId>& seen)
{
    if (needle == haystack)
        return true;

    if (seen.contains(haystack))
        return false;

    seen.insert(haystack);

    if (auto ut = get<UnionType>(haystack))
    {
        for (auto option : ut)
            if (occurs(option, needle, seen))
                return true;
    }

    if (auto it = get<UnionType>(haystack))
    {
        for (auto part : it)
            if (occurs(part, needle, seen))
                return true;
    }

    return false;
}

bool occurs(TypeId haystack, TypeId needle)
{
    DenseHashSet<TypeId> seen{nullptr};
    return occurs(haystack, needle, seen);
}

} // namespace

TypeFunctionReductionResult<TypeId> refineTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (typeParams.size() < 2 || !packParams.empty())
    {
        ctx->ice->ice("refine type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    TypeId targetTy = follow(typeParams.at(0));

    if (FFlag::LuauOccursCheckForRefinement)
    {
        // If we end up minting a refine type like:
        //
        //  t1 where t1 = refine<T | t1, Y>
        //
        // This can create a degenerate set type such as:
        //
        //  t1 where t1 = (T | t1) & Y
        //
        // Instead, we can clip the recursive part:
        //
        //  t1 where t1 = refine<T | t1, Y> => refine<T, Y>
        if (!FFlag::LuauAvoidExcessiveTypeCopying || occurs(targetTy, instance))
        {
            RefineTypeScrubber rts{ctx, instance};
            if (auto result = rts.substitute(targetTy))
                targetTy = *result;
        }
    }

    std::vector<TypeId> discriminantTypes;
    for (size_t i = 1; i < typeParams.size(); i++)
        discriminantTypes.push_back(follow(typeParams.at(i)));

    const bool targetIsPending = FFlag::LuauEagerGeneralization4 ? is<BlockedType, PendingExpansionType, TypeFunctionInstanceType>(targetTy)
                                                                 : isPending(targetTy, ctx->solver);

    // check to see if both operand types are resolved enough, and wait to reduce if not
    if (targetIsPending)
        return {std::nullopt, Reduction::MaybeOk, {targetTy}, {}};
    else
    {
        for (auto t : discriminantTypes)
        {
            if (isPending(t, ctx->solver))
                return {std::nullopt, Reduction::MaybeOk, {t}, {}};
        }
    }

    // If we have a blocked type in the target, we *could* potentially
    // refine it, but more likely we end up with some type explosion in
    // normalization.
    FindRefinementBlockers frb;
    frb.traverse(targetTy);
    if (!frb.found.empty())
        return {std::nullopt, Reduction::MaybeOk, {frb.found.begin(), frb.found.end()}, {}};

    // Refine a target type and a discriminant one at a time.
    // Returns result : TypeId, toBlockOn : vector<TypeId>
    auto stepRefine = [&ctx](TypeId target, TypeId discriminant) -> std::pair<TypeId, std::vector<TypeId>>
    {
        std::vector<TypeId> toBlock;
        // we need a more complex check for blocking on the discriminant in particular
        FindRefinementBlockers frb;
        frb.traverse(discriminant);

        if (!frb.found.empty())
            return {nullptr, {frb.found.begin(), frb.found.end()}};

        if (FFlag::DebugLuauEqSatSimplification)
        {
            auto simplifyResult = eqSatSimplify(ctx->simplifier, ctx->arena->addType(IntersectionType{{target, discriminant}}));
            if (simplifyResult)
            {
                if (ctx->solver)
                {
                    for (TypeId newTf : simplifyResult->newTypeFunctions)
                        ctx->pushConstraint(ReduceConstraint{newTf});
                }

                return {simplifyResult->result, {}};
            }
            else
                return {nullptr, {}};
        }
        else
        {
            // If the discriminant type is only:
            // - The `*no-refine*` type or,
            // - tables, metatables, unions, intersections, functions, or negations _containing_ `*no-refine*`.
            // There's no point in refining against it.
            ContainsRefinableType crt;
            crt.traverse(discriminant);
            if (!crt.found)
                return {target, {}};

            if (FFlag::LuauRefineTablesWithReadType)
            {
                if (auto ty = intersectWithSimpleDiscriminant(ctx->builtins, ctx->arena, target, discriminant))
                    return {*ty, {}};
            }

            // NOTE: This block causes us to refine too early in some cases.
            if (auto negation = get<NegationType>(discriminant))
            {
                if (auto primitive = get<PrimitiveType>(follow(negation->ty)); primitive && primitive->type == PrimitiveType::NilType)
                {
                    SimplifyResult result = simplifyIntersection(ctx->builtins, ctx->arena, target, discriminant);
                    return {result.result, {}};
                }
            }

            // If the target type is a table, then simplification already implements the logic to deal with refinements properly since the
            // type of the discriminant is guaranteed to only ever be an (arbitrarily-nested) table of a single property type.
            // We also fire for simple discriminants such as false? and ~(false?): the falsy and truthy types respectively.
            if (is<TableType>(target) || isTruthyOrFalsyType(discriminant))
            {
                SimplifyResult result = simplifyIntersection(ctx->builtins, ctx->arena, target, discriminant);
                if (FFlag::LuauEagerGeneralization4)
                {
                    // Simplification considers free and generic types to be
                    // 'blocking', but that's not suitable for refine<>.
                    //
                    // If we are only blocked on those types, we consider
                    // the simplification a success and reduce.
                    if (std::all_of(
                            begin(result.blockedTypes),
                            end(result.blockedTypes),
                            [](auto&& v)
                            {
                                return is<FreeType, GenericType>(follow(v));
                            }
                        ))
                    {
                        return {result.result, {}};
                    }
                    else
                        return {nullptr, {result.blockedTypes.begin(), result.blockedTypes.end()}};
                }
                else
                {
                    if (!result.blockedTypes.empty())
                        return {nullptr, {result.blockedTypes.begin(), result.blockedTypes.end()}};
                }
                return {result.result, {}};
            }


            // In the general case, we'll still use normalization though.
            TypeId intersection = ctx->arena->addType(IntersectionType{{target, discriminant}});
            std::shared_ptr<const NormalizedType> normIntersection = ctx->normalizer->normalize(intersection);
            std::shared_ptr<const NormalizedType> normType = ctx->normalizer->normalize(target);

            // if the intersection failed to normalize, we can't reduce, but know nothing about inhabitance.
            if (!normIntersection || !normType)
                return {nullptr, {}};

            TypeId resultTy = ctx->normalizer->typeFromNormal(*normIntersection);
            // include the error type if the target type is error-suppressing and the intersection we computed is not
            if (normType->shouldSuppressErrors() && !normIntersection->shouldSuppressErrors())
                resultTy = ctx->arena->addType(UnionType{{resultTy, ctx->builtins->errorType}});

            return {resultTy, {}};
        }

    };

    // refine target with each discriminant type in sequence (reverse of insertion order)
    // If we cannot proceed, block. If all discriminant types refine successfully, return
    // the result
    TypeId target = targetTy;
    while (!discriminantTypes.empty())
    {
        TypeId discriminant = discriminantTypes.back();
        auto [refined, blocked] = stepRefine(target, discriminant);

        if (blocked.empty() && refined == nullptr)
            return {std::nullopt, Reduction::MaybeOk, {}, {}};

        if (!blocked.empty())
            return {std::nullopt, Reduction::MaybeOk, blocked, {}};

        target = refined;
        discriminantTypes.pop_back();
    }
    return {target, Reduction::MaybeOk, {}, {}};
}

TypeFunctionReductionResult<TypeId> singletonTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (typeParams.size() != 1 || !packParams.empty())
    {
        ctx->ice->ice("singleton type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    TypeId type = follow(typeParams.at(0));

    // check to see if both operand types are resolved enough, and wait to reduce if not
    if (isPending(type, ctx->solver))
        return {std::nullopt, Reduction::MaybeOk, {type}, {}};

    TypeId followed = type;
    // we want to follow through a negation here as well.
    if (auto negation = get<NegationType>(followed))
        followed = follow(negation->ty);

    // if we have a singleton type or `nil`, which is its own singleton type...
    if (get<SingletonType>(followed) || isNil(followed))
        return {type, Reduction::MaybeOk, {}, {}};

    // otherwise, we'll return the top type, `unknown`.
    return {ctx->builtins->unknownType, Reduction::MaybeOk, {}, {}};
}

struct CollectUnionTypeOptions : TypeOnceVisitor
{
    NotNull<TypeFunctionContext> ctx;
    DenseHashSet<TypeId> options{nullptr};
    DenseHashSet<TypeId> blockingTypes{nullptr};

    explicit CollectUnionTypeOptions(NotNull<TypeFunctionContext> ctx)
        : TypeOnceVisitor(/* skipBoundTypes */ true)
        , ctx(ctx)
    {
    }

    bool visit(TypeId ty) override
    {
        options.insert(ty);
        if (isPending(ty, ctx->solver))
            blockingTypes.insert(ty);
        return false;
    }

    bool visit(TypePackId tp) override
    {
        return false;
    }

    bool visit(TypeId ty, const UnionType& ut) override
    {
        // If we have something like:
        //
        //  union<A | B, C | D>
        //
        // We probably just want to consider this to be the same as
        //
        //   union<A, B, C, D>
        return true;
    }

    bool visit(TypeId ty, const TypeFunctionInstanceType& tfit) override
    {
        if (tfit.function->name != builtinTypeFunctions().unionFunc.name)
        {
            options.insert(ty);
            blockingTypes.insert(ty);
            return false;
        }
        return true;
    }
};

TypeFunctionReductionResult<TypeId> unionTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (!packParams.empty())
    {
        ctx->ice->ice("union type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    // if we only have one parameter, there's nothing to do.
    if (typeParams.size() == 1)
        return {follow(typeParams[0]), Reduction::MaybeOk, {}, {}};


    CollectUnionTypeOptions collector{ctx};
    collector.traverse(instance);

    if (!collector.blockingTypes.empty())
    {
        std::vector<TypeId> blockingTypes{collector.blockingTypes.begin(), collector.blockingTypes.end()};
        return {std::nullopt, Reduction::MaybeOk, std::move(blockingTypes), {}};
    }

    TypeId resultTy = ctx->builtins->neverType;
    for (auto ty : collector.options)
    {
        SimplifyResult result = simplifyUnion(ctx->builtins, ctx->arena, resultTy, ty);
        // This condition might fire if one of the arguments to this type
        // function is a free type somewhere deep in a nested union or
        // intersection type, even though we ran a pass above to capture
        // some blocked types.
        if (!result.blockedTypes.empty())
            return {std::nullopt, Reduction::MaybeOk, {result.blockedTypes.begin(), result.blockedTypes.end()}, {}};

        resultTy = result.result;
    }

    return {resultTy, Reduction::MaybeOk, {}, {}};
}


TypeFunctionReductionResult<TypeId> intersectTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (!packParams.empty())
    {
        ctx->ice->ice("intersect type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    // if we only have one parameter, there's nothing to do.
    if (typeParams.size() == 1)
        return {follow(typeParams[0]), Reduction::MaybeOk, {}, {}};

    // we need to follow all of the type parameters.
    std::vector<TypeId> types;
    types.reserve(typeParams.size());
    for (auto ty : typeParams)
        types.emplace_back(follow(ty));

    // if we only have two parameters and one is `*no-refine*`, we're all done.
    if (types.size() == 2 && get<NoRefineType>(types[1]))
        return {types[0], Reduction::MaybeOk, {}, {}};
    else if (types.size() == 2 && get<NoRefineType>(types[0]))
        return {types[1], Reduction::MaybeOk, {}, {}};

    // check to see if the operand types are resolved enough, and wait to reduce if not
    // if any of them are `never`, the intersection will always be `never`, so we can reduce directly.
    for (auto ty : types)
    {
        if (isPending(ty, ctx->solver))
            return {std::nullopt, Reduction::MaybeOk, {ty}, {}};
        else if (get<NeverType>(ty))
            return {ctx->builtins->neverType, Reduction::MaybeOk, {}, {}};
    }

    // fold over the types with `simplifyIntersection`
    TypeId resultTy = ctx->builtins->unknownType;
    // collect types which caused intersection to return never
    DenseHashSet<TypeId> unintersectableTypes{nullptr};
    for (auto ty : types)
    {
        // skip any `*no-refine*` types.
        if (get<NoRefineType>(ty))
            continue;

        if (FFlag::LuauRefineTablesWithReadType)
        {
            if (auto simpleResult = intersectWithSimpleDiscriminant(ctx->builtins, ctx->arena, resultTy, ty))
            {
                if (get<NeverType>(*simpleResult))
                    unintersectableTypes.insert(follow(ty));
                else
                    resultTy = *simpleResult;
                continue;
            }
        }

        SimplifyResult result = simplifyIntersection(ctx->builtins, ctx->arena, resultTy, ty);

        // If simplifying the intersection returned never, note the type we tried to intersect it with, and continue trying to intersect with the
        // rest
        if (get<NeverType>(result.result))
        {
            unintersectableTypes.insert(follow(ty));
            continue;
        }
        for (TypeId blockedType : result.blockedTypes)
        {
            if (!get<GenericType>(blockedType))
                return {std::nullopt, Reduction::MaybeOk, {result.blockedTypes.begin(), result.blockedTypes.end()}, {}};
        }

        resultTy = result.result;
    }

    if (!unintersectableTypes.empty())
    {
        unintersectableTypes.insert(resultTy);
        if (unintersectableTypes.size() > 1)
        {
            TypeId intersection =
                ctx->arena->addType(IntersectionType{std::vector<TypeId>(unintersectableTypes.begin(), unintersectableTypes.end())});
            return {intersection, Reduction::MaybeOk, {}, {}};
        }
        else
        {
            return {*unintersectableTypes.begin(), Reduction::MaybeOk, {}, {}};
        }
    }
    // if the intersection simplifies to `never`, this gives us bad autocomplete.
    // we'll just produce the intersection plainly instead, but this might be revisitable
    // if we ever give `never` some kind of "explanation" trail.
    if (get<NeverType>(resultTy))
    {
        TypeId intersection = ctx->arena->addType(IntersectionType{typeParams});
        return {intersection, Reduction::MaybeOk, {}, {}};
    }

    return {resultTy, Reduction::MaybeOk, {}, {}};
}

// computes the keys of `ty` into `result`
// `isRaw` parameter indicates whether or not we should follow __index metamethods
// returns `false` if `result` should be ignored because the answer is "all strings"
bool computeKeysOf_DEPRECATED(TypeId ty, Set<std::string>& result, DenseHashSet<TypeId>& seen, bool isRaw, NotNull<TypeFunctionContext> ctx)
{
    // if the type is the top table type, the answer is just "all strings"
    if (get<PrimitiveType>(ty))
        return false;

    // if we've already seen this type, we can do nothing
    if (seen.contains(ty))
        return true;
    seen.insert(ty);

    // if we have a particular table type, we can insert the keys
    if (auto tableTy = get<TableType>(ty))
    {
        if (tableTy->indexer)
        {
            // if we have a string indexer, the answer is, again, "all strings"
            if (isString(tableTy->indexer->indexType))
                return false;
        }

        for (auto [key, _] : tableTy->props)
            result.insert(key);
        return true;
    }

    // otherwise, we have a metatable to deal with
    if (auto metatableTy = get<MetatableType>(ty))
    {
        bool res = true;

        if (!isRaw)
        {
            // findMetatableEntry demands the ability to emit errors, so we must give it
            // the necessary state to do that, even if we intend to just eat the errors.
            ErrorVec dummy;

            std::optional<TypeId> mmType = findMetatableEntry(ctx->builtins, dummy, ty, "__index", Location{});
            if (mmType)
                res = res && computeKeysOf_DEPRECATED(*mmType, result, seen, isRaw, ctx);
        }

        res = res && computeKeysOf_DEPRECATED(metatableTy->table, result, seen, isRaw, ctx);

        return res;
    }

    if (auto classTy = get<ExternType>(ty))
    {
        for (auto [key, _] : classTy->props) // NOLINT(performance-for-range-copy)
            result.insert(key);

        bool res = true;
        if (classTy->metatable && !isRaw)
        {
            // findMetatableEntry demands the ability to emit errors, so we must give it
            // the necessary state to do that, even if we intend to just eat the errors.
            ErrorVec dummy;

            std::optional<TypeId> mmType = findMetatableEntry(ctx->builtins, dummy, ty, "__index", Location{});
            if (mmType)
                res = res && computeKeysOf_DEPRECATED(*mmType, result, seen, isRaw, ctx);
        }

        if (classTy->parent)
            res = res && computeKeysOf_DEPRECATED(follow(*classTy->parent), result, seen, isRaw, ctx);

        return res;
    }

    // this should not be reachable since the type should be a valid tables or extern types part from normalization.
    LUAU_ASSERT(false);
    return false;
}

namespace {

/**
 * Computes the keys of `ty` into `result`
 * `isRaw` parameter indicates whether or not we should follow __index metamethods
 * returns `false` if `result` should be ignored because the answer is "all strings"
 */
bool computeKeysOf(TypeId ty, Set<std::optional<std::string>>& result, DenseHashSet<TypeId>& seen, bool isRaw, NotNull<TypeFunctionContext> ctx)
{

    // if the type is the top table type, the answer is just "all strings"
    if (get<PrimitiveType>(ty))
        return false;

    // if we've already seen this type, we can do nothing
    if (seen.contains(ty))
        return true;
    seen.insert(ty);

    // if we have a particular table type, we can insert the keys
    if (auto tableTy = get<TableType>(ty))
    {
        if (tableTy->indexer)
        {
            // if we have a string indexer, the answer is, again, "all strings"
            if (isString(tableTy->indexer->indexType))
                return false;
        }

        for (const auto& [key, _] : tableTy->props)
            result.insert(key);
        return true;
    }

    // otherwise, we have a metatable to deal with
    if (auto metatableTy = get<MetatableType>(ty))
    {
        bool res = true;

        if (!isRaw)
        {
            // findMetatableEntry demands the ability to emit errors, so we must give it
            // the necessary state to do that, even if we intend to just eat the errors.
            ErrorVec dummy;

            std::optional<TypeId> mmType = findMetatableEntry(ctx->builtins, dummy, ty, "__index", Location{});
            if (mmType)
                res = res && computeKeysOf(*mmType, result, seen, isRaw, ctx);
        }

        res = res && computeKeysOf(metatableTy->table, result, seen, isRaw, ctx);

        return res;
    }

    if (auto classTy = get<ExternType>(ty))
    {
        for (const auto& [key, _] : classTy->props)
            result.insert(key);

        bool res = true;
        if (classTy->metatable && !isRaw)
        {
            // findMetatableEntry demands the ability to emit errors, so we must give it
            // the necessary state to do that, even if we intend to just eat the errors.
            ErrorVec dummy;

            std::optional<TypeId> mmType = findMetatableEntry(ctx->builtins, dummy, ty, "__index", Location{});
            if (mmType)
                res = res && computeKeysOf(*mmType, result, seen, isRaw, ctx);
        }

        if (classTy->parent)
            res = res && computeKeysOf(follow(*classTy->parent), result, seen, isRaw, ctx);

        return res;
    }

    // this should not be reachable since the type should be a valid tables or extern types part from normalization.
    LUAU_ASSERT(false);
    return false;
}

}

TypeFunctionReductionResult<TypeId> keyofFunctionImpl(
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx,
    bool isRaw
)
{
    if (typeParams.size() != 1 || !packParams.empty())
    {
        ctx->ice->ice("keyof type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    TypeId operandTy = follow(typeParams.at(0));

    std::shared_ptr<const NormalizedType> normTy = ctx->normalizer->normalize(operandTy);

    // if the operand failed to normalize, we can't reduce, but know nothing about inhabitance.
    if (!normTy)
        return {std::nullopt, Reduction::MaybeOk, {}, {}};

    // if we don't have either just tables or just extern types, we've got nothing to get keys of (at least until a future version perhaps adds extern
    // types as well)
    if (normTy->hasTables() == normTy->hasExternTypes())
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    // this is sort of atrocious, but we're trying to reject any type that has not normalized to a table or a union of tables.
    if (normTy->hasTops() || normTy->hasBooleans() || normTy->hasErrors() || normTy->hasNils() || normTy->hasNumbers() || normTy->hasStrings() ||
        normTy->hasThreads() || normTy->hasBuffers() || normTy->hasFunctions() || normTy->hasTyvars())
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    if (FFlag::LuauEmptyStringInKeyOf)
    {
        // We're going to collect the keys in here, and we use optional strings
        // so that we can differentiate between the empty string and _no_ string.
        Set<std::optional<std::string>> keys{std::nullopt};

        // computing the keys for extern types
        if (normTy->hasExternTypes())
        {
            LUAU_ASSERT(!normTy->hasTables());

            // seen set for key computation for extern types
            DenseHashSet<TypeId> seen{{}};

            auto externTypeIter = normTy->externTypes.ordering.begin();
            auto externTypeIterEnd = normTy->externTypes.ordering.end();
            LUAU_ASSERT(externTypeIter != externTypeIterEnd); // should be guaranteed by the `hasExternTypes` check earlier

            // collect all the properties from the first class type
            if (!computeKeysOf(*externTypeIter, keys, seen, isRaw, ctx))
                return {ctx->builtins->stringType, Reduction::MaybeOk, {}, {}}; // if it failed, we have a top type!

            // we need to look at each class to remove any keys that are not common amongst them all
            while (++externTypeIter != externTypeIterEnd)
            {
                seen.clear(); // we'll reuse the same seen set

                Set<std::optional<std::string>> localKeys{std::nullopt};

                // we can skip to the next class if this one is a top type
                if (!computeKeysOf(*externTypeIter, localKeys, seen, isRaw, ctx))
                    continue;

                for (auto& key : keys)
                {
                    // remove any keys that are not present in each class
                    if (!localKeys.contains(key))
                        keys.erase(key);
                }
            }
        }

        // computing the keys for tables
        if (normTy->hasTables())
        {
            LUAU_ASSERT(!normTy->hasExternTypes());

            // seen set for key computation for tables
            DenseHashSet<TypeId> seen{{}};

            auto tablesIter = normTy->tables.begin();
            LUAU_ASSERT(tablesIter != normTy->tables.end()); // should be guaranteed by the `hasTables` check earlier

            // collect all the properties from the first table type
            if (!computeKeysOf(*tablesIter, keys, seen, isRaw, ctx))
                return {ctx->builtins->stringType, Reduction::MaybeOk, {}, {}}; // if it failed, we have the top table type!

            // we need to look at each tables to remove any keys that are not common amongst them all
            while (++tablesIter != normTy->tables.end())
            {
                seen.clear(); // we'll reuse the same seen set

                Set<std::optional<std::string>> localKeys{std::nullopt};

                // we can skip to the next table if this one is the top table type
                if (!computeKeysOf(*tablesIter, localKeys, seen, isRaw, ctx))
                    continue;

                for (auto& key : keys)
                {
                    // remove any keys that are not present in each table
                    if (!localKeys.contains(key))
                        keys.erase(key);
                }
            }
        }

        // if the set of keys is empty, `keyof<T>` is `never`
        if (keys.empty())
            return {ctx->builtins->neverType, Reduction::MaybeOk, {}, {}};

        // everything is validated, we need only construct our big union of singletons now!
        std::vector<TypeId> singletons;
        singletons.reserve(keys.size());

        for (const auto& key : keys)
        {
            if (key)
                singletons.push_back(ctx->arena->addType(SingletonType{StringSingleton{*key}}));
        }

        // If there's only one entry, we don't need a UnionType.
        // We can take straight take it from the first entry
        // because it was added into the type arena already.
        if (singletons.size() == 1)
            return {singletons.front(), Reduction::MaybeOk, {}, {}};

        return {ctx->arena->addType(UnionType{std::move(singletons)}), Reduction::MaybeOk, {}, {}};
    }
    else
    {

        // we're going to collect the keys in here
        Set<std::string> keys{{}};

        // computing the keys for extern types
        if (normTy->hasExternTypes())
        {
            LUAU_ASSERT(!normTy->hasTables());

            // seen set for key computation for extern types
            DenseHashSet<TypeId> seen{{}};

            auto externTypeIter = normTy->externTypes.ordering.begin();
            auto externTypeIterEnd = normTy->externTypes.ordering.end();
            LUAU_ASSERT(externTypeIter != externTypeIterEnd); // should be guaranteed by the `hasExternTypes` check earlier

            // collect all the properties from the first class type
            if (!computeKeysOf_DEPRECATED(*externTypeIter, keys, seen, isRaw, ctx))
                return {ctx->builtins->stringType, Reduction::MaybeOk, {}, {}}; // if it failed, we have a top type!

            // we need to look at each class to remove any keys that are not common amongst them all
            while (++externTypeIter != externTypeIterEnd)
            {
                seen.clear(); // we'll reuse the same seen set

                Set<std::string> localKeys{{}};

                // we can skip to the next class if this one is a top type
                if (!computeKeysOf_DEPRECATED(*externTypeIter, localKeys, seen, isRaw, ctx))
                    continue;

                for (auto& key : keys)
                {
                    // remove any keys that are not present in each class
                    if (!localKeys.contains(key))
                        keys.erase(key);
                }
            }
        }

        // computing the keys for tables
        if (normTy->hasTables())
        {
            LUAU_ASSERT(!normTy->hasExternTypes());

            // seen set for key computation for tables
            DenseHashSet<TypeId> seen{{}};

            auto tablesIter = normTy->tables.begin();
            LUAU_ASSERT(tablesIter != normTy->tables.end()); // should be guaranteed by the `hasTables` check earlier

            // collect all the properties from the first table type
            if (!computeKeysOf_DEPRECATED(*tablesIter, keys, seen, isRaw, ctx))
                return {ctx->builtins->stringType, Reduction::MaybeOk, {}, {}}; // if it failed, we have the top table type!

            // we need to look at each tables to remove any keys that are not common amongst them all
            while (++tablesIter != normTy->tables.end())
            {
                seen.clear(); // we'll reuse the same seen set

                Set<std::string> localKeys{{}};

                // we can skip to the next table if this one is the top table type
                if (!computeKeysOf_DEPRECATED(*tablesIter, localKeys, seen, isRaw, ctx))
                    continue;

                for (auto& key : keys)
                {
                    // remove any keys that are not present in each table
                    if (!localKeys.contains(key))
                        keys.erase(key);
                }
            }
        }

        // if the set of keys is empty, `keyof<T>` is `never`
        if (keys.empty())
            return {ctx->builtins->neverType, Reduction::MaybeOk, {}, {}};

        // everything is validated, we need only construct our big union of singletons now!
        std::vector<TypeId> singletons;
        singletons.reserve(keys.size());

        for (const std::string& key : keys)
            singletons.push_back(ctx->arena->addType(SingletonType{StringSingleton{key}}));

        // If there's only one entry, we don't need a UnionType.
        // We can take straight take it from the first entry
        // because it was added into the type arena already.
        if (singletons.size() == 1)
            return {singletons.front(), Reduction::MaybeOk, {}, {}};

        return {ctx->arena->addType(UnionType{std::move(singletons)}), Reduction::MaybeOk, {}, {}};
    }
}

TypeFunctionReductionResult<TypeId> keyofTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (typeParams.size() != 1 || !packParams.empty())
    {
        ctx->ice->ice("keyof type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    return keyofFunctionImpl(typeParams, packParams, ctx, /* isRaw */ false);
}

TypeFunctionReductionResult<TypeId> rawkeyofTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (typeParams.size() != 1 || !packParams.empty())
    {
        ctx->ice->ice("rawkeyof type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    return keyofFunctionImpl(typeParams, packParams, ctx, /* isRaw */ true);
}

/* Searches through table's or class's props/indexer to find the property of `ty`
   If found, appends that property to `result` and returns true
   Else, returns false */
bool searchPropsAndIndexer(
    TypeId ty,
    TableType::Props tblProps,
    std::optional<TableIndexer> tblIndexer,
    DenseHashSet<TypeId>& result,
    NotNull<TypeFunctionContext> ctx
)
{
    ty = follow(ty);

    // index into tbl's properties
    if (auto stringSingleton = get<StringSingleton>(get<SingletonType>(ty)))
    {
        if (tblProps.find(stringSingleton->value) != tblProps.end())
        {

            TypeId propTy;
            if (FFlag::LuauRemoveTypeCallsForReadWriteProps)
            {
                Property& prop = tblProps.at(stringSingleton->value);

                if (prop.readTy)
                    propTy = follow(*prop.readTy);
                else if (prop.writeTy)
                    propTy = follow(*prop.writeTy);
                else // found the property, but there was no type associated with it
                    return false;
            }
            else
                propTy = follow(tblProps.at(stringSingleton->value).type_DEPRECATED());

            // property is a union type -> we need to extend our reduction type
            if (auto propUnionTy = get<UnionType>(propTy))
            {
                for (TypeId option : propUnionTy->options)
                {
                    result.insert(follow(option));
                }
            }
            else // property is a singular type or intersection type -> we can simply append
                result.insert(propTy);

            return true;
        }
    }

    // index into tbl's indexer
    if (tblIndexer)
    {
        TypeId indexType = follow(tblIndexer->indexType);

        if (auto tfit = get<TypeFunctionInstanceType>(indexType))
        {
            // if we have an index function here, it means we're in a cycle, so let's see if it's well-founded if we tie the knot
            if (tfit->function.get() == &builtinTypeFunctions().indexFunc)
                indexType = follow(tblIndexer->indexResultType);
        }

        if (isSubtype(ty, indexType, ctx->scope, ctx->builtins, ctx->simplifier, *ctx->ice, SolverMode::New))
        {
            TypeId idxResultTy = follow(tblIndexer->indexResultType);

            // indexResultType is a union type -> we need to extend our reduction type
            if (auto idxResUnionTy = get<UnionType>(idxResultTy))
            {
                for (TypeId option : idxResUnionTy->options)
                {
                    result.insert(follow(option));
                }
            }
            else // indexResultType is a singular type or intersection type -> we can simply append
                result.insert(idxResultTy);

            return true;
        }
    }

    return false;
}

bool tblIndexInto(
    TypeId indexer,
    TypeId indexee,
    DenseHashSet<TypeId>& result,
    DenseHashSet<TypeId>& seenSet,
    NotNull<TypeFunctionContext> ctx,
    bool isRaw
)
{
    indexer = follow(indexer);
    indexee = follow(indexee);

    if (seenSet.contains(indexee))
        return false;
    seenSet.insert(indexee);

    if (auto unionTy = get<UnionType>(indexee))
    {
        bool res = true;
        for (auto component : unionTy)
        {
            // if the component is in the seen set and isn't the indexee itself,
            // we can skip it cause it means we encountered it in an earlier component in the union.
            if (seenSet.contains(component) && component != indexee)
                continue;

            res = res && tblIndexInto(indexer, component, result, seenSet, ctx, isRaw);
        }
        return res;
    }

    if (get<FunctionType>(indexee))
    {
        TypePackId argPack = ctx->arena->addTypePack({indexer});
        SolveResult solveResult = solveFunctionCall(
            ctx->arena,
            ctx->builtins,
            ctx->simplifier,
            ctx->normalizer,
            ctx->typeFunctionRuntime,
            ctx->ice,
            ctx->limits,
            ctx->scope,
            ctx->scope->location,
            indexee,
            argPack
        );

        if (!solveResult.typePackId.has_value())
            return false;

        TypePack extracted = extendTypePack(*ctx->arena, ctx->builtins, *solveResult.typePackId, 1);
        if (extracted.head.empty())
            return false;

        result.insert(follow(extracted.head.front()));
        return true;
    }

    // we have a table type to try indexing
    if (auto tableTy = get<TableType>(indexee))
    {
        return searchPropsAndIndexer(indexer, tableTy->props, tableTy->indexer, result, ctx);
    }

    // we have a metatable type to try indexing
    if (auto metatableTy = get<MetatableType>(indexee))
    {
        if (auto tableTy = get<TableType>(follow(metatableTy->table)))
        {

            // try finding all properties within the current scope of the table
            if (searchPropsAndIndexer(indexer, tableTy->props, tableTy->indexer, result, ctx))
                return true;
        }

        // if the code reached here, it means we weren't able to find all properties -> look into __index metamethod
        if (!isRaw)
        {
            // findMetatableEntry demands the ability to emit errors, so we must give it
            // the necessary state to do that, even if we intend to just eat the errors.
            ErrorVec dummy;
            std::optional<TypeId> mmType = findMetatableEntry(ctx->builtins, dummy, indexee, "__index", Location{});
            if (mmType)
                return tblIndexInto(indexer, *mmType, result, seenSet, ctx, isRaw);
        }
    }

    return false;
}

bool tblIndexInto(TypeId indexer, TypeId indexee, DenseHashSet<TypeId>& result, NotNull<TypeFunctionContext> ctx, bool isRaw)
{
    DenseHashSet<TypeId> seenSet{{}};
    return tblIndexInto(indexer, indexee, result, seenSet, ctx, isRaw);
}

/* Vocabulary note: indexee refers to the type that contains the properties,
                    indexer refers to the type that is used to access indexee
   Example:         index<Person, "name"> => `Person` is the indexee and `"name"` is the indexer */
TypeFunctionReductionResult<TypeId> indexFunctionImpl(
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx,
    bool isRaw
)
{
    TypeId indexeeTy = follow(typeParams.at(0));

    if (isPending(indexeeTy, ctx->solver))
        return {std::nullopt, Reduction::MaybeOk, {indexeeTy}, {}};

    std::shared_ptr<const NormalizedType> indexeeNormTy = ctx->normalizer->normalize(indexeeTy);

    // if the indexee failed to normalize, we can't reduce, but know nothing about inhabitance.
    if (!indexeeNormTy)
        return {std::nullopt, Reduction::MaybeOk, {}, {}};

    // if the indexee is `any`, then indexing also gives us `any`.
    if (indexeeNormTy->shouldSuppressErrors())
        return {ctx->builtins->anyType, Reduction::MaybeOk, {}, {}};

    // if we don't have either just tables or just extern types, we've got nothing to index into
    if (indexeeNormTy->hasTables() == indexeeNormTy->hasExternTypes())
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    // we're trying to reject any type that has not normalized to a table or extern type or a union of tables or extern types.
    if (indexeeNormTy->hasTops() || indexeeNormTy->hasBooleans() || indexeeNormTy->hasErrors() || indexeeNormTy->hasNils() ||
        indexeeNormTy->hasNumbers() || indexeeNormTy->hasStrings() || indexeeNormTy->hasThreads() || indexeeNormTy->hasBuffers() ||
        indexeeNormTy->hasFunctions() || indexeeNormTy->hasTyvars())
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    TypeId indexerTy = follow(typeParams.at(1));

    if (isPending(indexerTy, ctx->solver))
        return {std::nullopt, Reduction::MaybeOk, {indexerTy}, {}};

    std::shared_ptr<const NormalizedType> indexerNormTy = ctx->normalizer->normalize(indexerTy);

    // if the indexer failed to normalize, we can't reduce, but know nothing about inhabitance.
    if (!indexerNormTy)
        return {std::nullopt, Reduction::MaybeOk, {}, {}};

    // we're trying to reject any type that is not a string singleton or primitive (string, number, boolean, thread, nil, function, table, or buffer)
    if (indexerNormTy->hasTops() || indexerNormTy->hasErrors())
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    // indexer can be a union —> break them down into a vector
    const std::vector<TypeId>* typesToFind = nullptr;
    const std::vector<TypeId> singleType{indexerTy};
    if (auto unionTy = get<UnionType>(indexerTy))
        typesToFind = &unionTy->options;
    else
        typesToFind = &singleType;

    DenseHashSet<TypeId> properties{{}}; // vector of types that will be returned

    if (indexeeNormTy->hasExternTypes())
    {
        LUAU_ASSERT(!indexeeNormTy->hasTables());

        if (isRaw) // rawget should never reduce for extern types (to match the behavior of the rawget global function)
            return {std::nullopt, Reduction::Erroneous, {}, {}};

        // at least one class is guaranteed to be in the iterator by .hasExternTypes()
        for (auto externTypeIter = indexeeNormTy->externTypes.ordering.begin(); externTypeIter != indexeeNormTy->externTypes.ordering.end();
             ++externTypeIter)
        {
            auto externTy = get<ExternType>(*externTypeIter);
            if (!externTy)
            {
                LUAU_ASSERT(false); // this should not be possible according to normalization's spec
                return {std::nullopt, Reduction::Erroneous, {}, {}};
            }

            for (TypeId ty : *typesToFind)
            {
                // Search for all instances of indexer in class->props and class->indexer
                if (searchPropsAndIndexer(ty, externTy->props, externTy->indexer, properties, ctx))
                    continue; // Indexer was found in this class, so we can move on to the next

                auto parent = externTy->parent;
                bool foundInParent = false;
                while (parent && !foundInParent)
                {
                    auto parentExternType = get<ExternType>(follow(*parent));
                    foundInParent = searchPropsAndIndexer(ty, parentExternType->props, parentExternType->indexer, properties, ctx);
                    parent = parentExternType->parent;
                }

                // we move on to the next type if any of the parents we went through had the property.
                if (foundInParent)
                    continue;

                // If code reaches here,that means the property not found -> check in the metatable's __index

                // findMetatableEntry demands the ability to emit errors, so we must give it
                // the necessary state to do that, even if we intend to just eat the errors.
                ErrorVec dummy;
                std::optional<TypeId> mmType = findMetatableEntry(ctx->builtins, dummy, *externTypeIter, "__index", Location{});
                if (!mmType) // if a metatable does not exist, there is no where else to look
                    return {std::nullopt, Reduction::Erroneous, {}, {}};

                if (!tblIndexInto(ty, *mmType, properties, ctx, isRaw)) // if indexer is not in the metatable, we fail to reduce
                    return {std::nullopt, Reduction::Erroneous, {}, {}};
            }
        }
    }

    if (indexeeNormTy->hasTables())
    {
        LUAU_ASSERT(!indexeeNormTy->hasExternTypes());

        // at least one table is guaranteed to be in the iterator by .hasTables()
        for (auto tablesIter = indexeeNormTy->tables.begin(); tablesIter != indexeeNormTy->tables.end(); ++tablesIter)
        {
            for (TypeId ty : *typesToFind)
                if (!tblIndexInto(ty, *tablesIter, properties, ctx, isRaw))
                    return {std::nullopt, Reduction::Erroneous, {}, {}};
        }
    }

    // If the type being reduced to is a single type, no need to union
    if (properties.size() == 1)
        return {*properties.begin(), Reduction::MaybeOk, {}, {}};

    return {ctx->arena->addType(UnionType{std::vector<TypeId>(properties.begin(), properties.end())}), Reduction::MaybeOk, {}, {}};
}

TypeFunctionReductionResult<TypeId> indexTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (typeParams.size() != 2 || !packParams.empty())
    {
        ctx->ice->ice("index type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    return indexFunctionImpl(typeParams, packParams, ctx, /* isRaw */ false);
}

TypeFunctionReductionResult<TypeId> rawgetTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (typeParams.size() != 2 || !packParams.empty())
    {
        ctx->ice->ice("rawget type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    return indexFunctionImpl(typeParams, packParams, ctx, /* isRaw */ true);
}

TypeFunctionReductionResult<TypeId> setmetatableTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (typeParams.size() != 2 || !packParams.empty())
    {
        ctx->ice->ice("setmetatable type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    const Location location = ctx->constraint ? ctx->constraint->location : Location{};

    TypeId targetTy = follow(typeParams.at(0));
    TypeId metatableTy = follow(typeParams.at(1));

    std::shared_ptr<const NormalizedType> targetNorm = ctx->normalizer->normalize(targetTy);

    // if the operand failed to normalize, we can't reduce, but know nothing about inhabitance.
    if (!targetNorm)
        return {std::nullopt, Reduction::MaybeOk, {}, {}};

    // cannot setmetatable on something without table parts.
    if (!targetNorm->hasTables())
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    // we're trying to reject any type that has not normalized to a table or a union/intersection of tables.
    if (targetNorm->hasTops() || targetNorm->hasBooleans() || targetNorm->hasErrors() || targetNorm->hasNils() || targetNorm->hasNumbers() ||
        targetNorm->hasStrings() || targetNorm->hasThreads() || targetNorm->hasBuffers() || targetNorm->hasFunctions() || targetNorm->hasTyvars() ||
        targetNorm->hasExternTypes())
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    // if the supposed metatable is not a table, we will fail to reduce.
    if (!get<TableType>(metatableTy) && !get<MetatableType>(metatableTy))
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    if (targetNorm->tables.size() == 1)
    {
        TypeId table = *targetNorm->tables.begin();

        // findMetatableEntry demands the ability to emit errors, so we must give it
        // the necessary state to do that, even if we intend to just eat the errors.
        ErrorVec dummy;

        std::optional<TypeId> metatableMetamethod = findMetatableEntry(ctx->builtins, dummy, table, "__metatable", location);

        // if the `__metatable` metamethod is present, then the table is locked and we cannot `setmetatable` on it.
        if (metatableMetamethod)
            return {std::nullopt, Reduction::Erroneous, {}, {}};

        TypeId withMetatable = ctx->arena->addType(MetatableType{table, metatableTy});

        return {withMetatable, Reduction::MaybeOk, {}, {}};
    }

    TypeId result = ctx->builtins->neverType;

    for (auto componentTy : targetNorm->tables)
    {
        // findMetatableEntry demands the ability to emit errors, so we must give it
        // the necessary state to do that, even if we intend to just eat the errors.
        ErrorVec dummy;

        std::optional<TypeId> metatableMetamethod = findMetatableEntry(ctx->builtins, dummy, componentTy, "__metatable", location);

        // if the `__metatable` metamethod is present, then the table is locked and we cannot `setmetatable` on it.
        if (metatableMetamethod)
            return {std::nullopt, Reduction::Erroneous, {}, {}};

        TypeId withMetatable = ctx->arena->addType(MetatableType{componentTy, metatableTy});
        SimplifyResult simplified = simplifyUnion(ctx->builtins, ctx->arena, result, withMetatable);

        if (!simplified.blockedTypes.empty())
        {
            std::vector<TypeId> blockedTypes{};
            blockedTypes.reserve(simplified.blockedTypes.size());
            for (auto ty : simplified.blockedTypes)
                blockedTypes.push_back(ty);
            return {std::nullopt, Reduction::MaybeOk, std::move(blockedTypes), {}};
        }

        result = simplified.result;
    }

    return {result, Reduction::MaybeOk, {}, {}};
}

static TypeFunctionReductionResult<TypeId> getmetatableHelper(TypeId targetTy, const Location& location, NotNull<TypeFunctionContext> ctx)
{
    targetTy = follow(targetTy);

    std::optional<TypeId> result = std::nullopt;
    bool erroneous = true;

    if (auto table = get<TableType>(targetTy))
        erroneous = false;

    if (auto mt = get<MetatableType>(targetTy))
    {
        result = mt->metatable;
        erroneous = false;
    }

    if (auto clazz = get<ExternType>(targetTy))
    {
        result = clazz->metatable;
        erroneous = false;
    }

    if (auto primitive = get<PrimitiveType>(targetTy))
    {
        result = primitive->metatable;
        erroneous = false;
    }

    if (auto singleton = get<SingletonType>(targetTy))
    {
        if (get<StringSingleton>(singleton))
        {
            auto primitiveString = get<PrimitiveType>(ctx->builtins->stringType);
            result = primitiveString->metatable;
        }
        erroneous = false;
    }

    if (FFlag::LuauUpdateGetMetatableTypeSignature && get<AnyType>(targetTy))
    {
        // getmetatable<any> ~ any
        result = targetTy;
        erroneous = false;
    }

    if (erroneous)
        return {std::nullopt, Reduction::Erroneous, {}, {}};

    // findMetatableEntry demands the ability to emit errors, so we must give it
    // the necessary state to do that, even if we intend to just eat the errors.
    ErrorVec dummy;

    std::optional<TypeId> metatableMetamethod = findMetatableEntry(ctx->builtins, dummy, targetTy, "__metatable", location);

    if (metatableMetamethod)
        return {metatableMetamethod, Reduction::MaybeOk, {}, {}};

    if (result)
        return {result, Reduction::MaybeOk, {}, {}};

    return {ctx->builtins->nilType, Reduction::MaybeOk, {}, {}};
}

TypeFunctionReductionResult<TypeId> getmetatableTypeFunction(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (typeParams.size() != 1 || !packParams.empty())
    {
        ctx->ice->ice("getmetatable type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    const Location location = ctx->constraint ? ctx->constraint->location : Location{};

    TypeId targetTy = follow(typeParams.at(0));

    if (isPending(targetTy, ctx->solver))
        return {std::nullopt, Reduction::MaybeOk, {targetTy}, {}};

    if (auto ut = get<UnionType>(targetTy))
    {
        std::vector<TypeId> options{};
        options.reserve(ut->options.size());

        for (auto option : ut->options)
        {
            TypeFunctionReductionResult<TypeId> result = getmetatableHelper(option, location, ctx);

            if (!result.result)
                return result;

            options.push_back(*result.result);
        }

        return {ctx->arena->addType(UnionType{std::move(options)}), Reduction::MaybeOk, {}, {}};
    }

    if (auto it = get<IntersectionType>(targetTy))
    {
        std::vector<TypeId> parts{};
        parts.reserve(it->parts.size());

        bool erroredWithUnknown = false;

        for (auto part : it->parts)
        {
            TypeFunctionReductionResult<TypeId> result = getmetatableHelper(part, location, ctx);

            if (!result.result)
            {
                // Don't immediately error if part is unknown
                if (FFlag::LuauUpdateGetMetatableTypeSignature && get<UnknownType>(follow(part)))
                {
                    erroredWithUnknown = true;
                    continue;
                }
                else
                    return result;
            }

            parts.push_back(*result.result);
        }

        // If all parts are unknown, return erroneous reduction
        if (FFlag::LuauUpdateGetMetatableTypeSignature && erroredWithUnknown && parts.empty())
            return {std::nullopt, Reduction::Erroneous, {}, {}};

        if (FFlag::LuauUpdateGetMetatableTypeSignature && parts.size() == 1)
            return {parts.front(), Reduction::MaybeOk, {}, {}};

        return {ctx->arena->addType(IntersectionType{std::move(parts)}), Reduction::MaybeOk, {}, {}};
    }

    return getmetatableHelper(targetTy, location, ctx);
}

TypeFunctionReductionResult<TypeId> weakoptionalTypeFunc(
    TypeId instance,
    const std::vector<TypeId>& typeParams,
    const std::vector<TypePackId>& packParams,
    NotNull<TypeFunctionContext> ctx
)
{
    if (typeParams.size() != 1 || !packParams.empty())
    {
        ctx->ice->ice("weakoptional type function: encountered a type function instance without the required argument structure");
        LUAU_ASSERT(false);
    }

    TypeId targetTy = follow(typeParams.at(0));

    if (isPending(targetTy, ctx->solver))
        return {std::nullopt, Reduction::MaybeOk, {targetTy}, {}};

    if (is<NeverType>(instance))
        return {ctx->builtins->nilType, Reduction::MaybeOk, {}, {}};

    std::shared_ptr<const NormalizedType> targetNorm = ctx->normalizer->normalize(targetTy);

    if (!targetNorm)
        return {std::nullopt, Reduction::MaybeOk, {}, {}};

    auto result = ctx->normalizer->isInhabited(targetNorm.get());
    if (result == NormalizationResult::False)
        return {ctx->builtins->nilType, Reduction::MaybeOk, {}, {}};

    return {targetTy, Reduction::MaybeOk, {}, {}};
}

BuiltinTypeFunctions::BuiltinTypeFunctions()
    : userFunc{"user", userDefinedTypeFunction}
    , notFunc{"not", notTypeFunction}
    , lenFunc{"len", lenTypeFunction}
    , unmFunc{"unm", unmTypeFunction}
    , addFunc{"add", addTypeFunction}
    , subFunc{"sub", subTypeFunction}
    , mulFunc{"mul", mulTypeFunction}
    , divFunc{"div", divTypeFunction}
    , idivFunc{"idiv", idivTypeFunction}
    , powFunc{"pow", powTypeFunction}
    , modFunc{"mod", modTypeFunction}
    , concatFunc{"concat", concatTypeFunction}
    , andFunc{"and", andTypeFunction, /*canReduceGenerics*/ true}
    , orFunc{"or", orTypeFunction, /*canReduceGenerics*/ true}
    , ltFunc{"lt", ltTypeFunction}
    , leFunc{"le", leTypeFunction}
    , eqFunc{"eq", eqTypeFunction}
    , refineFunc{"refine", refineTypeFunction, /*canReduceGenerics*/ FFlag::LuauEagerGeneralization4}
    , singletonFunc{"singleton", singletonTypeFunction}
    , unionFunc{"union", unionTypeFunction}
    , intersectFunc{"intersect", intersectTypeFunction}
    , keyofFunc{"keyof", keyofTypeFunction}
    , rawkeyofFunc{"rawkeyof", rawkeyofTypeFunction}
    , indexFunc{"index", indexTypeFunction}
    , rawgetFunc{"rawget", rawgetTypeFunction}
    , setmetatableFunc{"setmetatable", setmetatableTypeFunction}
    , getmetatableFunc{"getmetatable", getmetatableTypeFunction}
    , weakoptionalFunc{"weakoptional", weakoptionalTypeFunc}
{
}

void BuiltinTypeFunctions::addToScope(NotNull<TypeArena> arena, NotNull<Scope> scope) const
{
    // make a type function for a one-argument type function
    auto mkUnaryTypeFunction = [&](const TypeFunction* tf)
    {
        TypeId t = arena->addType(GenericType{"T", Polarity::Negative});
        GenericTypeDefinition genericT{t};

        return TypeFun{{genericT}, arena->addType(TypeFunctionInstanceType{NotNull{tf}, {t}, {}})};
    };

    // make a type function for a two-argument type function with a default argument for the second type being the first
    auto mkBinaryTypeFunctionWithDefault = [&](const TypeFunction* tf)
    {
        TypeId t = arena->addType(GenericType{"T", Polarity::Negative});
        TypeId u = arena->addType(GenericType{"U", Polarity::Negative});
        GenericTypeDefinition genericT{t};
        GenericTypeDefinition genericU{u, {t}};

        return TypeFun{{genericT, genericU}, arena->addType(TypeFunctionInstanceType{NotNull{tf}, {t, u}, {}})};
    };

    // make a two-argument type function without the default arguments
    auto mkBinaryTypeFunction = [&](const TypeFunction* tf)
    {
        TypeId t = arena->addType(GenericType{"T", Polarity::Negative});
        TypeId u = arena->addType(GenericType{"U", Polarity::Negative});
        GenericTypeDefinition genericT{t};
        GenericTypeDefinition genericU{u};

        return TypeFun{{genericT, genericU}, arena->addType(TypeFunctionInstanceType{NotNull{tf}, {t, u}, {}})};
    };

    scope->exportedTypeBindings[lenFunc.name] = mkUnaryTypeFunction(&lenFunc);
    scope->exportedTypeBindings[unmFunc.name] = mkUnaryTypeFunction(&unmFunc);

    scope->exportedTypeBindings[addFunc.name] = mkBinaryTypeFunctionWithDefault(&addFunc);
    scope->exportedTypeBindings[subFunc.name] = mkBinaryTypeFunctionWithDefault(&subFunc);
    scope->exportedTypeBindings[mulFunc.name] = mkBinaryTypeFunctionWithDefault(&mulFunc);
    scope->exportedTypeBindings[divFunc.name] = mkBinaryTypeFunctionWithDefault(&divFunc);
    scope->exportedTypeBindings[idivFunc.name] = mkBinaryTypeFunctionWithDefault(&idivFunc);
    scope->exportedTypeBindings[powFunc.name] = mkBinaryTypeFunctionWithDefault(&powFunc);
    scope->exportedTypeBindings[modFunc.name] = mkBinaryTypeFunctionWithDefault(&modFunc);
    scope->exportedTypeBindings[concatFunc.name] = mkBinaryTypeFunctionWithDefault(&concatFunc);

    scope->exportedTypeBindings[ltFunc.name] = mkBinaryTypeFunctionWithDefault(&ltFunc);
    scope->exportedTypeBindings[leFunc.name] = mkBinaryTypeFunctionWithDefault(&leFunc);
    scope->exportedTypeBindings[eqFunc.name] = mkBinaryTypeFunctionWithDefault(&eqFunc);

    scope->exportedTypeBindings[keyofFunc.name] = mkUnaryTypeFunction(&keyofFunc);
    scope->exportedTypeBindings[rawkeyofFunc.name] = mkUnaryTypeFunction(&rawkeyofFunc);

    if (FFlag::LuauNotAllBinaryTypeFunsHaveDefaults)
    {
        scope->exportedTypeBindings[indexFunc.name] = mkBinaryTypeFunction(&indexFunc);
        scope->exportedTypeBindings[rawgetFunc.name] = mkBinaryTypeFunction(&rawgetFunc);
    }
    else
    {
        scope->exportedTypeBindings[indexFunc.name] = mkBinaryTypeFunctionWithDefault(&indexFunc);
        scope->exportedTypeBindings[rawgetFunc.name] = mkBinaryTypeFunctionWithDefault(&rawgetFunc);
    }

    if (FFlag::LuauNotAllBinaryTypeFunsHaveDefaults)
        scope->exportedTypeBindings[setmetatableFunc.name] = mkBinaryTypeFunction(&setmetatableFunc);
    else
        scope->exportedTypeBindings[setmetatableFunc.name] = mkBinaryTypeFunctionWithDefault(&setmetatableFunc);
    scope->exportedTypeBindings[getmetatableFunc.name] = mkUnaryTypeFunction(&getmetatableFunc);
}

const BuiltinTypeFunctions& builtinTypeFunctions()
{
    static std::unique_ptr<const BuiltinTypeFunctions> result = std::make_unique<BuiltinTypeFunctions>();

    return *result;
}

} // namespace Luau
