//===--- DerivedConformancePointwiseMultiplicative.cpp --------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements explicit derivation of the PointwiseMultiplicative
// protocol for struct types.
//
//===----------------------------------------------------------------------===//

#include "CodeSynthesis.h"
#include "TypeChecker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Types.h"
#include "DerivedConformances.h"

using namespace swift;

bool
DerivedConformance::canDerivePointwiseMultiplicative(NominalTypeDecl *nominal,
                                                     DeclContext *DC) {
  // Nominal type must be a struct. (No stored properties is okay.)
  auto *structDecl = dyn_cast<StructDecl>(nominal);
  if (!structDecl)
    return false;
  // Must not have any `let` stored properties with an initial value.
  // - This restriction may be lifted later with support for "true" memberwise
  //   initializers that initialize all stored properties, including initial
  //   value information.
  if (hasLetStoredPropertyWithInitialValue(nominal))
    return false;
  // All stored properties must conform to `AdditiveArithmetic`.
  auto &C = nominal->getASTContext();
  auto *proto = C.getProtocol(KnownProtocolKind::PointwiseMultiplicative);
  return llvm::all_of(structDecl->getStoredProperties(), [&](VarDecl *v) {
    if (v->getInterfaceType()->hasError())
      return false;
    auto varType = DC->mapTypeIntoContext(v->getValueInterfaceType());
    return (bool)TypeChecker::conformsToProtocol(varType, proto, DC);
  });
}

// Synthesize body for math operator.
static std::pair<BraceStmt *, bool>
deriveBodyMathOperator(AbstractFunctionDecl *funcDecl, void *) {
  auto *parentDC = funcDecl->getParent();
  auto *nominal = parentDC->getSelfNominalTypeDecl();
  auto &C = nominal->getASTContext();

  // Create memberwise initializer: `Nominal.init(...)`.
  auto *memberwiseInitDecl = nominal->getEffectiveMemberwiseInitializer();
  assert(memberwiseInitDecl && "Memberwise initializer must exist");
  auto *initDRE =
      new (C) DeclRefExpr(memberwiseInitDecl, DeclNameLoc(), /*Implicit*/ true);
  initDRE->setFunctionRefKind(FunctionRefKind::SingleApply);
  auto *nominalTypeExpr = TypeExpr::createImplicitForDecl(
      DeclNameLoc(), nominal, funcDecl,
      funcDecl->mapTypeIntoContext(nominal->getInterfaceType()));
  auto *initExpr = new (C) ConstructorRefCallExpr(initDRE, nominalTypeExpr);

  // Get operator protocol requirement.
  auto *proto = C.getProtocol(KnownProtocolKind::PointwiseMultiplicative);
  auto operatorId = C.getIdentifier(".*");
  auto *operatorReq = getProtocolRequirement(proto, operatorId);

  // Create reference to operator parameters: lhs and rhs.
  auto params = funcDecl->getParameters();

  // Create expression combining lhs and rhs members using member operator.
  auto createMemberOpExpr = [&](VarDecl *member) -> Expr * {
    auto module = nominal->getModuleContext();
    auto memberType =
        parentDC->mapTypeIntoContext(member->getValueInterfaceType());
    auto confRef = module->lookupConformance(memberType, proto);
    assert(confRef && "Member does not conform to math protocol");

    // Get member type's math operator, e.g. `Member.+`.
    // Use protocol requirement declaration for the operator by default: this
    // will be dynamically dispatched.
    ValueDecl *memberOpDecl = operatorReq;
    // If conformance reference is concrete, then use concrete witness
    // declaration for the operator.
    if (confRef.isConcrete())
      if (auto *concreteMemberMethodDecl =
              confRef.getConcrete()->getWitnessDecl(operatorReq))
        memberOpDecl = concreteMemberMethodDecl;
    assert(memberOpDecl && "Member operator declaration must exist");
    auto *memberTypeExpr = TypeExpr::createImplicit(memberType, C);
    auto memberOpExpr =
        new (C) MemberRefExpr(memberTypeExpr, SourceLoc(), memberOpDecl,
                              DeclNameLoc(), /*Implicit*/ true);

    // Create expression `lhs.member <op> rhs.member`.
    // NOTE(TF-1054): create new `DeclRefExpr`s per loop iteration to avoid
    // `ConstraintSystem::resolveOverload` error.
    auto *lhsDRE =
        new (C) DeclRefExpr(params->get(0), DeclNameLoc(), /*Implicit*/ true);
    auto *rhsDRE =
        new (C) DeclRefExpr(params->get(1), DeclNameLoc(), /*Implicit*/ true);
    Expr *lhsArg = new (C) MemberRefExpr(lhsDRE, SourceLoc(), member,
                                         DeclNameLoc(), /*Implicit*/ true);
    auto *rhsArg = new (C) MemberRefExpr(rhsDRE, SourceLoc(), member,
                                         DeclNameLoc(), /*Implicit*/ true);
    auto *memberOpArgs =
        TupleExpr::create(C, SourceLoc(), {lhsArg, rhsArg}, {}, {}, SourceLoc(),
                          /*HasTrailingClosure*/ false,
                          /*Implicit*/ true);
    auto *memberOpCallExpr =
        new (C) BinaryExpr(memberOpExpr, memberOpArgs, /*Implicit*/ true);
    return memberOpCallExpr;
  };

  // Create array of member operator call expressions.
  llvm::SmallVector<Expr *, 2> memberOpExprs;
  llvm::SmallVector<Identifier, 2> memberNames;
  for (auto member : nominal->getStoredProperties()) {
    memberOpExprs.push_back(createMemberOpExpr(member));
    memberNames.push_back(member->getName());
  }
  // Call memberwise initializer with member operator call expressions.
  auto *callExpr =
      CallExpr::createImplicit(C, initExpr, memberOpExprs, memberNames);
  ASTNode returnStmt = new (C) ReturnStmt(SourceLoc(), callExpr, true);
  return std::pair<BraceStmt *, bool>(
      BraceStmt::create(C, SourceLoc(), returnStmt, SourceLoc(), true), false);
}

// Synthesize function declaration for the given math operator.
static ValueDecl *
derivePointwiseMultiplicative_multiply(DerivedConformance &derived) {
  auto nominal = derived.Nominal;
  auto parentDC = derived.getConformanceContext();
  auto &C = derived.Context;
  auto selfInterfaceType = parentDC->getDeclaredInterfaceType();

  // Create parameter declaration with the given name and type.
  auto createParamDecl = [&](StringRef name, Type type) -> ParamDecl * {
    auto *param = new (C)
        ParamDecl(SourceLoc(), SourceLoc(), Identifier(), SourceLoc(),
                  C.getIdentifier(name), parentDC);
    param->setSpecifier(ParamDecl::Specifier::Default);
    param->setInterfaceType(type);
    return param;
  };

  ParameterList *params =
      ParameterList::create(C, {createParamDecl("lhs", selfInterfaceType),
                                createParamDecl("rhs", selfInterfaceType)});

  auto operatorId = C.getIdentifier(".*");
  DeclName operatorDeclName(C, operatorId, params);
  auto operatorDecl =
      FuncDecl::create(C, SourceLoc(), StaticSpellingKind::KeywordStatic,
                       SourceLoc(), operatorDeclName, SourceLoc(),
		       /*Async*/ false, SourceLoc(),
                       /*Throws*/ false, SourceLoc(),
                       /*GenericParams=*/nullptr, params,
                       TypeLoc::withoutLoc(selfInterfaceType), parentDC);
  operatorDecl->setImplicit();
  operatorDecl->setBodySynthesizer(&deriveBodyMathOperator);
  operatorDecl->setGenericSignature(parentDC->getGenericSignatureOfContext());
  operatorDecl->copyFormalAccessFrom(nominal, /*sourceIsParentContext*/ true);

  derived.addMembersToConformanceContext({operatorDecl});
  return operatorDecl;
}

// Synthesize body for a computed property getter.
static std::pair<BraceStmt *, bool>
deriveComputedPropertyGetter(AbstractFunctionDecl *funcDecl,
                             ProtocolDecl *proto, ValueDecl *reqDecl) {
  auto *parentDC = funcDecl->getParent();
  auto *nominal = parentDC->getSelfNominalTypeDecl();
  auto &C = nominal->getASTContext();

  auto *memberwiseInitDecl = nominal->getEffectiveMemberwiseInitializer();
  assert(memberwiseInitDecl && "Memberwise initializer must exist");
  auto *initDRE =
      new (C) DeclRefExpr(memberwiseInitDecl, DeclNameLoc(), /*Implicit*/ true);
  initDRE->setFunctionRefKind(FunctionRefKind::SingleApply);

  auto *nominalTypeExpr = TypeExpr::createImplicitForDecl(
      DeclNameLoc(), nominal, funcDecl,
      funcDecl->mapTypeIntoContext(nominal->getInterfaceType()));
  auto *initExpr = new (C) ConstructorRefCallExpr(initDRE, nominalTypeExpr);

  auto createMemberPropertyExpr = [&](VarDecl *member) -> Expr * {
    auto memberType =
        parentDC->mapTypeIntoContext(member->getValueInterfaceType());
    Expr *memberExpr = nullptr;
    // If the property is static, create a type expression: `Member`.
    if (reqDecl->isStatic()) {
      memberExpr = TypeExpr::createImplicit(memberType, C);
    }
    // If the property is not static, create a member ref expression:
    // `self.member`.
    else {
      auto *selfDecl = funcDecl->getImplicitSelfDecl();
      auto *selfDRE =
          new (C) DeclRefExpr(selfDecl, DeclNameLoc(), /*Implicit*/ true);
      memberExpr =
         new (C) MemberRefExpr(selfDRE, SourceLoc(), member, DeclNameLoc(),
                               /*Implicit*/ true);
    }
    auto module = nominal->getModuleContext();
    auto confRef = module->lookupConformance(memberType, proto);
    assert(confRef && "Member does not conform to `PointwiseMultiplicative`");
    // If conformance reference is not concrete, then concrete witness
    // declaration for property cannot be resolved. Return reference to protocol
    // requirement: this will be dynamically dispatched.
    if (!confRef.isConcrete()) {
      return new (C) MemberRefExpr(memberExpr, SourceLoc(), reqDecl,
                                   DeclNameLoc(), /*Implicit*/ true);
    }
    // Otherwise, return reference to concrete witness declaration.
    auto conf = confRef.getConcrete();
    auto witnessDecl = conf->getWitnessDecl(reqDecl);
    return new (C) MemberRefExpr(memberExpr, SourceLoc(), witnessDecl,
                                 DeclNameLoc(), /*Implicit*/ true);
  };

  // Create array of `member.<property>` expressions.
  llvm::SmallVector<Expr *, 2> memberPropExprs;
  llvm::SmallVector<Identifier, 2> memberNames;
  for (auto member : nominal->getStoredProperties()) {
    memberPropExprs.push_back(createMemberPropertyExpr(member));
    memberNames.push_back(member->getName());
  }
  // Call memberwise initializer with member property expressions.
  auto *callExpr =
      CallExpr::createImplicit(C, initExpr, memberPropExprs, memberNames);
  ASTNode returnStmt = new (C) ReturnStmt(SourceLoc(), callExpr, true);
  auto *braceStmt =
      BraceStmt::create(C, SourceLoc(), returnStmt, SourceLoc(), true);
  return std::pair<BraceStmt *, bool>(braceStmt, false);
}

// Synthesize body for the `PointwiseMultiplicative.one` computed property
// getter.
static std::pair<BraceStmt *, bool>
deriveBodyPointwiseMultiplicative_one(AbstractFunctionDecl *funcDecl, void *) {
  auto &C = funcDecl->getASTContext();
  auto *pointMulProto =
      C.getProtocol(KnownProtocolKind::PointwiseMultiplicative);
  auto *oneReq = getProtocolRequirement(pointMulProto, C.Id_one);
  return deriveComputedPropertyGetter(funcDecl, pointMulProto, oneReq);
}

// Synthesize body for the `PointwiseMultiplicative.reciprocal` computed
// property getter.
static std::pair<BraceStmt *, bool>
deriveBodyPointwiseMultiplicative_reciprocal(AbstractFunctionDecl *funcDecl,
                                             void *) {
  auto &C = funcDecl->getASTContext();
  auto *pointMulProto =
      C.getProtocol(KnownProtocolKind::PointwiseMultiplicative);
  auto *reciprocalReq = getProtocolRequirement(pointMulProto, C.Id_reciprocal);
  return deriveComputedPropertyGetter(funcDecl, pointMulProto, reciprocalReq);
}

// Synthesize a `PointwiseMultiplicative` property declaration.
static ValueDecl *
deriveProperty(DerivedConformance &derived, Identifier propertyName,
               bool isStatic,
               AbstractFunctionDecl::BodySynthesizer bodySynthesizer) {
  auto *nominal = derived.Nominal;
  auto *parentDC = derived.getConformanceContext();

  auto returnInterfaceTy = nominal->getDeclaredInterfaceType();
  auto returnTy = parentDC->mapTypeIntoContext(returnInterfaceTy);

  // Create property declaration.
  VarDecl *propDecl;
  PatternBindingDecl *pbDecl;
  std::tie(propDecl, pbDecl) = derived.declareDerivedProperty(
      propertyName, returnInterfaceTy, returnTy, /*isStatic*/ isStatic,
      /*isFinal*/ true);

  // Create property getter.
  auto *getterDecl =
      derived.addGetterToReadOnlyDerivedProperty(propDecl, returnTy);
  getterDecl->setBodySynthesizer(bodySynthesizer.Fn, bodySynthesizer.Context);
  derived.addMembersToConformanceContext({propDecl, pbDecl});

  return propDecl;
}

// Synthesize the static property declaration for
// `PointwiseMultiplicative.one`.
static ValueDecl *
derivePointwiseMultiplicative_one(DerivedConformance &derived) {
  auto &C = derived.Context;
  return deriveProperty(derived, C.Id_one, /*isStatic*/ true,
                        {deriveBodyPointwiseMultiplicative_one, nullptr});
}

// Synthesize the instance property declaration for
// `PointwiseMultiplicative.reciprocal`.
static ValueDecl *
derivePointwiseMultiplicative_reciprocal(DerivedConformance &derived) {
  auto &C = derived.Context;
  return deriveProperty(
      derived, C.Id_reciprocal, /*isStatic*/ false,
      {deriveBodyPointwiseMultiplicative_reciprocal, nullptr});
}

ValueDecl *
DerivedConformance::derivePointwiseMultiplicative(ValueDecl *requirement) {
  // Diagnose conformances in disallowed contexts.
  if (checkAndDiagnoseDisallowedContext(requirement))
    return nullptr;
  if (requirement->getBaseName() == Context.getIdentifier(".*"))
    return derivePointwiseMultiplicative_multiply(*this);
  if (requirement->getBaseName() == Context.Id_one)
    return derivePointwiseMultiplicative_one(*this);
  if (requirement->getBaseName() == Context.Id_reciprocal)
    return derivePointwiseMultiplicative_reciprocal(*this);
  Context.Diags.diagnose(requirement->getLoc(),
              diag::broken_pointwise_multiplicative_requirement);
  return nullptr;
}
