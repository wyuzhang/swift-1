//===--- NameBinding.cpp - Name Binding -----------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements name binding for Swift.
//
//===----------------------------------------------------------------------===//

// FIXME: Entrypoint declared in Parser.h
#include "swift/Parse/Parser.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Type.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/system_error.h"
using namespace swift;
using llvm::isa;
using llvm::dyn_cast;
using llvm::cast;
using llvm::SMLoc;

namespace {
  class ReferencedModule {
    // FIXME: A module can be more than one translation unit eventually.
    TranslationUnitDecl *TUD;
    
    llvm::DenseMap<Identifier, ValueDecl *> TopLevelValues;

  public:
    ReferencedModule(TranslationUnitDecl *tud) : TUD(tud) {}
    ~ReferencedModule() {
      // Nothing to destroy here, TU is ASTContext allocated.
    }

    /// lookupValue - Resolve a reference to a value name that found this module
    /// through the specified import declaration.
    ValueDecl *lookupValue(ImportDecl *ID, Identifier Name);
  };
} // end anonymous namespace.


/// lookupValue - Resolve a reference to a value name that found this module
/// through the specified import declaration.
ValueDecl *ReferencedModule::lookupValue(ImportDecl *ID, Identifier Name) {
  // TODO: ImportDecls cannot specified namespaces or individual entities
  // yet, so everything is just a lookup at the top-level.
    
  // If we haven't built a map of the top-level values, do so now.
  if (TopLevelValues.empty()) {
    for (llvm::ArrayRef<Decl*>::iterator I = TUD->Decls.begin(),
         E = TUD->Decls.end(); I != E; ++I) {
      if (ValueDecl *VD = dyn_cast<ValueDecl>(*I))
        if (!VD->Name.empty())
          TopLevelValues[VD->Name] = VD;
    }
  }
   
  llvm::DenseMap<Identifier, ValueDecl *>::iterator I =
    TopLevelValues.find(Name);
  return I != TopLevelValues.end() ? I->second : 0;
}

namespace {
  class NameBinder {
    std::vector<ReferencedModule *> LoadedModules;
    /// TopLevelValues - This is the list of top-level declarations we have.
    llvm::DenseMap<Identifier, ValueDecl *> TopLevelValues;
    llvm::SmallVector<std::pair<ImportDecl*, ReferencedModule*>, 4> Imports;
  public:
    ASTContext &Context;
    NameBinder(ASTContext &C) : Context(C) {}
    ~NameBinder() {
      for (unsigned i = 0, e = LoadedModules.size(); i != e; ++i)
        delete LoadedModules[i];      
    }
    
    void addNamedTopLevelDecl(ValueDecl *VD) {
      TopLevelValues[VD->Name] = VD;
    }
    
    void addImport(ImportDecl *ID);

    Expr *bindValueName(Identifier I, SMLoc Loc);
    
    void note(SMLoc Loc, const llvm::Twine &Message) {
      Context.SourceMgr.PrintMessage(Loc, Message, "note");
    }
    void warning(SMLoc Loc, const llvm::Twine &Message) {
      Context.SourceMgr.PrintMessage(Loc, Message, "warning");
    }
    void error(SMLoc Loc, const llvm::Twine &Message) {
      Context.setHadError();
      Context.SourceMgr.PrintMessage(Loc, Message, "error");
    }
  private:
    /// getReferencedModule - Load a module referenced by an import statement,
    /// emitting an error at the specified location and returning null on
    /// failure.
    ReferencedModule *getReferencedModule(llvm::SMLoc Loc, Identifier ModuleID);
  };
}

ReferencedModule *NameBinder::
getReferencedModule(SMLoc Loc, Identifier ModuleID) {
  std::string InputFilename = ModuleID.get()+std::string(".swift");
  
  // Open the input file.
  llvm::OwningPtr<llvm::MemoryBuffer> InputFile;
  if (llvm::error_code Err =
        llvm::MemoryBuffer::getFile(InputFilename, InputFile)) {
    error(Loc, "opening import file '" + InputFilename + "': " + Err.message());
    return 0;
  }

  unsigned BufferID =
    Context.SourceMgr.AddNewSourceBuffer(InputFile.take(), Loc);

  // Parse the translation unit, but don't do name binding or type checking.
  // This can produce new errors etc if the input is erroneous.
  TranslationUnitDecl *TUD = Parser(BufferID, Context).parseTranslationUnit();
  if (TUD == 0)
    return 0;
  
  ReferencedModule *RM = new ReferencedModule(TUD);
  LoadedModules.push_back(RM);
  return RM;
}


void NameBinder::addImport(ImportDecl *ID) {
  if (ReferencedModule *RM = getReferencedModule(ID->ImportLoc, ID->Name))
    Imports.push_back(std::make_pair(ID, RM));
}


/// bindValueName - This is invoked for each UnresolvedDeclRefExpr in the AST.
Expr *NameBinder::bindValueName(Identifier Name, SMLoc Loc) {
  // Resolve forward references defined within the module.
  llvm::DenseMap<Identifier, ValueDecl *>::iterator I =
    TopLevelValues.find(Name);
    // If we found a resolved decl, replace the unresolved ref with a resolved
    // DeclRefExpr.
  if (I != TopLevelValues.end())
    return new (Context) DeclRefExpr(I->second, Loc);

  // If we still haven't found it, scrape through all of the imports, taking the
  // first match of the name.
  for (unsigned i = 0, e = Imports.size(); i != e; ++i)
    if (ValueDecl *D = Imports[i].second->lookupValue(Imports[i].first, Name)) {
      // If we found a match, replace the unresolved ref with a resolved
      // DeclRefExpr.
      return new (Context) DeclRefExpr(D, Loc);
    }

  error(Loc, "use of unresolved identifier '" + Name.str() + "'");
  return 0;
}


static Expr *BindNames(Expr *E, Expr::WalkOrder Order, void *binder) {
  NameBinder &Binder = *static_cast<NameBinder*>(binder);
  
  // Ignore the preorder walk.
  if (Order == Expr::Walk_PreOrder)
    return E;
  
  // Ignore everything except UnresolvedDeclRefExpr.
  UnresolvedDeclRefExpr *UDRE = dyn_cast<UnresolvedDeclRefExpr>(E);
  if (UDRE == 0) return E;
  
  return Binder.bindValueName(UDRE->Name, UDRE->Loc);  
}

/// performNameBinding - Once parsing is complete, this walks the AST to
/// resolve names and do other top-level validation.
///
/// At this parsing has been performed, but we still have UnresolvedDeclRefExpr
/// nodes for unresolved value names, and we may have unresolved type names as
/// well.  This handles import directives and forward references.
void swift::performNameBinding(TranslationUnitDecl *TUD, ASTContext &Ctx) {
  NameBinder Binder(Ctx);
  
  // Do a prepass over the declarations to find the list of top-level value
  // declarations.
  for (llvm::ArrayRef<Decl*>::iterator I = TUD->Decls.begin(),
       E = TUD->Decls.end(); I != E; ++I) {
    if (ValueDecl *VD = dyn_cast<ValueDecl>(*I))
      if (!VD->Name.empty())
        Binder.addNamedTopLevelDecl(VD);
    
    if (ImportDecl *ID = dyn_cast<ImportDecl>(*I))
      Binder.addImport(ID);
  }
  
  // Now that we know the top-level value names, go through and resolve any
  // UnresolvedDeclRefExprs that exist.
  for (llvm::ArrayRef<Decl*>::iterator I = TUD->Decls.begin(),
       E = TUD->Decls.end(); I != E; ++I) {
    if (ValueDecl *VD = dyn_cast<ValueDecl>(*I))
      if (VD->Init)
        VD->Init = VD->Init->WalkExpr(BindNames, &Binder);
  }
}
