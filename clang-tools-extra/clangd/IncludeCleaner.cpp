//===--- IncludeCleaner.cpp - Unused/Missing Headers Analysis ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "IncludeCleaner.h"
#include "support/Logger.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceLocation.h"

namespace clang {
namespace clangd {
namespace {

/// Crawler traverses the AST and feeds in the locations of (sometimes
/// implicitly) used symbols into \p Result.
class ReferencedLocationCrawler
    : public RecursiveASTVisitor<ReferencedLocationCrawler> {
public:
  ReferencedLocationCrawler(ReferencedLocations &Result) : Result(Result) {}

  bool VisitDeclRefExpr(DeclRefExpr *DRE) {
    add(DRE->getDecl());
    add(DRE->getFoundDecl());
    return true;
  }

  bool VisitMemberExpr(MemberExpr *ME) {
    add(ME->getMemberDecl());
    add(ME->getFoundDecl().getDecl());
    return true;
  }

  bool VisitTagType(TagType *TT) {
    add(TT->getDecl());
    return true;
  }

  bool VisitCXXConstructExpr(CXXConstructExpr *CCE) {
    add(CCE->getConstructor());
    return true;
  }

  bool VisitTemplateSpecializationType(TemplateSpecializationType *TST) {
    if (isNew(TST)) {
      add(TST->getTemplateName().getAsTemplateDecl()); // Primary template.
      add(TST->getAsCXXRecordDecl());                  // Specialization
    }
    return true;
  }

  bool VisitTypedefType(TypedefType *TT) {
    add(TT->getDecl());
    return true;
  }

  // Consider types of any subexpression used, even if the type is not named.
  // This is helpful in getFoo().bar(), where Foo must be complete.
  // FIXME(kirillbobyrev): Should we tweak this? It may not be desirable to
  // consider types "used" when they are not directly spelled in code.
  bool VisitExpr(Expr *E) {
    TraverseType(E->getType());
    return true;
  }

  bool TraverseType(QualType T) {
    if (isNew(T.getTypePtrOrNull())) { // don't care about quals
      Base::TraverseType(T);
    }
    return true;
  }

  bool VisitUsingDecl(UsingDecl *D) {
    for (const auto *Shadow : D->shadows()) {
      add(Shadow->getTargetDecl());
    }
    return true;
  }

private:
  using Base = RecursiveASTVisitor<ReferencedLocationCrawler>;

  void add(const Decl *D) {
    if (!D || !isNew(D->getCanonicalDecl())) {
      return;
    }
    for (const Decl *Redecl : D->redecls()) {
      Result.insert(Redecl->getLocation());
    }
  }

  bool isNew(const void *P) { return P && Visited.insert(P).second; }

  ReferencedLocations &Result;
  llvm::DenseSet<const void *> Visited;
};

// Given a set of referenced FileIDs, determines all the potentially-referenced
// files and macros by traversing expansion/spelling locations of macro IDs.
// This is used to map the referenced SourceLocations onto real files.
struct ReferencedFiles {
  ReferencedFiles(const SourceManager &SM) : SM(SM) {}
  llvm::DenseSet<FileID> Files;
  llvm::DenseSet<FileID> Macros;
  const SourceManager &SM;

  void add(SourceLocation Loc) { add(SM.getFileID(Loc), Loc); }

  void add(FileID FID, SourceLocation Loc) {
    if (FID.isInvalid())
      return;
    assert(SM.isInFileID(Loc, FID));
    if (Loc.isFileID()) {
      Files.insert(FID);
      return;
    }
    // Don't process the same macro FID twice.
    if (!Macros.insert(FID).second)
      return;
    const auto &Exp = SM.getSLocEntry(FID).getExpansion();
    add(Exp.getSpellingLoc());
    add(Exp.getExpansionLocStart());
    add(Exp.getExpansionLocEnd());
  }
};

} // namespace

ReferencedLocations findReferencedLocations(ParsedAST &AST) {
  ReferencedLocations Result;
  ReferencedLocationCrawler Crawler(Result);
  Crawler.TraverseAST(AST.getASTContext());
  // FIXME(kirillbobyrev): Handle macros.
  return Result;
}

llvm::DenseSet<FileID>
findReferencedFiles(const llvm::DenseSet<SourceLocation> &Locs,
                    const SourceManager &SM) {
  std::vector<SourceLocation> Sorted{Locs.begin(), Locs.end()};
  llvm::sort(Sorted); // Group by FileID.
  ReferencedFiles Result(SM);
  for (auto It = Sorted.begin(); It < Sorted.end();) {
    FileID FID = SM.getFileID(*It);
    Result.add(FID, *It);
    // Cheaply skip over all the other locations from the same FileID.
    // This avoids lots of redundant Loc->File lookups for the same file.
    do
      ++It;
    while (It != Sorted.end() && SM.isInFileID(*It, FID));
  }
  return std::move(Result.Files);
}

std::vector<Inclusion>
getUnused(IncludeStructure::HeaderID EntryPoint,
          const IncludeStructure &Structure,
          const llvm::DenseSet<IncludeStructure::HeaderID> &ReferencedFiles,
          const SourceManager &SM) {
  std::vector<Inclusion> Unused;
  for (auto &MFI : Structure.MainFileIncludes) {
    // FIXME: Skip includes that are not self-contained.
    auto Entry = SM.getFileManager().getFile(MFI.Resolved);
    if (!Entry) {
      elog("Missing FileEntry for {0}", MFI.Resolved);
      continue;
    }
    auto It = Structure.getID(*Entry);
    if (!It) {
      elog("Missing IncludeStructure::File for {0}", MFI.Resolved);
      continue;
    }
    bool Used = ReferencedFiles.find(*It) != ReferencedFiles.end();
    if (!Used) {
      Unused.push_back(MFI);
    }
    dlog("{0} is {1}", MFI.Written, Used ? "USED" : "UNUSED");
  }
  return Unused;
}

} // namespace clangd
} // namespace clang
