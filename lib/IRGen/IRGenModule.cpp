//===--- IRGenModule.cpp - Swift Global LLVM IR Generation ----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements IR generation for global declarations in Swift.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Availability.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Module.h"
#include "swift/AST/DiagnosticsIRGen.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/Basic/Dwarf.h"
#include "swift/Demangling/ManglingMacros.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/IRGen/IRGenPublic.h"
#include "swift/IRGen/Linking.h"
#include "swift/Runtime/RuntimeFnWrappersGen.h"
#include "swift/Runtime/Config.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/CodeGen/CodeGenABITypes.h"
#include "clang/CodeGen/ModuleBuilder.h"
#include "clang/CodeGen/SwiftCallingConv.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/HeaderSearchOptions.h"
#include "clang/Basic/CodeGenOptions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MD5.h"

#include "ConformanceDescription.h"
#include "GenDecl.h"
#include "GenEnum.h"
#include "GenIntegerLiteral.h"
#include "GenType.h"
#include "IRGenModule.h"
#include "IRGenDebugInfo.h"
#include "ProtocolInfo.h"
#include "StructLayout.h"

#include <initializer_list>

using namespace swift;
using namespace irgen;
using llvm::Attribute;

const unsigned DefaultAS = 0;

/// A helper for creating LLVM struct types.
static llvm::StructType *createStructType(IRGenModule &IGM,
                                          StringRef name,
                                  std::initializer_list<llvm::Type*> types,
                                          bool packed = false) {
  return llvm::StructType::create(IGM.getLLVMContext(),
                                  ArrayRef<llvm::Type*>(types.begin(),
                                                        types.size()),
                                  name, packed);
};

/// A helper for creating pointer-to-struct types.
static llvm::PointerType *createStructPointerType(IRGenModule &IGM,
                                                  StringRef name,
                                  std::initializer_list<llvm::Type*> types) {
  return createStructType(IGM, name, types)->getPointerTo(DefaultAS);
};

static clang::CodeGenerator *createClangCodeGenerator(ASTContext &Context,
                                                 llvm::LLVMContext &LLVMContext,
                                                      const IRGenOptions &Opts,
                                                      StringRef ModuleName,
                                                      StringRef PD) {
  auto Loader = Context.getClangModuleLoader();
  auto *Importer = static_cast<ClangImporter*>(&*Loader);
  assert(Importer && "No clang module loader!");
  auto &ClangContext = Importer->getClangASTContext();

  auto &CGO = Importer->getClangCodeGenOpts();
  CGO.OptimizationLevel = Opts.shouldOptimize() ? 3 : 0;
  CGO.setFramePointer(Opts.DisableFPElim
                          ? clang::CodeGenOptions::FramePointerKind::All
                          : clang::CodeGenOptions::FramePointerKind::None);
  CGO.DiscardValueNames = !Opts.shouldProvideValueNames();
  switch (Opts.DebugInfoLevel) {
  case IRGenDebugInfoLevel::None:
    CGO.setDebugInfo(clang::codegenoptions::DebugInfoKind::NoDebugInfo);
    break;
  case IRGenDebugInfoLevel::LineTables:
    CGO.setDebugInfo(clang::codegenoptions::DebugInfoKind::DebugLineTablesOnly);
    break;
  case IRGenDebugInfoLevel::ASTTypes:
  case IRGenDebugInfoLevel::DwarfTypes:
    CGO.DebugTypeExtRefs = true;
    CGO.setDebugInfo(clang::codegenoptions::DebugInfoKind::FullDebugInfo);
    break;
  }
  switch (Opts.DebugInfoFormat) {
  case IRGenDebugInfoFormat::None:
    break;
  case IRGenDebugInfoFormat::DWARF:
    CGO.DebugCompilationDir = Opts.DebugCompilationDir;
    CGO.DwarfVersion = Opts.DWARFVersion;
    CGO.DwarfDebugFlags = Opts.getDebugFlags(PD);
    break;
  case IRGenDebugInfoFormat::CodeView:
    CGO.EmitCodeView = true;
    CGO.DebugCompilationDir = Opts.DebugCompilationDir;
    // This actually contains the debug flags for codeview.
    CGO.DwarfDebugFlags = Opts.getDebugFlags(PD);
    break;
  }

  auto &HSI = Importer->getClangPreprocessor()
                  .getHeaderSearchInfo()
                  .getHeaderSearchOpts();
  auto &PPO = Importer->getClangPreprocessor().getPreprocessorOpts();
  auto *ClangCodeGen = clang::CreateLLVMCodeGen(ClangContext.getDiagnostics(),
                                                ModuleName, HSI, PPO, CGO,
                                                LLVMContext);
  ClangCodeGen->Initialize(ClangContext);
  return ClangCodeGen;
}

IRGenModule::IRGenModule(IRGenerator &irgen,
                         std::unique_ptr<llvm::TargetMachine> &&target,
                         SourceFile *SF, llvm::LLVMContext &LLVMContext,
                         StringRef ModuleName, StringRef OutputFilename,
                         StringRef MainInputFilenameForDebugInfo,
                         StringRef PrivateDiscriminator)
    : IRGen(irgen), Context(irgen.SIL.getASTContext()),
      ClangCodeGen(createClangCodeGenerator(Context, LLVMContext, irgen.Opts,
                                            ModuleName, PrivateDiscriminator)),
      Module(*ClangCodeGen->GetModule()), LLVMContext(Module.getContext()),
      DataLayout(irgen.getClangDataLayout()),
      Triple(irgen.getEffectiveClangTriple()), TargetMachine(std::move(target)),
      silConv(irgen.SIL), OutputFilename(OutputFilename),
      MainInputFilenameForDebugInfo(MainInputFilenameForDebugInfo),
      TargetInfo(SwiftTargetInfo::get(*this)), DebugInfo(nullptr),
      ModuleHash(nullptr), ObjCInterop(Context.LangOpts.EnableObjCInterop),
      UseDarwinPreStableABIBit(Context.LangOpts.UseDarwinPreStableABIBit),
      Types(*new TypeConverter(*this)) {
  irgen.addGenModule(SF, this);

  auto &opts = irgen.Opts;

  EnableValueNames = opts.shouldProvideValueNames();
  
  VoidTy = llvm::Type::getVoidTy(getLLVMContext());
  Int1Ty = llvm::Type::getInt1Ty(getLLVMContext());
  Int8Ty = llvm::Type::getInt8Ty(getLLVMContext());
  Int16Ty = llvm::Type::getInt16Ty(getLLVMContext());
  Int32Ty = llvm::Type::getInt32Ty(getLLVMContext());
  Int32PtrTy = Int32Ty->getPointerTo();
  Int64Ty = llvm::Type::getInt64Ty(getLLVMContext());
  Int8PtrTy = llvm::Type::getInt8PtrTy(getLLVMContext());
  Int8PtrPtrTy = Int8PtrTy->getPointerTo(0);
  SizeTy = DataLayout.getIntPtrType(getLLVMContext(), /*addrspace*/ 0);

  // For the relative address type, we want to use the int32 bit type
  // on most architectures, e.g. x86_64, because it produces valid
  // fixups/relocations. The exception is 16-bit architectures,
  // so we shorten the relative address type there.
  if (SizeTy->getBitWidth()<32) {
    RelativeAddressTy = SizeTy;
  } else {
    RelativeAddressTy = Int32Ty;
  }

  RelativeAddressPtrTy = RelativeAddressTy->getPointerTo();

  FloatTy = llvm::Type::getFloatTy(getLLVMContext());
  DoubleTy = llvm::Type::getDoubleTy(getLLVMContext());

  auto CI = static_cast<ClangImporter*>(&*Context.getClangModuleLoader());
  assert(CI && "no clang module loader");
  auto &clangASTContext = CI->getClangASTContext();

  ObjCBoolTy = Int1Ty;
  if (clangASTContext.getTargetInfo().useSignedCharForObjCBool())
    ObjCBoolTy = Int8Ty;

  RefCountedStructTy =
    llvm::StructType::create(getLLVMContext(), "swift.refcounted");
  RefCountedPtrTy = RefCountedStructTy->getPointerTo(/*addrspace*/ 0);
  RefCountedNull = llvm::ConstantPointerNull::get(RefCountedPtrTy);

  // For now, references storage types are just pointers.
#define CHECKED_REF_STORAGE(Name, name, ...) \
  Name##ReferencePtrTy = \
    createStructPointerType(*this, "swift." #name, { RefCountedPtrTy });
#include "swift/AST/ReferenceStorage.def"

  // A type metadata record is the structure pointed to by the canonical
  // address point of a type metadata.  This is at least one word, and
  // potentially more than that, past the start of the actual global
  // structure.
  TypeMetadataStructTy = createStructType(*this, "swift.type", {
    MetadataKindTy          // MetadataKind Kind;
  });
  TypeMetadataPtrTy = TypeMetadataStructTy->getPointerTo(DefaultAS);

  TypeMetadataResponseTy = createStructType(*this, "swift.metadata_response", {
    TypeMetadataPtrTy,
    SizeTy
  });

  OffsetPairTy = llvm::StructType::get(getLLVMContext(), { SizeTy, SizeTy });

  // The TypeLayout structure, including all possible trailing components.
  FullTypeLayoutTy = createStructType(*this, "swift.full_type_layout", {
    SizeTy, // size
    SizeTy, // flags
    SizeTy, // alignment
    SizeTy  // extra inhabitant flags (optional)
  });

  TypeLayoutTy = createStructType(*this, "swift.type_layout", {
    SizeTy, // size
    SizeTy, // stride
    Int32Ty, // flags
    Int32Ty // extra inhabitant count
  });

  // A protocol descriptor describes a protocol. It is not type metadata in
  // and of itself, but is referenced in the structure of existential type
  // metadata records.
  ProtocolDescriptorStructTy = createStructType(*this, "swift.protocol", {
    Int8PtrTy,              // objc isa
    Int8PtrTy,              // name
    Int8PtrTy,              // inherited protocols
    Int8PtrTy,              // required objc instance methods
    Int8PtrTy,              // required objc class methods
    Int8PtrTy,              // optional objc instance methods
    Int8PtrTy,              // optional objc class methods
    Int8PtrTy,              // objc properties
    Int32Ty,                // size
    Int32Ty,                // flags
    Int32Ty,                // total requirement count
    Int32Ty,                // requirements array
    RelativeAddressTy,      // superclass
    RelativeAddressTy       // associated type names
  });
  
  ProtocolDescriptorPtrTy = ProtocolDescriptorStructTy->getPointerTo();

  ProtocolRequirementStructTy =
      createStructType(*this, "swift.protocol_requirement", {
    Int32Ty,                // flags
    RelativeAddressTy,      // default implementation
  });
  
  // A tuple type metadata record has a couple extra fields.
  auto tupleElementTy = createStructType(*this, "swift.tuple_element_type", {
    TypeMetadataPtrTy,      // Metadata *Type;
    Int32Ty                 // int32_t Offset;
  });
  TupleTypeMetadataPtrTy = createStructPointerType(*this, "swift.tuple_type", {
    TypeMetadataStructTy,   // (base)
    SizeTy,                 // size_t NumElements;
    Int8PtrTy,              // const char *Labels;
    llvm::ArrayType::get(tupleElementTy, 0) // Element Elements[];
  });

  // A full type metadata record is basically just an adjustment to the
  // address point of a type metadata.  Resilience may cause
  // additional data to be laid out prior to this address point.
  FullTypeMetadataStructTy = createStructType(*this, "swift.full_type", {
    WitnessTablePtrTy,
    TypeMetadataStructTy
  });
  FullTypeMetadataPtrTy = FullTypeMetadataStructTy->getPointerTo(DefaultAS);

  DeallocatingDtorTy = llvm::FunctionType::get(VoidTy, RefCountedPtrTy, false);
  llvm::Type *dtorPtrTy = DeallocatingDtorTy->getPointerTo();

  // A full heap metadata is basically just an additional small prefix
  // on a full metadata, used for metadata corresponding to heap
  // allocations.
  FullHeapMetadataStructTy =
                  createStructType(*this, "swift.full_heapmetadata", {
    dtorPtrTy,
    WitnessTablePtrTy,
    TypeMetadataStructTy
  });
  FullHeapMetadataPtrTy = FullHeapMetadataStructTy->getPointerTo(DefaultAS);

  // A full box metadata is non-type heap metadata for a heap allocation of a
  // single value. The box tracks the offset to the value inside the box.
  FullBoxMetadataStructTy =
                  createStructType(*this, "swift.full_boxmetadata", {
    dtorPtrTy,
    WitnessTablePtrTy,
    TypeMetadataStructTy,
    Int32Ty,
    CaptureDescriptorPtrTy,
  });
  FullBoxMetadataPtrTy = FullBoxMetadataStructTy->getPointerTo(DefaultAS);

  // This must match struct HeapObject in the runtime.
  llvm::Type *refCountedElts[] = {TypeMetadataPtrTy, IntPtrTy};
  RefCountedStructTy->setBody(refCountedElts);
  RefCountedStructSize =
    Size(DataLayout.getStructLayout(RefCountedStructTy)->getSizeInBytes());

  PtrSize = Size(DataLayout.getPointerSize(DefaultAS));

  FunctionPairTy = createStructType(*this, "swift.function", {
    FunctionPtrTy,
    RefCountedPtrTy,
  });

  OpaqueTy = llvm::StructType::create(LLVMContext, "swift.opaque");
  OpaquePtrTy = OpaqueTy->getPointerTo(DefaultAS);
  NoEscapeFunctionPairTy = createStructType(*this, "swift.noescape.function", {
    FunctionPtrTy,
    OpaquePtrTy,
  });

  ProtocolRecordTy =
    createStructType(*this, "swift.protocolref", {
      RelativeAddressTy
    });
  ProtocolRecordPtrTy = ProtocolRecordTy->getPointerTo();

  ProtocolConformanceDescriptorTy
    = createStructType(*this, "swift.protocol_conformance_descriptor", {
      RelativeAddressTy,
      RelativeAddressTy,
      RelativeAddressTy,
      Int32Ty
    });
  ProtocolConformanceDescriptorPtrTy
    = ProtocolConformanceDescriptorTy->getPointerTo(DefaultAS);

  TypeContextDescriptorTy
    = llvm::StructType::create(LLVMContext, "swift.type_descriptor");
  TypeContextDescriptorPtrTy
    = TypeContextDescriptorTy->getPointerTo(DefaultAS);

  ClassContextDescriptorTy =
        llvm::StructType::get(LLVMContext, {
    Int32Ty, // context flags
    Int32Ty, // parent
    Int32Ty, // name
    Int32Ty, // kind
    Int32Ty, // accessor function
    Int32Ty, // num fields
    Int32Ty, // field offset vector
    Int32Ty, // is_reflectable flag
    Int32Ty, // (Generics Descriptor) argument offset
    Int32Ty, // (Generics Descriptor) num params
    Int32Ty, // (Generics Descriptor) num requirements
    Int32Ty, // (Generics Descriptor) num key arguments
    Int32Ty, // (Generics Descriptor) num extra arguments
    Int32Ty, // (VTable Descriptor) offset
    Int32Ty, // (VTable Descriptor) size
    Int32Ty, // (Methods Descriptor) accessor
    Int32Ty, // (Methods Descriptor) flags
  }, /*packed=*/true);

  MethodDescriptorStructTy
    = createStructType(*this, "swift.method_descriptor", {
      Int32Ty,
      RelativeAddressTy,
    });

  MethodOverrideDescriptorStructTy
    = createStructType(*this, "swift.method_override_descriptor", {
      RelativeAddressTy,
      RelativeAddressTy,
      RelativeAddressTy
    });

  TypeMetadataRecordTy
    = createStructType(*this, "swift.type_metadata_record", {
      RelativeAddressTy
    });
  TypeMetadataRecordPtrTy
    = TypeMetadataRecordTy->getPointerTo(DefaultAS);

  FieldDescriptorTy
    = llvm::StructType::create(LLVMContext, "swift.field_descriptor");
  FieldDescriptorPtrTy = FieldDescriptorTy->getPointerTo(DefaultAS);
  FieldDescriptorPtrPtrTy = FieldDescriptorPtrTy->getPointerTo(DefaultAS);

  FixedBufferTy = nullptr;
  for (unsigned i = 0; i != MaxNumValueWitnesses; ++i)
    ValueWitnessTys[i] = nullptr;

  ObjCPtrTy = llvm::StructType::create(getLLVMContext(), "objc_object")
                ->getPointerTo(DefaultAS);
  BridgeObjectPtrTy = llvm::StructType::create(getLLVMContext(), "swift.bridge")
                ->getPointerTo(DefaultAS);

  ObjCClassStructTy = llvm::StructType::create(LLVMContext, "objc_class");
  ObjCClassPtrTy = ObjCClassStructTy->getPointerTo(DefaultAS);
  llvm::Type *objcClassElts[] = {
    ObjCClassPtrTy,
    ObjCClassPtrTy,
    OpaquePtrTy,
    OpaquePtrTy,
    IntPtrTy
  };
  ObjCClassStructTy->setBody(objcClassElts);

  ObjCSuperStructTy = llvm::StructType::create(LLVMContext, "objc_super");
  ObjCSuperPtrTy = ObjCSuperStructTy->getPointerTo(DefaultAS);
  llvm::Type *objcSuperElts[] = {
    ObjCPtrTy,
    ObjCClassPtrTy
  };
  ObjCSuperStructTy->setBody(objcSuperElts);
  
  ObjCBlockStructTy = llvm::StructType::create(LLVMContext, "objc_block");
  ObjCBlockPtrTy = ObjCBlockStructTy->getPointerTo(DefaultAS);
  llvm::Type *objcBlockElts[] = {
    ObjCClassPtrTy, // isa
    Int32Ty,        // flags
    Int32Ty,        // reserved
    FunctionPtrTy,  // invoke function pointer
    Int8PtrTy,      // TODO: block descriptor pointer.
                    // We will probably need a struct type for that at some
                    // point too.
  };
  ObjCBlockStructTy->setBody(objcBlockElts);

  // Class _Nullable callback(Class _Nonnull cls, void * _Nullable arg);
  llvm::Type *params[] = { ObjCClassPtrTy, Int8PtrTy };
  ObjCUpdateCallbackTy = llvm::FunctionType::get(ObjCClassPtrTy, params, false);

  // The full class stub structure, including a word before the address point.
  ObjCFullResilientClassStubTy = createStructType(*this, "objc_full_class_stub", {
    SizeTy, // zero padding to appease the linker
    SizeTy, // isa pointer -- always 1
    ObjCUpdateCallbackTy->getPointerTo() // the update callback
  });

  // What we actually export.
  ObjCResilientClassStubTy = createStructType(*this, "objc_class_stub", {
    SizeTy, // isa pointer -- always 1
    ObjCUpdateCallbackTy->getPointerTo() // the update callback
  });

  auto ErrorStructTy = llvm::StructType::create(LLVMContext, "swift.error");
  // ErrorStruct is currently opaque to the compiler.
  ErrorPtrTy = ErrorStructTy->getPointerTo(DefaultAS);
  
  llvm::Type *openedErrorTriple[] = {
    OpaquePtrTy,
    TypeMetadataPtrTy,
    WitnessTablePtrTy,
  };
  OpenedErrorTripleTy = llvm::StructType::get(getLLVMContext(),
                                              openedErrorTriple,
                                              /*packed*/ false);
  OpenedErrorTriplePtrTy = OpenedErrorTripleTy->getPointerTo(DefaultAS);

  WitnessTablePtrPtrTy = WitnessTablePtrTy->getPointerTo(DefaultAS);
  
  // todo
  OpaqueTypeDescriptorTy = TypeContextDescriptorTy;
  OpaqueTypeDescriptorPtrTy = OpaqueTypeDescriptorTy->getPointerTo();

  InvariantMetadataID = LLVMContext.getMDKindID("invariant.load");
  InvariantNode = llvm::MDNode::get(LLVMContext, {});
  DereferenceableID = LLVMContext.getMDKindID("dereferenceable");
  
  C_CC = llvm::CallingConv::C;
  // TODO: use "tinycc" on platforms that support it
  DefaultCC = SWIFT_DEFAULT_LLVM_CC;
  SwiftCC = llvm::CallingConv::Swift;

  if (opts.DebugInfoLevel > IRGenDebugInfoLevel::None)
    DebugInfo = IRGenDebugInfo::createIRGenDebugInfo(IRGen.Opts, *CI, *this,
                                                     Module,
                                                 MainInputFilenameForDebugInfo,
                                                     PrivateDiscriminator);

  initClangTypeConverter();

  if (ClangASTContext) {
    auto atomicBoolTy = ClangASTContext->getAtomicType(ClangASTContext->BoolTy);
    AtomicBoolSize = Size(ClangASTContext->getTypeSize(atomicBoolTy));
    AtomicBoolAlign = Alignment(ClangASTContext->getTypeSize(atomicBoolTy));
  }

  IsSwiftErrorInRegister =
    clang::CodeGen::swiftcall::isSwiftErrorLoweredInRegister(
      ClangCodeGen->CGM());

  DynamicReplacementsTy =
      llvm::StructType::get(getLLVMContext(), {Int8PtrPtrTy, Int8PtrTy});
  DynamicReplacementsPtrTy = DynamicReplacementsTy->getPointerTo(DefaultAS);

  DynamicReplacementLinkEntryTy =
      llvm::StructType::create(getLLVMContext(), "swift.dyn_repl_link_entry");
  DynamicReplacementLinkEntryPtrTy =
      DynamicReplacementLinkEntryTy->getPointerTo(DefaultAS);
  llvm::Type *linkEntryFields[] = {
    Int8PtrTy, // function pointer.
    DynamicReplacementLinkEntryPtrTy // next.
  };
  DynamicReplacementLinkEntryTy->setBody(linkEntryFields);

  DynamicReplacementKeyTy = createStructType(*this, "swift.dyn_repl_key",
                                             {RelativeAddressTy, Int32Ty});

  DifferentiabilityWitnessTy = createStructType(
      *this, "swift.differentiability_witness", {Int8PtrTy, Int8PtrTy});
}

IRGenModule::~IRGenModule() {
  destroyClangTypeConverter();
  destroyMetadataLayoutMap();
  delete &Types;
}

static bool isReturnAttribute(llvm::Attribute::AttrKind Attr);

// Explicitly listing these constants is an unfortunate compromise for
// making the database file much more compact.
//
// They have to be non-local because otherwise we'll get warnings when
// a particular x-macro expansion doesn't use one.
namespace RuntimeConstants {
  const auto ReadNone = llvm::Attribute::ReadNone;
  const auto ReadOnly = llvm::Attribute::ReadOnly;
  const auto ArgMemOnly = llvm::Attribute::ArgMemOnly;
  const auto NoReturn = llvm::Attribute::NoReturn;
  const auto NoUnwind = llvm::Attribute::NoUnwind;
  const auto ZExt = llvm::Attribute::ZExt;
  const auto FirstParamReturned = llvm::Attribute::Returned;

  RuntimeAvailability AlwaysAvailable(ASTContext &Context) {
    return RuntimeAvailability::AlwaysAvailable;
  }

  bool
  isDeploymentAvailabilityContainedIn(ASTContext &Context,
                                      AvailabilityContext featureAvailability) {
    auto deploymentAvailability =
      AvailabilityContext::forDeploymentTarget(Context);
    return deploymentAvailability.isContainedIn(featureAvailability);
  }

  RuntimeAvailability OpaqueTypeAvailability(ASTContext &Context) {
    auto featureAvailability = Context.getOpaqueTypeAvailability();
    if (!isDeploymentAvailabilityContainedIn(Context, featureAvailability)) {
      return RuntimeAvailability::ConditionallyAvailable;
    }
    return RuntimeAvailability::AlwaysAvailable;
  }

  RuntimeAvailability
  GetTypesInAbstractMetadataStateAvailability(ASTContext &context) {
    auto featureAvailability =
        context.getTypesInAbstractMetadataStateAvailability();
    if (!isDeploymentAvailabilityContainedIn(context, featureAvailability)) {
      return RuntimeAvailability::ConditionallyAvailable;
    }
    return RuntimeAvailability::AlwaysAvailable;
  }

  RuntimeAvailability DynamicReplacementAvailability(ASTContext &Context) {
    auto featureAvailability = Context.getSwift51Availability();
    if (!isDeploymentAvailabilityContainedIn(Context, featureAvailability)) {
      return RuntimeAvailability::AvailableByCompatibilityLibrary;
    }
    return RuntimeAvailability::AlwaysAvailable;
  }
} // namespace RuntimeConstants

// We don't use enough attributes to justify generalizing the
// RuntimeFunctions.def FUNCTION macro. Instead, special case the one attribute
// associated with the return type not the function type.
static bool isReturnAttribute(llvm::Attribute::AttrKind Attr) {
  return Attr == llvm::Attribute::ZExt;
}
// Similar to the 'return' attribute we assume that the 'returned' attributed is
// associated with the first function parameter.
static bool isReturnedAttribute(llvm::Attribute::AttrKind Attr) {
  return Attr == llvm::Attribute::Returned;
}

namespace {
bool isStandardLibrary(const llvm::Module &M) {
  if (auto *Flags = M.getNamedMetadata("swift.module.flags")) {
    for (const auto *F : Flags->operands()) {
      const auto *Key = dyn_cast_or_null<llvm::MDString>(F->getOperand(0));
      if (!Key)
        continue;

      const auto *Value =
          dyn_cast_or_null<llvm::ConstantAsMetadata>(F->getOperand(1));
      if (!Value)
        continue;

      if (Key->getString() == "standard-library")
        return cast<llvm::ConstantInt>(Value->getValue())->isOne();
    }
  }
  return false;
}
}

bool IRGenModule::isStandardLibrary() const {
  return ::isStandardLibrary(Module);
}

llvm::Constant *swift::getRuntimeFn(llvm::Module &Module,
                      llvm::Constant *&cache,
                      const char *name,
                      llvm::CallingConv::ID cc,
                      RuntimeAvailability availability,
                      llvm::ArrayRef<llvm::Type*> retTypes,
                      llvm::ArrayRef<llvm::Type*> argTypes,
                      ArrayRef<Attribute::AttrKind> attrs) {

  if (cache)
    return cache;

  bool isWeakLinked = false;
  std::string functionName(name);

  switch (availability) {
  case RuntimeAvailability::AlwaysAvailable:
    // Nothing to do.
    break;
  case RuntimeAvailability::ConditionallyAvailable: {
    isWeakLinked = true;
    break;
  }
  case RuntimeAvailability::AvailableByCompatibilityLibrary: {
    functionName.append("50");
    break;
  }
  }

  llvm::Type *retTy;
  if (retTypes.size() == 1)
    retTy = *retTypes.begin();
  else
    retTy = llvm::StructType::get(Module.getContext(),
                                  {retTypes.begin(), retTypes.end()},
                                  /*packed*/ false);
  auto fnTy = llvm::FunctionType::get(retTy,
                                      {argTypes.begin(), argTypes.end()},
                                      /*isVararg*/ false);

  auto addr = Module.getOrInsertFunction(functionName.c_str(), fnTy).getCallee();
  auto fnptr = addr;
  // Strip off any bitcast we might have due to this function being declared of
  // a different type previously.
  if (auto bitcast = dyn_cast<llvm::BitCastInst>(fnptr))
    fnptr = cast<llvm::Constant>(bitcast->getOperand(0));
  cache = cast<llvm::Constant>(addr);

  // Add any function attributes and set the calling convention.
  if (auto fn = dyn_cast<llvm::Function>(fnptr)) {
    fn->setCallingConv(cc);

    bool IsExternal =
        fn->getLinkage() == llvm::GlobalValue::AvailableExternallyLinkage ||
        (fn->getLinkage() == llvm::GlobalValue::ExternalLinkage &&
         fn->isDeclaration());

    if (!isStandardLibrary(Module) && IsExternal &&
        ::useDllStorage(llvm::Triple(Module.getTargetTriple())))
      fn->setDLLStorageClass(llvm::GlobalValue::DLLImportStorageClass);
    
    if (IsExternal && isWeakLinked
        && !::useDllStorage(llvm::Triple(Module.getTargetTriple())))
      fn->setLinkage(llvm::GlobalValue::ExternalWeakLinkage);

    llvm::AttrBuilder buildFnAttr;
    llvm::AttrBuilder buildRetAttr;
    llvm::AttrBuilder buildFirstParamAttr;

    for (auto Attr : attrs) {
      if (isReturnAttribute(Attr))
        buildRetAttr.addAttribute(Attr);
      else if (isReturnedAttribute(Attr))
        buildFirstParamAttr.addAttribute(Attr);
      else
        buildFnAttr.addAttribute(Attr);
    }
    fn->addAttributes(llvm::AttributeList::FunctionIndex, buildFnAttr);
    fn->addAttributes(llvm::AttributeList::ReturnIndex, buildRetAttr);
    fn->addParamAttrs(0, buildFirstParamAttr);
  }

  return cache;
}

#define QUOTE(...) __VA_ARGS__
#define STR(X)     #X

#define FUNCTION(ID, NAME, CC, AVAILABILITY, RETURNS, ARGS, ATTRS) \
  FUNCTION_IMPL(ID, NAME, CC, AVAILABILITY, QUOTE(RETURNS), QUOTE(ARGS), QUOTE(ATTRS))

#define RETURNS(...) { __VA_ARGS__ }
#define ARGS(...) { __VA_ARGS__ }
#define NO_ARGS {}
#define ATTRS(...) { __VA_ARGS__ }
#define NO_ATTRS {}

#define FUNCTION_IMPL(ID, NAME, CC, AVAILABILITY, RETURNS, ARGS, ATTRS)        \
  llvm::Constant *IRGenModule::get##ID##Fn() {                                 \
    using namespace RuntimeConstants;                                          \
    return getRuntimeFn(Module, ID##Fn, #NAME, CC,                             \
                        AVAILABILITY(this->Context),                          \
                        RETURNS, ARGS, ATTRS);                                 \
  }

#include "swift/Runtime/RuntimeFunctions.def"

std::pair<llvm::GlobalVariable *, llvm::Constant *>
IRGenModule::createStringConstant(StringRef Str,
  bool willBeRelativelyAddressed, StringRef sectionName) {
  // If not, create it.  This implicitly adds a trailing null.
  auto init = llvm::ConstantDataArray::getString(LLVMContext, Str);
  auto global = new llvm::GlobalVariable(Module, init->getType(), true,
                                         llvm::GlobalValue::PrivateLinkage,
                                         init);
  // FIXME: ld64 crashes resolving relative references to coalesceable symbols.
  // rdar://problem/22674524
  // If we intend to relatively address this string, don't mark it with
  // unnamed_addr to prevent it from going into the cstrings section and getting
  // coalesced.
  if (!willBeRelativelyAddressed)
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

  if (!sectionName.empty())
    global->setSection(sectionName);

  // Drill down to make an i8*.
  auto zero = llvm::ConstantInt::get(SizeTy, 0);
  llvm::Constant *indices[] = { zero, zero };
  auto address = llvm::ConstantExpr::getInBoundsGetElementPtr(
    global->getValueType(), global, indices);

  return { global, address };
}

#define KNOWN_METADATA_ACCESSOR(NAME, SYM)                                     \
  llvm::Constant *IRGenModule::get##NAME() {                                   \
    if (NAME)                                                                  \
      return NAME;                                                             \
    NAME = Module.getOrInsertGlobal(SYM, FullTypeMetadataStructTy);            \
    if (useDllStorage() && !isStandardLibrary())                               \
      ApplyIRLinkage(IRLinkage::ExternalImport)                                \
          .to(cast<llvm::GlobalVariable>(NAME));                               \
    return NAME;                                                               \
  }

KNOWN_METADATA_ACCESSOR(EmptyTupleMetadata,
                        MANGLE_AS_STRING(METADATA_SYM(EMPTY_TUPLE_MANGLING)))
KNOWN_METADATA_ACCESSOR(AnyExistentialMetadata,
                        MANGLE_AS_STRING(METADATA_SYM(ANY_MANGLING)))
KNOWN_METADATA_ACCESSOR(AnyObjectExistentialMetadata,
                        MANGLE_AS_STRING(METADATA_SYM(ANYOBJECT_MANGLING)))

#undef KNOWN_METADATA_ACCESSOR

llvm::Constant *IRGenModule::getObjCEmptyCachePtr() {
  if (ObjCEmptyCachePtr)
    return ObjCEmptyCachePtr;

  if (ObjCInterop) {
    // struct objc_cache _objc_empty_cache;
    ObjCEmptyCachePtr = Module.getOrInsertGlobal("_objc_empty_cache",
                                                 OpaquePtrTy->getElementType());
    ApplyIRLinkage(IRLinkage::ExternalImport)
        .to(cast<llvm::GlobalVariable>(ObjCEmptyCachePtr));
  } else {
    // FIXME: Remove even the null value per rdar://problem/18801263
    ObjCEmptyCachePtr = llvm::ConstantPointerNull::get(OpaquePtrTy);
  }
  return ObjCEmptyCachePtr;
}

llvm::Constant *IRGenModule::getObjCEmptyVTablePtr() {
  // IMP _objc_empty_vtable;

  // On recent Darwin platforms, this symbol is defined at
  // runtime as an absolute symbol with the value of null. Older ObjCs
  // didn't guarantee _objc_empty_vtable to be nil, but Swift doesn't
  // deploy far enough back for that to be a concern.

  // FIXME: When !ObjCInterop, we should remove even the null value per
  // rdar://problem/18801263

  if (!ObjCEmptyVTablePtr)
    ObjCEmptyVTablePtr = llvm::ConstantPointerNull::get(OpaquePtrTy);

  return ObjCEmptyVTablePtr;
}

Address IRGenModule::getAddrOfObjCISAMask() {
  // This symbol is only exported by the runtime if the platform uses
  // isa masking.
  assert(TargetInfo.hasISAMasking());
  if (!ObjCISAMaskPtr) {
    ObjCISAMaskPtr = Module.getOrInsertGlobal("swift_isaMask", IntPtrTy);
    ApplyIRLinkage(IRLinkage::ExternalImport)
        .to(cast<llvm::GlobalVariable>(ObjCISAMaskPtr));
  }
  return Address(ObjCISAMaskPtr, getPointerAlignment());
}

ModuleDecl *IRGenModule::getSwiftModule() const {
  return IRGen.SIL.getSwiftModule();
}

AvailabilityContext IRGenModule::getAvailabilityContext() const {
  return AvailabilityContext::forDeploymentTarget(Context);
}

Lowering::TypeConverter &IRGenModule::getSILTypes() const {
  return IRGen.SIL.Types;
}

clang::CodeGen::CodeGenModule &IRGenModule::getClangCGM() const {
  return ClangCodeGen->CGM();
}

llvm::Module *IRGenModule::getModule() const {
  return ClangCodeGen->GetModule();
}

llvm::Module *IRGenModule::releaseModule() {
  return ClangCodeGen->ReleaseModule();
}

bool IRGenerator::canEmitWitnessTableLazily(SILWitnessTable *wt) {
  if (Opts.UseJIT)
    return false;

  // Regardless of the access level, if the witness table is shared it means
  // we can safely not emit it. Every other module which needs it will generate
  // its own shared copy of it.
  if (wt->getLinkage() == SILLinkage::Shared)
    return true;

  NominalTypeDecl *ConformingTy =
    wt->getConformingType()->getNominalOrBoundGenericNominal();

  switch (ConformingTy->getEffectiveAccess()) {
    case AccessLevel::Private:
    case AccessLevel::FilePrivate:
      return true;

    case AccessLevel::Internal:
      return PrimaryIGM->getSILModule().isWholeModule();

    default:
      return false;
  }
  llvm_unreachable("switch does not handle all cases");
}

void IRGenerator::addLazyWitnessTable(const ProtocolConformance *Conf) {
  if (auto *wt = SIL.lookUpWitnessTable(Conf, /*deserializeLazily=*/false)) {
    // Add it to the queue if it hasn't already been put there.
    if (canEmitWitnessTableLazily(wt) &&
        LazilyEmittedWitnessTables.insert(wt).second) {
      assert(!FinishedEmittingLazyDefinitions);
      LazyWitnessTables.push_back(wt);
    }
  }
}

void IRGenerator::addClassForEagerInitialization(ClassDecl *ClassDecl) {
  if (!ClassDecl->getAttrs().hasAttribute<StaticInitializeObjCMetadataAttr>())
    return;

  assert(!ClassDecl->isGenericContext());
  assert(!ClassDecl->hasClangNode());

  ClassesForEagerInitialization.push_back(ClassDecl);
}

llvm::AttributeList IRGenModule::getAllocAttrs() {
  if (AllocAttrs.isEmpty()) {
    AllocAttrs =
        llvm::AttributeList::get(LLVMContext, llvm::AttributeList::ReturnIndex,
                                 llvm::Attribute::NoAlias);
    AllocAttrs =
        AllocAttrs.addAttribute(LLVMContext, llvm::AttributeList::FunctionIndex,
                                llvm::Attribute::NoUnwind);
  }
  return AllocAttrs;
}

/// Disable thumb-mode until debugger support is there.
bool swift::irgen::shouldRemoveTargetFeature(StringRef feature) {
  return feature == "+thumb-mode";
}

void IRGenModule::setHasFramePointer(llvm::AttrBuilder &Attrs,
                                     bool HasFramePointer) {
  Attrs.addAttribute("frame-pointer", HasFramePointer ? "all" : "none");
}

void IRGenModule::setHasFramePointer(llvm::Function *F,
                                     bool HasFramePointer) {
  llvm::AttrBuilder b;
  setHasFramePointer(b, HasFramePointer);
  F->addAttributes(llvm::AttributeList::FunctionIndex, b);
}

/// Construct initial function attributes from options.
void IRGenModule::constructInitialFnAttributes(llvm::AttrBuilder &Attrs,
                                               OptimizationMode FuncOptMode) {
  // Add frame pointer attributes.
  setHasFramePointer(Attrs, IRGen.Opts.DisableFPElim);
  
  // Add target-cpu and target-features if they are non-null.
  auto *Clang = static_cast<ClangImporter *>(Context.getClangModuleLoader());
  clang::TargetOptions &ClangOpts = Clang->getTargetInfo().getTargetOpts();

  std::string &CPU = ClangOpts.CPU;
  if (CPU != "")
    Attrs.addAttribute("target-cpu", CPU);

  std::vector<std::string> Features;
  for (auto &F : ClangOpts.Features)
    if (!shouldRemoveTargetFeature(F))
        Features.push_back(F);

  if (!Features.empty()) {
    SmallString<64> allFeatures;
    // Sort so that the target features string is canonical.
    std::sort(Features.begin(), Features.end());
    interleave(Features, [&](const std::string &s) {
      allFeatures.append(s);
    }, [&]{
      allFeatures.push_back(',');
    });
    Attrs.addAttribute("target-features", allFeatures);
  }
  if (FuncOptMode == OptimizationMode::NotSet)
    FuncOptMode = IRGen.Opts.OptMode;
  if (FuncOptMode == OptimizationMode::ForSize)
    Attrs.addAttribute(llvm::Attribute::MinSize);
}

llvm::AttributeList IRGenModule::constructInitialAttributes() {
  llvm::AttrBuilder b;
  constructInitialFnAttributes(b);
  return llvm::AttributeList::get(LLVMContext,
                                  llvm::AttributeList::FunctionIndex, b);
}

llvm::Constant *IRGenModule::getInt32(uint32_t value) {
  return llvm::ConstantInt::get(Int32Ty, value);
}

llvm::Constant *IRGenModule::getSize(Size size) {
  return llvm::ConstantInt::get(SizeTy, size.getValue());
}

llvm::Constant *IRGenModule::getOpaquePtr(llvm::Constant *ptr) {
  return llvm::ConstantExpr::getBitCast(ptr, Int8PtrTy);
}

static void appendEncodedName(raw_ostream &os, StringRef name) {
  if (clang::isValidIdentifier(name)) {
    os << "_" << name;
  } else {
    for (auto c : name)
      os.write_hex(static_cast<uint8_t>(c));
  }
}

static void appendEncodedName(llvm::SmallVectorImpl<char> &buf,
                              StringRef name) {
  llvm::raw_svector_ostream os{buf};
  appendEncodedName(os, name);
}

StringRef
swift::irgen::encodeForceLoadSymbolName(llvm::SmallVectorImpl<char> &buf,
                                        StringRef name) {
  llvm::raw_svector_ostream os{buf};
  os << "_swift_FORCE_LOAD_$";
  appendEncodedName(os, name);
  return os.str();
}

llvm::SmallString<32> getTargetDependentLibraryOption(const llvm::Triple &T,
                                                      StringRef library) {
  llvm::SmallString<32> buffer;

  if (T.isWindowsMSVCEnvironment() || T.isWindowsItaniumEnvironment()) {
    bool quote = library.find(' ') != StringRef::npos;

    buffer += "/DEFAULTLIB:";
    if (quote)
      buffer += '"';
    buffer += library;
    if (!library.endswith_lower(".lib"))
      buffer += ".lib";
    if (quote)
      buffer += '"';
  } else if (T.isPS4()) {
    bool quote = library.find(' ') != StringRef::npos;

    buffer += "\01";
    if (quote)
      buffer += '"';
    buffer += library;
    if (quote)
      buffer += '"';
  } else {
    buffer += "-l";
    buffer += library;
  }

  return buffer;
}

void IRGenModule::addLinkLibrary(const LinkLibrary &linkLib) {
  llvm::LLVMContext &ctx = Module.getContext();

  // The debugger gets the autolink information directly from
  // the LinkLibraries of the module, so there's no reason to
  // emit it into the IR of debugger expressions.
  if (Context.LangOpts.DebuggerSupport)
    return;
  
  switch (linkLib.getKind()) {
  case LibraryKind::Library: {
    llvm::SmallString<32> opt =
        getTargetDependentLibraryOption(Triple, linkLib.getName());
    AutolinkEntries.push_back(
        llvm::MDNode::get(ctx, llvm::MDString::get(ctx, opt)));
    break;
  }
  case LibraryKind::Framework: {
    // If we're supposed to disable autolinking of this framework, bail out.
    auto &frameworks = IRGen.Opts.DisableAutolinkFrameworks;
    if (std::find(frameworks.begin(), frameworks.end(), linkLib.getName())
          != frameworks.end())
      return;

    llvm::Metadata *args[] = {
      llvm::MDString::get(ctx, "-framework"),
      llvm::MDString::get(ctx, linkLib.getName())
    };
    AutolinkEntries.push_back(llvm::MDNode::get(ctx, args));
    break;
  }
  }

  if (linkLib.shouldForceLoad()) {
    llvm::SmallString<64> buf;
    encodeForceLoadSymbolName(buf, linkLib.getName());
    auto ForceImportThunk = cast<llvm::Function>(
        Module.getOrInsertFunction(buf, llvm::FunctionType::get(VoidTy, false))
            .getCallee());

    const IRLinkage IRL =
        llvm::Triple(Module.getTargetTriple()).isOSBinFormatCOFF()
            ? IRLinkage::ExternalImport
            : IRLinkage::ExternalWeakImport;
    ApplyIRLinkage(IRL).to(cast<llvm::GlobalValue>(ForceImportThunk));

    buf += "_$";
    appendEncodedName(buf, IRGen.Opts.ModuleName);

    if (!Module.getGlobalVariable(buf.str())) {
      auto ref = new llvm::GlobalVariable(Module, ForceImportThunk->getType(),
                                          /*isConstant=*/true,
                                          llvm::GlobalValue::WeakODRLinkage,
                                          ForceImportThunk, buf.str());
      ApplyIRLinkage(IRLinkage::InternalWeakODR).to(ref);
      auto casted = llvm::ConstantExpr::getBitCast(ref, Int8PtrTy);
      LLVMUsed.push_back(casted);
    }
  }
}

static bool replaceModuleFlagsEntry(llvm::LLVMContext &Ctx,
                                    llvm::Module &Module, StringRef EntryName,
                                    llvm::Module::ModFlagBehavior Behavior,
                                    llvm::Metadata *Val) {
  auto *ModuleFlags = Module.getModuleFlagsMetadata();

  for (unsigned I = 0, E = ModuleFlags->getNumOperands(); I != E; ++I) {
    llvm::MDNode *Op = ModuleFlags->getOperand(I);
    llvm::MDString *ID = cast<llvm::MDString>(Op->getOperand(1));

    if (ID->getString().equals(EntryName)) {

      // Create the new entry.
      llvm::Type *Int32Ty = llvm::Type::getInt32Ty(Ctx);
      llvm::Metadata *Ops[3] = {llvm::ConstantAsMetadata::get(
                                    llvm::ConstantInt::get(Int32Ty, Behavior)),
                                llvm::MDString::get(Ctx, EntryName), Val};

      ModuleFlags->setOperand(I, llvm::MDNode::get(Ctx, Ops));
      return true;
    }
  }
  llvm_unreachable("Could not replace old linker options entry?");
}

/// Returns true if the object file generated by \p IGM will be the "first"
/// object file in the module. This lets us determine where to put a symbol
/// that must be unique.
static bool isFirstObjectFileInModule(IRGenModule &IGM) {
  if (IGM.getSILModule().isWholeModule())
    return IGM.IRGen.getPrimaryIGM() == &IGM;

  const DeclContext *DC = IGM.getSILModule().getAssociatedContext();
  if (!DC)
    return false;

  assert(!isa<ModuleDecl>(DC) && "that would be a whole module build");
  assert(isa<FileUnit>(DC) && "compiling something smaller than a file?");
  ModuleDecl *containingModule = cast<FileUnit>(DC)->getParentModule();
  return containingModule->getFiles().front() == DC;
}

void IRGenModule::emitAutolinkInfo() {
  // Collect the linker options already in the module (from ClangCodeGen).
  // FIXME: This constant should be vended by LLVM somewhere.
  auto *Metadata = Module.getOrInsertNamedMetadata("llvm.linker.options");
  for (llvm::MDNode *LinkOption : Metadata->operands())
    AutolinkEntries.push_back(LinkOption);

  // Remove duplicates.
  llvm::SmallPtrSet<llvm::MDNode *, 4> knownAutolinkEntries;
  AutolinkEntries.erase(std::remove_if(AutolinkEntries.begin(),
                                       AutolinkEntries.end(),
                                       [&](llvm::MDNode *entry) -> bool {
                                         return !knownAutolinkEntries.insert(
                                                   entry).second;
                                       }),
                        AutolinkEntries.end());

  const bool AutolinkExtractRequired =
      (TargetInfo.OutputObjectFormat == llvm::Triple::ELF && !Triple.isPS4()) ||
      TargetInfo.OutputObjectFormat == llvm::Triple::Wasm ||
      Triple.isOSCygMing();

  if (!AutolinkExtractRequired) {
    // On platforms that support autolinking, continue to use the metadata.
    Metadata->clearOperands();
    for (auto *Entry : AutolinkEntries)
      Metadata->addOperand(Entry);
  } else {
    // Merge the entries into null-separated string.
    llvm::SmallString<64> EntriesString;
    for (auto &EntryNode : AutolinkEntries) {
      const llvm::MDNode *MD = cast<llvm::MDNode>(EntryNode);
      for (auto &Entry : MD->operands()) {
        const llvm::MDString *MS = cast<llvm::MDString>(Entry);
        EntriesString += MS->getString();
        EntriesString += '\0';
      }
    }
    auto EntriesConstant = llvm::ConstantDataArray::getString(
        LLVMContext, EntriesString, /*AddNull=*/false);
    // Mark the swift1_autolink_entries section with the SHF_EXCLUDE attribute
    // to get the linker to drop it in the final linked binary.
    // LLVM doesn't provide an interface to specify section attributs in the IR
    // so we pass the attribute with inline assembly.
    if (TargetInfo.OutputObjectFormat == llvm::Triple::ELF)
      Module.appendModuleInlineAsm(".section .swift1_autolink_entries,"
                                   "\"0x80000000\"");
    auto var =
        new llvm::GlobalVariable(*getModule(), EntriesConstant->getType(), true,
                                 llvm::GlobalValue::PrivateLinkage,
                                 EntriesConstant, "_swift1_autolink_entries");
    var->setSection(".swift1_autolink_entries");
    var->setAlignment(getPointerAlignment().getValue());

    disableAddressSanitizer(*this, var);
    addUsedGlobal(var);
  }

  if (!IRGen.Opts.ForceLoadSymbolName.empty() &&
      (Triple.supportsCOMDAT() || isFirstObjectFileInModule(*this))) {
    llvm::SmallString<64> buf;
    encodeForceLoadSymbolName(buf, IRGen.Opts.ForceLoadSymbolName);
    auto ForceImportThunk =
        llvm::Function::Create(llvm::FunctionType::get(VoidTy, false),
                               llvm::GlobalValue::ExternalLinkage, buf,
                               &Module);
    ApplyIRLinkage(IRLinkage::ExternalExport).to(ForceImportThunk);
    if (Triple.supportsCOMDAT())
      if (auto *GO = cast<llvm::GlobalObject>(ForceImportThunk))
        GO->setComdat(Module.getOrInsertComdat(ForceImportThunk->getName()));

    auto BB = llvm::BasicBlock::Create(getLLVMContext(), "", ForceImportThunk);
    llvm::IRBuilder<> IRB(BB);
    IRB.CreateRetVoid();
  }
}

void IRGenModule::cleanupClangCodeGenMetadata() {
  // Remove llvm.ident that ClangCodeGen might have left in the module.
  auto *LLVMIdent = Module.getNamedMetadata("llvm.ident");
  if (LLVMIdent)
    Module.eraseNamedMetadata(LLVMIdent);

  // LLVM's object-file emission collects a fixed set of keys for the
  // image info.
  // Using "Objective-C Garbage Collection" as the key here is a hack,
  // but LLVM's object-file emission isn't general enough to collect
  // arbitrary keys to put in the image info.

  const char *ObjectiveCGarbageCollection = "Objective-C Garbage Collection";
  uint8_t Major, Minor;
  std::tie(Major, Minor) = version::getSwiftNumericVersion();
  uint32_t Value = (Major << 24) | (Minor << 16) | (swiftVersion << 8);

  if (Module.getModuleFlag(ObjectiveCGarbageCollection)) {
    bool FoundOldEntry = replaceModuleFlagsEntry(
        Module.getContext(), Module, ObjectiveCGarbageCollection,
        llvm::Module::Override,
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(Int32Ty, Value)));

    (void)FoundOldEntry;
    assert(FoundOldEntry && "Could not replace old module flag entry?");
  } else
    Module.addModuleFlag(llvm::Module::Override,
                         ObjectiveCGarbageCollection,
                         Value);
}

bool IRGenModule::finalize() {
  const char *ModuleHashVarName = "llvm.swift_module_hash";
  if (IRGen.Opts.OutputKind == IRGenOutputKind::ObjectFile &&
      !Module.getGlobalVariable(ModuleHashVarName)) {
    // Create a global variable into which we will store the hash of the
    // module (used for incremental compilation).
    // We have to create the variable now (before we emit the global lists).
    // But we want to calculate the hash later because later we can do it
    // multi-threaded.
    llvm::MD5::MD5Result zero{};
    ArrayRef<uint8_t> ZeroArr(reinterpret_cast<uint8_t *>(&zero), sizeof(zero));
    auto *ZeroConst = llvm::ConstantDataArray::get(Module.getContext(), ZeroArr);
    ModuleHash = new llvm::GlobalVariable(Module, ZeroConst->getType(), true,
                                          llvm::GlobalValue::PrivateLinkage,
                                          ZeroConst, ModuleHashVarName);
    switch (TargetInfo.OutputObjectFormat) {
    case llvm::Triple::MachO:
      // On Darwin the linker ignores the __LLVM segment.
      ModuleHash->setSection("__LLVM,__swift_modhash");
      break;
    case llvm::Triple::ELF:
    case llvm::Triple::Wasm:
      ModuleHash->setSection(".swift_modhash");
      break;
    case llvm::Triple::COFF:
      ModuleHash->setSection(".sw5hash");
      break;
    default:
      llvm_unreachable("Don't know how to emit the module hash for the selected"
                       "object format.");
    }
    addUsedGlobal(ModuleHash);
  }
  emitLazyPrivateDefinitions();

  // Finalize clang IR-generation.
  finalizeClangCodeGen();

  // If that failed, report failure up and skip the final clean-up.
  if (!ClangCodeGen->GetModule())
    return false;

  emitAutolinkInfo();
  emitGlobalLists();
  if (DebugInfo)
    DebugInfo->finalize();
  cleanupClangCodeGenMetadata();

  return true;
}

/// Emit lazy definitions that have to be emitted in this specific
/// IRGenModule.
void IRGenModule::emitLazyPrivateDefinitions() {
  emitLazyObjCProtocolDefinitions();
}

llvm::MDNode *IRGenModule::createProfileWeights(uint64_t TrueCount,
                                                uint64_t FalseCount) const {
  uint64_t MaxWeight = std::max(TrueCount, FalseCount);
  uint64_t Scale = (MaxWeight > UINT32_MAX) ? UINT32_MAX : 1;
  uint32_t ScaledTrueCount = (TrueCount / Scale) + 1;
  uint32_t ScaledFalseCount = (FalseCount / Scale) + 1;
  llvm::MDBuilder MDHelper(getLLVMContext());
  return MDHelper.createBranchWeights(ScaledTrueCount, ScaledFalseCount);
}

void IRGenModule::unimplemented(SourceLoc loc, StringRef message) {
  Context.Diags.diagnose(loc, diag::irgen_unimplemented, message);
}

void IRGenModule::fatal_unimplemented(SourceLoc loc, StringRef message) {
  Context.Diags.diagnose(loc, diag::irgen_unimplemented, message);
  llvm::report_fatal_error(llvm::Twine("unimplemented IRGen feature! ") +
                             message);
}

void IRGenModule::error(SourceLoc loc, const Twine &message) {
  SmallVector<char, 128> buffer;
  Context.Diags.diagnose(loc, diag::irgen_failure,
                         message.toStringRef(buffer));
}

bool IRGenModule::useDllStorage() { return ::useDllStorage(Triple); }

bool IRGenModule::shouldPrespecializeGenericMetadata() {
  // Prespecialize generic metadata in the standard library always, disregarding
  // flags.
  if (isStandardLibrary()) {
    return true;
  }
  auto &context = getSwiftModule()->getASTContext();
  auto deploymentAvailability =
      AvailabilityContext::forDeploymentTarget(context);
  return IRGen.Opts.PrespecializeGenericMetadata && 
    deploymentAvailability.isContainedIn(
      context.getPrespecializedGenericMetadataAvailability()) &&
    (Triple.isOSDarwin() || Triple.isTvOS() || Triple.isOSLinux());
}

void IRGenerator::addGenModule(SourceFile *SF, IRGenModule *IGM) {
  assert(GenModules.count(SF) == 0);
  GenModules[SF] = IGM;
  if (!PrimaryIGM) {
    PrimaryIGM = IGM;
  }
  Queue.push_back(IGM);
}

IRGenModule *IRGenerator::getGenModule(DeclContext *ctxt) {
  if (GenModules.size() == 1 || !ctxt) {
    return getPrimaryIGM();
  }
  SourceFile *SF = ctxt->getParentSourceFile();
  if (!SF) {
    return getPrimaryIGM();
  }
  IRGenModule *IGM = GenModules[SF];
  assert(IGM);
  return IGM;
}

IRGenModule *IRGenerator::getGenModule(SILFunction *f) {
  if (GenModules.size() == 1) {
    return getPrimaryIGM();
  }

  auto found = DefaultIGMForFunction.find(f);
  if (found != DefaultIGMForFunction.end())
    return found->second;

  if (auto *dc = f->getDeclContext())
    return getGenModule(dc);

  return getPrimaryIGM();
}

uint32_t swift::irgen::getSwiftABIVersion() {
  return IRGenModule::swiftVersion;
}

llvm::Triple IRGenerator::getEffectiveClangTriple() {
  auto CI = static_cast<ClangImporter *>(
      &*SIL.getASTContext().getClangModuleLoader());
  assert(CI && "no clang module loader");
  return llvm::Triple(CI->getTargetInfo().getTargetOpts().Triple);
}

const llvm::DataLayout &IRGenerator::getClangDataLayout() {
  return static_cast<ClangImporter *>(
             SIL.getASTContext().getClangModuleLoader())
      ->getTargetInfo()
      .getDataLayout();
  }

TypeExpansionContext IRGenModule::getMaximalTypeExpansionContext() const {
  return TypeExpansionContext::maximal(getSwiftModule(),
                                       getSILModule().isWholeModule());
}
