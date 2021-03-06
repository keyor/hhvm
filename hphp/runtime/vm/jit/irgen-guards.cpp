/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2014 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/
#include "hphp/runtime/vm/jit/irgen-guards.h"

#include "hphp/runtime/vm/jit/target-profile.h"

#include "hphp/runtime/vm/jit/irgen-exit.h"
#include "hphp/runtime/vm/jit/irgen-internal.h"

namespace HPHP { namespace jit { namespace irgen {

namespace {

//////////////////////////////////////////////////////////////////////

enum class ProfGuard { GuardLoc, CheckLoc, GuardStk, CheckStk };

/*
 * Emit a type guard, possibly using profiling information. Depending on the
 * current translation mode and type to be guarded, this function may emit
 * additional profiling code or modify the guarded type using previously
 * collected profiling information. Str -> StaticStr is the only supported
 * refinement for now.
 */
void profiledGuard(HTS& env,
                   Type type,
                   ProfGuard kind,
                   int32_t id, // locId or stackOff
                   Block* checkExit) {
  auto doGuard = [&] (Type type) {
    switch (kind) {
    case ProfGuard::CheckLoc:
    case ProfGuard::GuardLoc:
      if (auto failBlock = env.irb->guardFailBlock()) {
        gen(env, CheckLoc, type, LocalId(id), failBlock, fp(env));
      } else if (kind == ProfGuard::CheckLoc) {
        gen(env, CheckLoc, type, LocalId(id), checkExit, fp(env));
      } else {
        gen(env, GuardLoc, type, LocalId(id), fp(env), sp(env));
      }
      return;
    case ProfGuard::CheckStk:
    case ProfGuard::GuardStk:
      {
        // Adjust 'id' to get an offset from the current m_irb->sp().
        auto const adjOff = offsetFromSP(env, id);
        if (auto failBlock = env.irb->guardFailBlock()) {
          gen(env, CheckStk, type, StackOffset { adjOff }, failBlock, sp(env));
        } else if (kind == ProfGuard::CheckStk) {
          gen(env, CheckStk, type, StackOffset { adjOff }, checkExit, sp(env));
        } else {
          gen(env, GuardStk, type, StackOffset { adjOff }, sp(env), fp(env));
        }
        return;
      }
    }
  };

  auto loadAddr = [&]() -> SSATmp* {
    switch (kind) {
    case ProfGuard::CheckLoc:
    case ProfGuard::GuardLoc:
      return ldLocAddr(env, id);
    case ProfGuard::CheckStk:
    case ProfGuard::GuardStk:
      return ldStkAddr(env, id);
    }
    not_reached();
  };

  // We really do want to check for exact type equality here: if type
  // is StaticStr there's nothing for us to do, and we don't support
  // guarding on CountedStr.
  if (!RuntimeOption::EvalJitPGOStringSpec ||
      type != Type::Str ||
      (mcg->tx().mode() != TransKind::Profile &&
       mcg->tx().mode() != TransKind::Optimize)) {
    return doGuard(type);
  }

  auto const profileKey = [&] {
    switch (kind) {
    case ProfGuard::CheckLoc:
    case ProfGuard::GuardLoc:
      return makeStaticString(folly::to<std::string>("Loc", id));
    case ProfGuard::CheckStk:
    case ProfGuard::GuardStk:
      // Note that for stacks we are using a profiling key on the unadjusted
      // index (index from top of virtual stack).
      return makeStaticString(folly::to<std::string>("Stk", id));
    }
    not_reached();
  }();
  TargetProfile<StrProfile> profile(env.context,
                                    env.irb->curMarker(),
                                    profileKey);

  if (profile.profiling()) {
    doGuard(Type::Str);
    gen(env, ProfileStr, ProfileStrData { profileKey }, loadAddr());
    return;
  }

  if (profile.optimizing()) {
    auto const data = profile.data(StrProfile::reduce);
    auto const total = data.total();

    if (data.staticStr == total) doGuard(Type::StaticStr);
    else                         doGuard(Type::Str);
    return;
  }

  // TransLive: just do a normal guard.
  doGuard(Type::Str);
}

void guardTypeStack(HTS& env, uint32_t stackIndex, Type type, bool outerOnly) {
  assert(type <= Type::Gen);
  // This should only be called at the beginning of a trace, with a
  // clean stack
  assert(env.irb->evalStack().size() == 0);
  assert(env.irb->stackDeficit() == 0);
  auto const stackOff = StackOffset { offsetFromSP(env, stackIndex) };
  assert(type.isBoxed() || type.notBoxed());

  if (!type.isBoxed()) {
    profiledGuard(env, type, ProfGuard::GuardStk, stackIndex, nullptr);
    return;
  }

  profiledGuard(env, Type::BoxedInitCell, ProfGuard::GuardStk, stackIndex,
    nullptr);
  env.irb->constrainStack(stackOff.offset, DataTypeSpecific);
  gen(env, HintStkInner, type & Type::BoxedInitCell, stackOff, sp(env));

  if (!outerOnly && type.isBoxed() && type.unbox() < Type::Cell) {
    auto stk = gen(env, LdStk, Type::BoxedInitCell, stackOff, sp(env));
    gen(env,
        CheckRefInner,
        env.irb->stackInnerTypePrediction(stackOff.offset),
        makeExit(env),
        stk);
  }
}

void guardTypeLocal(HTS& env, uint32_t locId, Type type, bool outerOnly) {
  if (!type.isBoxed()) {
    profiledGuard(env, type, ProfGuard::GuardLoc, locId, nullptr);
    return;
  }

  profiledGuard(env, Type::BoxedInitCell, ProfGuard::GuardLoc, locId, nullptr);
  gen(env,
      HintLocInner,
      type & Type::BoxedInitCell,
      LocalId { locId },
      fp(env));

  if (!outerOnly && type.isBoxed() && type.unbox() < Type::Cell) {
    auto const ldrefExit = makeExit(env);
    auto const ldPMExit = makePseudoMainExit(env);
    auto const val = ldLoc(env, locId, ldPMExit, DataTypeSpecific);
    gen(env,
        CheckRefInner,
        env.irb->predictedInnerType(locId),
        ldrefExit,
        val);
  }
}

void checkTypeLocal(HTS& env, uint32_t locId, Type type, Offset dest) {
  if (!type.isBoxed()) {
    profiledGuard(env, type, ProfGuard::CheckLoc, locId, makeExit(env, dest));
    return;
  }

  profiledGuard(env, Type::BoxedInitCell, ProfGuard::CheckLoc, locId,
    makeExit(env, dest));
  gen(env,
      HintLocInner,
      type & Type::BoxedInitCell,
      LocalId { locId },
      fp(env));
}

uint64_t packBitVec(const std::vector<bool>& bits, unsigned i) {
  uint64_t retval = 0;
  assert(i % 64 == 0);
  assert(i < bits.size());
  while (i < bits.size()) {
    retval |= bits[i] << (i % 64);
    if ((++i % 64) == 0) {
      break;
    }
  }
  return retval;
}

void refCheckHelper(HTS& env,
                    int64_t entryArDelta,
                    const std::vector<bool>& mask,
                    const std::vector<bool>& vals,
                    Offset dest) {
  int32_t const actRecOff = entryArDelta + offsetFromSP(env, 0);
  auto const funcPtr = gen(
    env,
    LdARFuncPtr,
    StackOffset { actRecOff },
    sp(env)
  );
  SSATmp* nParams = nullptr;

  for (unsigned i = 0; i < mask.size(); i += 64) {
    assert(i < vals.size());

    uint64_t mask64 = packBitVec(mask, i);
    if (mask64 == 0) {
      continue;
    }

    if (i == 0) {
      nParams = cns(env, 64);
    } else if (!nParams || nParams->isConst()) {
      nParams = gen(env, LdFuncNumParams, funcPtr);
    }

    auto const vals64 = packBitVec(vals, i);
    if (auto failBlock = env.irb->guardFailBlock()) {
      gen(env,
          CheckRefs,
          failBlock,
          funcPtr,
          nParams,
          cns(env, i),
          cns(env, mask64),
          cns(env, vals64));
    } else if (dest == -1) {
      assert(offsetFromSP(env, 0) == 0);
      gen(env,
          GuardRefs,
          funcPtr,
          nParams,
          cns(env, i),
          cns(env, mask64),
          cns(env, vals64),
          fp(env),
          sp(env));
    } else {
      gen(env,
          CheckRefs,
          makeExit(env, dest),
          funcPtr,
          nParams,
          cns(env, i),
          cns(env, mask64),
          cns(env, vals64));
    }
  }
}

//////////////////////////////////////////////////////////////////////

}

void assertTypeLocal(HTS& env, uint32_t locId, Type type) {
  gen(env, AssertLoc, type, LocalId(locId), fp(env));
}

void assertTypeStack(HTS& env, uint32_t idx, Type type) {
  if (idx < env.irb->evalStack().size()) {
    // We're asserting a new type so we don't care about the previous type.
    auto const tmp = top(env, Type::StkElem, idx, DataTypeGeneric);
    assert(tmp);
    env.irb->evalStack().replace(idx, gen(env, AssertType, type, tmp));
  } else {
    gen(env,
        AssertStk,
        type,
        StackOffset { offsetFromSP(env, idx) },
        sp(env));
  }
}

void checkTypeStack(HTS& env, uint32_t idx, Type type, Offset dest) {
  assert(type <= Type::Gen);

  if (type.isBoxed()) {
    spillStack(env); // don't bother with the case that it's not spilled.
    auto const exit = makeExit(env, dest);
    auto const soff = StackOffset { offsetFromSP(env, idx) };
    profiledGuard(env, Type::BoxedInitCell, ProfGuard::CheckStk, idx, exit);
    env.irb->constrainStack(soff.offset, DataTypeSpecific);
    gen(env, HintStkInner, type & Type::BoxedInitCell, soff, sp(env));
    return;
  }

  auto exit = env.irb->guardFailBlock();
  if (exit == nullptr) exit = makeExit(env, dest);

  if (idx < env.irb->evalStack().size()) {
    FTRACE(1, "checkTypeStack({}): generating CheckType for {}\n",
           idx, type.toString());
    // CheckType only cares about its input type if the simplifier does
    // something with it and that's handled if and when it happens.
    auto const tmp = top(env, Type::StkElem, idx, DataTypeGeneric);
    assert(tmp);
    env.irb->evalStack().replace(idx, gen(env, CheckType, type, exit, tmp));
    return;
  }
  FTRACE(1, "checkTypeStack({}): no tmp: {}\n", idx, type.toString());
  // Just like CheckType, CheckStk only cares about its input type if the
  // simplifier does something with it.
  profiledGuard(env, type, ProfGuard::CheckStk, idx, exit);
}

//////////////////////////////////////////////////////////////////////

void assertTypeLocation(HTS& env, const RegionDesc::Location& loc, Type type) {
  assert(type <= Type::StkElem);
  using T = RegionDesc::Location::Tag;
  switch (loc.tag()) {
  case T::Stack: assertTypeStack(env, loc.stackOffset(), type); break;
  case T::Local: assertTypeLocal(env, loc.localId(), type);     break;
  }
}

void checkTypeLocation(HTS& env,
                       const RegionDesc::Location& loc,
                       Type type,
                       Offset dest) {
  assert(type <= Type::Gen);
  using T = RegionDesc::Location::Tag;
  switch (loc.tag()) {
  case T::Stack: checkTypeStack(env, loc.stackOffset(), type, dest); break;
  case T::Local: checkTypeLocal(env, loc.localId(), type, dest);     break;
  }
}

void guardTypeLocation(HTS& env,
                       const RegionDesc::Location& l,
                       Type type,
                       bool outerOnly) {
  assert(type <= Type::Gen);
  using T = RegionDesc::Location::Tag;
  switch (l.tag()) {
  case T::Stack: guardTypeStack(env, l.stackOffset(), type, outerOnly); break;
  case T::Local: guardTypeLocal(env, l.localId(),     type, outerOnly); break;
  }
}

void guardRefs(HTS& env,
               int64_t entryArDelta,
               const std::vector<bool>& mask,
               const std::vector<bool>& vals) {
  refCheckHelper(env, entryArDelta, mask, vals, -1);
}

void checkRefs(HTS& env,
               int64_t entryArDelta,
               const std::vector<bool>& mask,
               const std::vector<bool>& vals,
               Offset dest) {
  refCheckHelper(env, entryArDelta, mask, vals, dest);
}

//////////////////////////////////////////////////////////////////////

}}}
