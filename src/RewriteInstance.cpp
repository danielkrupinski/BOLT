//===--- RewriteInstance.cpp - Interface for machine-level function -------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//


#include "BinaryBasicBlock.h"
#include "BinaryContext.h"
#include "BinaryFunction.h"
#include "BinaryPassManager.h"
#include "CacheMetrics.h"
#include "DataAggregator.h"
#include "DataReader.h"
#include "Exceptions.h"
#include "MCPlusBuilder.h"
#include "ProfileReader.h"
#include "ProfileWriter.h"
#include "RewriteInstance.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugLine.h"
#include "llvm/ExecutionEngine/Orc/LambdaResolver.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/RTDyldMemoryManager.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAsmLayout.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCDwarf.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrAnalysis.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCObjectStreamer.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSection.h"
#include "llvm/MC/MCSectionELF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Object/SymbolicFile.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/DataExtractor.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include <algorithm>
#include <fstream>
#include <stack>
#include <system_error>

#undef  DEBUG_TYPE
#define DEBUG_TYPE "bolt"

using namespace llvm;
using namespace object;
using namespace bolt;

namespace opts {

extern cl::OptionCategory BoltCategory;
extern cl::OptionCategory BoltDiffCategory;
extern cl::OptionCategory BoltOptCategory;
extern cl::OptionCategory BoltOutputCategory;
extern cl::OptionCategory AggregatorCategory;

extern cl::opt<MacroFusionType> AlignMacroOpFusion;
extern cl::opt<JumpTableSupportLevel> JumpTables;
extern cl::list<std::string> ReorderData;

static cl::opt<bool>
ForceToDataRelocations("force-data-relocations",
  cl::desc("force relocations to data sections to always be processed"),
  cl::init(false),
  cl::Hidden,
  cl::ZeroOrMore,
  cl::cat(BoltCategory));

// Note: enabling this is liable to make things break.
static cl::opt<bool>
AllowSectionRelocations("allow-section-relocations",
  cl::desc("allow reordering of data referenced by section relocations "
           "(experimental)"),
  cl::init(false),
  cl::Hidden,
  cl::ZeroOrMore,
  cl::cat(BoltOptCategory));

static cl::opt<bool>
PrintCacheMetrics("print-cache-metrics",
  cl::desc("calculate and print various metrics for instruction cache"),
  cl::init(false),
  cl::ZeroOrMore,
  cl::cat(BoltOptCategory));

cl::opt<std::string>
OutputFilename("o",
  cl::desc("<output file>"),
  cl::Optional,
  cl::cat(BoltOutputCategory));

cl::opt<bool>
AllowStripped("allow-stripped",
  cl::desc("allow processing of stripped binaries"),
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<std::string>
BoltProfile("b",
  cl::desc("<bolt profile>"),
  cl::cat(BoltCategory));

static cl::list<std::string>
BreakFunctionNames("break-funcs",
  cl::CommaSeparated,
  cl::desc("list of functions to core dump on (debugging)"),
  cl::value_desc("func1,func2,func3,..."),
  cl::Hidden,
  cl::cat(BoltCategory));

cl::opt<bool>
DumpDotAll("dump-dot-all",
  cl::desc("dump function CFGs to graphviz format after each stage"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<bool>
DumpEHFrame("dump-eh-frame",
  cl::desc("dump parsed .eh_frame (debugging)"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<bool>
FixDebugInfoLargeFunctions("fix-debuginfo-large-functions",
  cl::init(true),
  cl::desc("do another pass if we encounter large functions, to correct their "
           "debug info."),
  cl::ZeroOrMore,
  cl::ReallyHidden,
  cl::cat(BoltCategory));

static cl::list<std::string>
FunctionNames("funcs",
  cl::CommaSeparated,
  cl::desc("list of functions to optimize"),
  cl::value_desc("func1,func2,func3,..."),
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<std::string>
FunctionNamesFile("funcs-file",
  cl::desc("file with list of functions to optimize"),
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::list<std::string>
FunctionPadSpec("pad-funcs",
  cl::CommaSeparated,
  cl::desc("list of functions to pad with amount of bytes"),
  cl::value_desc("func1:pad1,func2:pad2,func3:pad3,..."),
  cl::Hidden,
  cl::cat(BoltCategory));

cl::opt<bool>
HotText("hot-text",
  cl::desc("hot text symbols support (relocation mode)"),
  cl::ZeroOrMore,
  cl::cat(BoltCategory));

static cl::opt<bool>
HotData("hot-data",
  cl::desc("hot data symbols support (relocation mode)"),
  cl::ZeroOrMore,
  cl::cat(BoltCategory));

static cl::opt<bool>
UpdateEnd("update-end",
  cl::desc("update the _end symbol to point to the end of all data sections"),
  cl::init(true),
  cl::ZeroOrMore,
  cl::cat(BoltCategory));

static cl::opt<bool>
KeepTmp("keep-tmp",
  cl::desc("preserve intermediate .o file"),
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<bool>
MarkFuncs("mark-funcs",
  cl::desc("mark function boundaries with break instruction to make "
           "sure we accidentally don't cross them"),
  cl::ReallyHidden,
  cl::ZeroOrMore,
  cl::cat(BoltCategory));

static cl::opt<unsigned>
MaxFunctions("max-funcs",
  cl::desc("maximum number of functions to overwrite"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<unsigned>
MaxDataRelocations("max-data-relocations",
  cl::desc("maximum number of data relocations to process"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

cl::opt<bool>
PrintAll("print-all",
  cl::desc("print functions after each stage"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<bool>
PrintCFG("print-cfg",
  cl::desc("print functions after CFG construction"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<bool>
PrintDisasm("print-disasm",
  cl::desc("print function after disassembly"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<bool>
PrintGlobals("print-globals",
  cl::desc("print global symbols after disassembly"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<bool>
PrintSections("print-sections",
  cl::desc("print all registered sections"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<bool>
PrintLoopInfo("print-loops",
  cl::desc("print loop related information"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<cl::boolOrDefault>
RelocationMode("relocs",
  cl::desc("use relocations in the binary (default=autodetect)"),
  cl::ZeroOrMore,
  cl::cat(BoltCategory));

static cl::opt<std::string>
SaveProfile("w",
  cl::desc("save recorded profile to a file"),
  cl::cat(BoltOutputCategory));

static cl::list<std::string>
SkipFunctionNames("skip-funcs",
  cl::CommaSeparated,
  cl::desc("list of functions to skip"),
  cl::value_desc("func1,func2,func3,..."),
  cl::Hidden,
  cl::cat(BoltCategory));

static cl::opt<std::string>
SkipFunctionNamesFile("skip-funcs-file",
  cl::desc("file with list of functions to skip"),
  cl::Hidden,
  cl::cat(BoltCategory));

cl::opt<BinaryFunction::SplittingType>
SplitFunctions("split-functions",
  cl::desc("split functions into hot and cold regions"),
  cl::init(BinaryFunction::ST_NONE),
  cl::values(clEnumValN(BinaryFunction::ST_NONE, "0",
                        "do not split any function"),
             clEnumValN(BinaryFunction::ST_EH, "1",
                        "split all landing pads"),
             clEnumValN(BinaryFunction::ST_LARGE, "2",
                        "also split if function too large to fit"),
             clEnumValN(BinaryFunction::ST_ALL, "3",
                        "split all functions")),
  cl::ZeroOrMore,
  cl::cat(BoltOptCategory));

cl::opt<bool>
TrapOldCode("trap-old-code",
  cl::desc("insert traps in old function bodies (relocation mode)"),
  cl::Hidden,
  cl::cat(BoltCategory));

cl::opt<bool>
UpdateDebugSections("update-debug-sections",
  cl::desc("update DWARF debug sections of the executable"),
  cl::ZeroOrMore,
  cl::cat(BoltCategory));

static cl::opt<bool>
UseGnuStack("use-gnu-stack",
  cl::desc("use GNU_STACK program header for new segment (workaround for "
           "issues with strip/objcopy)"),
  cl::ZeroOrMore,
  cl::cat(BoltCategory));

cl::opt<bool>
UseOldText("use-old-text",
  cl::desc("re-use space in old .text if possible (relocation mode)"),
  cl::cat(BoltCategory));

// The default verbosity level (0) is pretty terse, level 1 is fairly
// verbose and usually prints some informational message for every
// function processed.  Level 2 is for the noisiest of messages and
// often prints a message per basic block.
// Error messages should never be suppressed by the verbosity level.
// Only warnings and info messages should be affected.
//
// The rational behind stream usage is as follows:
// outs() for info and debugging controlled by command line flags.
// errs() for errors and warnings.
// dbgs() for output within DEBUG().
cl::opt<unsigned>
Verbosity("v",
  cl::desc("set verbosity level for diagnostic output"),
  cl::init(0),
  cl::ZeroOrMore,
  cl::cat(BoltCategory));

static cl::opt<bool>
AddBoltInfo("add-bolt-info",
  cl::desc("add BOLT version and command line argument information to "
           "processed binaries"),
  cl::init(true),
  cl::cat(BoltCategory));

cl::opt<bool>
AggregateOnly("aggregate-only",
  cl::desc("exit after writing aggregated data file"),
  cl::Hidden,
  cl::cat(AggregatorCategory));

cl::opt<bool>
DiffOnly("diff-only",
  cl::desc("stop processing once we have enough to compare two binaries"),
  cl::Hidden,
  cl::cat(BoltDiffCategory));

static cl::opt<bool>
TimeRewrite("time-rewrite",
  cl::desc("print time spent in rewriting passes"),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

// Check against lists of functions from options if we should
// optimize the function with a given name.
bool shouldProcess(const BinaryFunction &Function) {
  if (opts::MaxFunctions && Function.getFunctionNumber() >= opts::MaxFunctions) {
    if (Function.getFunctionNumber() == opts::MaxFunctions)
      dbgs() << "BOLT-INFO: processing ending on " << Function << "\n";
    else
      return false;
  }

  auto populateFunctionNames = [](cl::opt<std::string> &FunctionNamesFile,
                                  cl::list<std::string> &FunctionNames) {
    assert(!FunctionNamesFile.empty() && "unexpected empty file name");
    std::ifstream FuncsFile(FunctionNamesFile, std::ios::in);
    std::string FuncName;
    while (std::getline(FuncsFile, FuncName)) {
      FunctionNames.push_back(FuncName);
    }
    FunctionNamesFile = "";
  };

  if (!FunctionNamesFile.empty())
    populateFunctionNames(FunctionNamesFile, FunctionNames);

  if (!SkipFunctionNamesFile.empty())
    populateFunctionNames(SkipFunctionNamesFile, SkipFunctionNames);

  bool IsValid = true;
  if (!FunctionNames.empty()) {
    IsValid = false;
    for (auto &Name : FunctionNames) {
      if (Function.hasName(Name)) {
        IsValid = true;
        break;
      }
    }
  }
  if (!IsValid)
    return false;

  if (!SkipFunctionNames.empty()) {
    for (auto &Name : SkipFunctionNames) {
      if (Function.hasName(Name)) {
        IsValid = false;
        break;
      }
    }
  }

  return IsValid;
}

size_t padFunction(const BinaryFunction &Function) {
  static std::map<std::string, size_t> FunctionPadding;

  if (FunctionPadding.empty() && !FunctionPadSpec.empty()) {
    for (auto &Spec : FunctionPadSpec) {
      auto N = Spec.find(':');
      if (N == std::string::npos)
        continue;
      auto Name = Spec.substr(0, N);
      auto Padding = std::stoull(Spec.substr(N+1));
      FunctionPadding[Name] = Padding;
    }
  }

  for (auto &FPI : FunctionPadding) {
    auto Name = FPI.first;
    auto Padding = FPI.second;
    if (Function.hasName(Name)) {
      return Padding;
    }
  }

  return 0;
}

} // namespace opts

extern MCPlusBuilder * createX86MCPlusBuilder(const MCInstrAnalysis *,
                                              const MCInstrInfo *,
                                              const MCRegisterInfo *);
extern MCPlusBuilder * createAArch64MCPlusBuilder(const MCInstrAnalysis *,
                                                  const MCInstrInfo *,
                                                  const MCRegisterInfo *);
namespace {

MCPlusBuilder *createMCPlusBuilder(const Triple::ArchType Arch,
    const MCInstrAnalysis *Analysis, const MCInstrInfo *Info,
    const MCRegisterInfo *RegInfo) {
  if (Arch == Triple::x86_64) {
    return createX86MCPlusBuilder(Analysis, Info, RegInfo);
  } else if (Arch == Triple::aarch64) {
    return createAArch64MCPlusBuilder(Analysis, Info, RegInfo);
  } else {
    llvm_unreachable("architecture unsupport by MCPlusBuilder");
  }
}

}

constexpr const char *RewriteInstance::SectionsToOverwrite[];

const std::string RewriteInstance::OrgSecPrefix = ".bolt.org";

const std::string RewriteInstance::BOLTSecPrefix = ".bolt";

const char RewriteInstance::TimerGroupName[] = "rewrite";
const char RewriteInstance::TimerGroupDesc[] = "Rewrite passes";

namespace llvm {
namespace bolt {
extern const char *BoltRevision;

void report_error(StringRef Message, std::error_code EC) {
  assert(EC);
  errs() << "BOLT-ERROR: '" << Message << "': " << EC.message() << ".\n";
  exit(1);
}

void report_error(StringRef Message, Error E) {
  assert(E);
  errs() << "BOLT-ERROR: '" << Message << "': " << toString(std::move(E))
         << ".\n";
  exit(1);
}

void check_error(std::error_code EC, StringRef Message) {
  if (!EC)
    return;
  report_error(Message, EC);
}

}
}

namespace {

std::string uniquifyName(BinaryContext &BC, std::string NamePrefix) {
  unsigned LocalID = 1;
  while (BC.getBinaryDataByName(NamePrefix + std::to_string(LocalID)))
    ++LocalID;
  return NamePrefix + std::to_string(LocalID);
}

bool refersToReorderedSection(ErrorOr<BinarySection &> Section) {
  auto Itr = std::find_if(opts::ReorderData.begin(),
                          opts::ReorderData.end(),
                          [&](const std::string &SectionName) {
                            return (Section &&
                                    Section->getName() == SectionName);
                          });
  return Itr != opts::ReorderData.end();
}

}

uint8_t *ExecutableFileMemoryManager::allocateSection(intptr_t Size,
                                                      unsigned Alignment,
                                                      unsigned SectionID,
                                                      StringRef SectionName,
                                                      bool IsCode,
                                                      bool IsReadOnly) {
  // Register as note section (non-allocatable) if we recognize it as so
  for (auto &OverwriteName : RewriteInstance::SectionsToOverwrite) {
    if (SectionName == OverwriteName) {
      uint8_t *DataCopy = new uint8_t[Size];
      auto &Section = BC.registerOrUpdateNoteSection(SectionName,
                                                     DataCopy,
                                                     Size,
                                                     Alignment);
      Section.setSectionID(SectionID);
      assert(!Section.isAllocatable() && "note sections cannot be allocatable");
      return DataCopy;
    }
  }

  uint8_t *Ret;
  if (IsCode) {
    Ret = SectionMemoryManager::allocateCodeSection(Size, Alignment,
                                                    SectionID, SectionName);
  } else {
    Ret = SectionMemoryManager::allocateDataSection(Size, Alignment,
                                                    SectionID, SectionName,
                                                    IsReadOnly);
  }

  const auto Flags = BinarySection::getFlags(IsReadOnly, IsCode, true);
  auto &Section = BC.registerOrUpdateSection(SectionName,
                                             ELF::SHT_PROGBITS,
                                             Flags,
                                             Ret,
                                             Size,
                                             Alignment);
  Section.setSectionID(SectionID);
  assert(Section.isAllocatable() &&
         "verify that allocatable is marked as allocatable");

  DEBUG(dbgs() << "BOLT: allocating " << (Section.isLocal() ? "local " : "")
               << (IsCode ? "code" : (IsReadOnly ? "read-only data" : "data"))
               << " section : " << SectionName
               << " with size " << Size << ", alignment " << Alignment
               << " at 0x" << Ret << ", ID = " << SectionID << "\n");

  return Ret;
}

/// Notifier for non-allocatable (note) section.
uint8_t *ExecutableFileMemoryManager::recordNoteSection(
    const uint8_t *Data,
    uintptr_t Size,
    unsigned Alignment,
    unsigned SectionID,
    StringRef SectionName) {
  DEBUG(dbgs() << "BOLT: note section "
               << SectionName
               << " with size " << Size << ", alignment " << Alignment
               << " at 0x"
               << Twine::utohexstr(reinterpret_cast<uint64_t>(Data)) << '\n');
  auto &Section = BC.registerOrUpdateNoteSection(SectionName,
                                                 copyByteArray(Data, Size),
                                                 Size,
                                                 Alignment);
  Section.setSectionID(SectionID);
  assert(!Section.isAllocatable() && "note sections cannot be allocatable");
  return Section.getOutputData();
}

bool ExecutableFileMemoryManager::finalizeMemory(std::string *ErrMsg) {
  DEBUG(dbgs() << "BOLT: finalizeMemory()\n");
  return SectionMemoryManager::finalizeMemory(ErrMsg);
}

ExecutableFileMemoryManager::~ExecutableFileMemoryManager() { }

namespace {

StringRef getSectionName(SectionRef Section) {
  StringRef SectionName;
  Section.getName(SectionName);
  return SectionName;
}

/// Create BinaryContext for a given architecture \p ArchName and
/// triple \p TripleName.
std::unique_ptr<BinaryContext>
createBinaryContext(ELFObjectFileBase *File, DataReader &DR,
                    std::unique_ptr<DWARFContext> DwCtx) {
  std::string ArchName;
  std::string TripleName;
  llvm::Triple::ArchType Arch = (llvm::Triple::ArchType)File->getArch();
  if (Arch == llvm::Triple::x86_64) {
    ArchName = "x86-64";
    TripleName = "x86_64-unknown-linux";
  } else if (Arch == llvm::Triple::aarch64) {
    ArchName = "aarch64";
    TripleName = "aarch64-unknown-linux";
  } else {
    errs() << "BOLT-ERROR: Unrecognized machine in ELF file.\n";
    return nullptr;
  }

  std::string Error;
  std::unique_ptr<Triple> TheTriple = llvm::make_unique<Triple>(TripleName);
  const Target *TheTarget = TargetRegistry::lookupTarget(ArchName,
                                                         *TheTriple,
                                                         Error);
  if (!TheTarget) {
    errs() << "BOLT-ERROR: " << Error;
    return nullptr;
  }

  std::unique_ptr<const MCRegisterInfo> MRI(
      TheTarget->createMCRegInfo(TripleName));
  if (!MRI) {
    errs() << "BOLT-ERROR: no register info for target " << TripleName << "\n";
    return nullptr;
  }

  // Set up disassembler.
  std::unique_ptr<const MCAsmInfo> AsmInfo(
      TheTarget->createMCAsmInfo(*MRI, TripleName));
  if (!AsmInfo) {
    errs() << "BOLT-ERROR: no assembly info for target " << TripleName << "\n";
    return nullptr;
  }

  std::unique_ptr<const MCSubtargetInfo> STI(
      TheTarget->createMCSubtargetInfo(TripleName, "", ""));
  if (!STI) {
    errs() << "BOLT-ERROR: no subtarget info for target " << TripleName << "\n";
    return nullptr;
  }

  std::unique_ptr<const MCInstrInfo> MII(TheTarget->createMCInstrInfo());
  if (!MII) {
    errs() << "BOLT-ERROR: no instruction info for target " << TripleName
           << "\n";
    return nullptr;
  }

  std::unique_ptr<MCObjectFileInfo> MOFI =
    llvm::make_unique<MCObjectFileInfo>();
  std::unique_ptr<MCContext> Ctx =
    llvm::make_unique<MCContext>(AsmInfo.get(), MRI.get(), MOFI.get());
  MOFI->InitMCObjectFileInfo(*TheTriple, /*PIC=*/false, *Ctx);

  std::unique_ptr<MCDisassembler> DisAsm(
    TheTarget->createMCDisassembler(*STI, *Ctx));

  if (!DisAsm) {
    errs() << "BOLT-ERROR: no disassembler for target " << TripleName << "\n";
    return nullptr;
  }

  std::unique_ptr<const MCInstrAnalysis> MIA(
     TheTarget->createMCInstrAnalysis(MII.get()));
  if (!MIA) {
    errs() << "BOLT-ERROR: failed to create instruction analysis for target"
           << TripleName << "\n";
    return nullptr;
  }


  std::unique_ptr<MCPlusBuilder> MIB(
    createMCPlusBuilder(Arch, MIA.get(), MII.get(), MRI.get()));
  if (!MIB) {
    errs() << "BOLT-ERROR: failed to create instruction builder for target"
           << TripleName << "\n";
    return nullptr;
  }

  int AsmPrinterVariant = AsmInfo->getAssemblerDialect();
  std::unique_ptr<MCInstPrinter> InstructionPrinter(
      TheTarget->createMCInstPrinter(Triple(TripleName), AsmPrinterVariant,
                                     *AsmInfo, *MII, *MRI));
  if (!InstructionPrinter) {
    errs() << "BOLT-ERROR: no instruction printer for target " << TripleName
           << '\n';
    return nullptr;
  }
  InstructionPrinter->setPrintImmHex(true);

  std::unique_ptr<MCCodeEmitter> MCE(
      TheTarget->createMCCodeEmitter(*MII, *MRI, *Ctx));

  // Make sure we don't miss any output on core dumps.
  outs().SetUnbuffered();
  errs().SetUnbuffered();
  dbgs().SetUnbuffered();

  auto BC =
      llvm::make_unique<BinaryContext>(std::move(Ctx),
                                       std::move(DwCtx),
                                       std::move(TheTriple),
                                       TheTarget,
                                       TripleName,
                                       std::move(MCE),
                                       std::move(MOFI),
                                       std::move(AsmInfo),
                                       std::move(MII),
                                       std::move(STI),
                                       std::move(InstructionPrinter),
                                       std::move(MIA),
                                       std::move(MIB),
                                       std::move(MRI),
                                       std::move(DisAsm),
                                       DR);

  return BC;
}

} // namespace

RewriteInstance::RewriteInstance(ELFObjectFileBase *File, DataReader &DR,
                                 DataAggregator &DA, const int Argc,
                                 const char *const *Argv)
    : InputFile(File), Argc(Argc), Argv(Argv), DA(DA),
      BC(createBinaryContext(
          File, DR,
          DWARFContext::create(*File, nullptr,
                               DWARFContext::defaultErrorHandler, "", false))),
      SHStrTab(StringTableBuilder::ELF) {}

RewriteInstance::~RewriteInstance() {}

void RewriteInstance::reset() {
  BinaryFunctions.clear();
  FileSymRefs.clear();
  auto &DR = BC->DR;
  BC = createBinaryContext(
      InputFile, DR,
      DWARFContext::create(*InputFile, nullptr,
                           DWARFContext::defaultErrorHandler, "", false));
  CFIRdWrt.reset(nullptr);
  OLT.reset(nullptr);
  EFMM.reset();
  Out.reset(nullptr);
  EHFrame = nullptr;
  FailedAddresses.clear();
  RangesSectionsWriter.reset();
  LocationListWriter.reset();
}

void RewriteInstance::discoverStorage() {
  NamedRegionTimer T("discoverStorage", "discover storage", TimerGroupName,
                     TimerGroupDesc, opts::TimeRewrite);

  // Stubs are harmful because RuntimeDyld may try to increase the size of
  // sections accounting for stubs when we need those sections to match the
  // same size seen in the input binary, in case this section is a copy
  // of the original one seen in the binary.
  EFMM.reset(new ExecutableFileMemoryManager(*BC, /*AllowStubs*/ false));

  auto ELF64LEFile = dyn_cast<ELF64LEObjectFile>(InputFile);
  if (!ELF64LEFile) {
    errs() << "BOLT-ERROR: only 64-bit LE ELF binaries are supported\n";
    exit(1);
  }
  auto Obj = ELF64LEFile->getELFFile();

  EntryPoint = Obj->getHeader()->e_entry;

  // This is where the first segment and ELF header were allocated.
  uint64_t FirstAllocAddress = std::numeric_limits<uint64_t>::max();

  NextAvailableAddress = 0;
  uint64_t NextAvailableOffset = 0;
  auto PHs = cantFail(Obj->program_headers(), "program_headers() failed");
  for (const auto &Phdr : PHs) {
    if (Phdr.p_type == ELF::PT_LOAD) {
      FirstAllocAddress = std::min(FirstAllocAddress,
                                   static_cast<uint64_t>(Phdr.p_vaddr));
      NextAvailableAddress = std::max(NextAvailableAddress,
                                      Phdr.p_vaddr + Phdr.p_memsz);
      NextAvailableOffset = std::max(NextAvailableOffset,
                                     Phdr.p_offset + Phdr.p_filesz);

      EFMM->SegmentMapInfo[Phdr.p_vaddr] = SegmentInfo{Phdr.p_vaddr,
                                                       Phdr.p_memsz,
                                                       Phdr.p_offset,
                                                       Phdr.p_filesz};
    }
  }

  for (const auto &Section : InputFile->sections()) {
    StringRef SectionName;
    Section.getName(SectionName);
    StringRef SectionContents;
    Section.getContents(SectionContents);
    if (SectionName == ".text") {
      BC->OldTextSectionAddress = Section.getAddress();
      BC->OldTextSectionSize = Section.getSize();
      BC->OldTextSectionOffset =
        SectionContents.data() - InputFile->getData().data();
    }

    if (SectionName.startswith(OrgSecPrefix) ||
        SectionName.startswith(BOLTSecPrefix)) {
      errs() << "BOLT-ERROR: input file was processed by BOLT. "
                "Cannot re-optimize.\n";
      exit(1);
    }
  }

  assert(NextAvailableAddress && NextAvailableOffset &&
         "no PT_LOAD pheader seen");

  outs() << "BOLT-INFO: first alloc address is 0x"
         << Twine::utohexstr(FirstAllocAddress) << '\n';

  FirstNonAllocatableOffset = NextAvailableOffset;

  NextAvailableAddress = alignTo(NextAvailableAddress, PageAlign);
  NextAvailableOffset = alignTo(NextAvailableOffset, PageAlign);

  if (!opts::UseGnuStack) {
    // This is where the black magic happens. Creating PHDR table in a segment
    // other than that containing ELF header is tricky. Some loaders and/or
    // parts of loaders will apply e_phoff from ELF header assuming both are in
    // the same segment, while others will do the proper calculation.
    // We create the new PHDR table in such a way that both of the methods
    // of loading and locating the table work. There's a slight file size
    // overhead because of that.
    //
    // NB: bfd's strip command cannot do the above and will corrupt the
    //     binary during the process of stripping non-allocatable sections.
    if (NextAvailableOffset <= NextAvailableAddress - FirstAllocAddress) {
      NextAvailableOffset = NextAvailableAddress - FirstAllocAddress;
    } else {
      NextAvailableAddress = NextAvailableOffset + FirstAllocAddress;
    }
    assert(NextAvailableOffset == NextAvailableAddress - FirstAllocAddress &&
           "PHDR table address calculation error");

    outs() << "BOLT-INFO: creating new program header table at address 0x"
           << Twine::utohexstr(NextAvailableAddress) << ", offset 0x"
           << Twine::utohexstr(NextAvailableOffset) << '\n';

    PHDRTableAddress = NextAvailableAddress;
    PHDRTableOffset = NextAvailableOffset;

    // Reserve space for 3 extra pheaders.
    unsigned Phnum = Obj->getHeader()->e_phnum;
    Phnum += 3;

    NextAvailableAddress += Phnum * sizeof(ELFFile<ELF64LE>::Elf_Phdr);
    NextAvailableOffset  += Phnum * sizeof(ELFFile<ELF64LE>::Elf_Phdr);
  }

  // Align at cache line.
  NextAvailableAddress = alignTo(NextAvailableAddress, 64);
  NextAvailableOffset = alignTo(NextAvailableOffset, 64);

  NewTextSegmentAddress = NextAvailableAddress;
  NewTextSegmentOffset = NextAvailableOffset;
  BC->LayoutStartAddress = NextAvailableAddress;
}

Optional<std::string>
RewriteInstance::getBuildID() const {
  for (auto &Section : InputFile->sections()) {
    StringRef SectionName;
    Section.getName(SectionName);

    if (SectionName != ".note.gnu.build-id")
      continue;

    StringRef SectionContents;
    Section.getContents(SectionContents);

    // Reading notes section (see Portable Formats Specification, Version 1.1,
    // pg 2-5, section "Note Section").
    DataExtractor DE = DataExtractor(SectionContents, true, 8);
    uint32_t Offset = 0;
    if (!DE.isValidOffset(Offset))
      return NoneType();
    uint32_t NameSz = DE.getU32(&Offset);
    if (!DE.isValidOffset(Offset))
      return NoneType();
    uint32_t DescSz = DE.getU32(&Offset);
    if (!DE.isValidOffset(Offset))
      return NoneType();
    uint32_t Type = DE.getU32(&Offset);

    DEBUG(dbgs() << "NameSz = " << NameSz << "; DescSz = " << DescSz
                 << "; Type = " << Type << "\n");

    // Type 3 is a GNU build-id note section
    if (Type != 3)
      return NoneType();

    StringRef Name = SectionContents.slice(Offset, Offset + NameSz);
    Offset = alignTo(Offset + NameSz, 4);
    StringRef BinaryBuildID = SectionContents.slice(Offset, Offset + DescSz);
    if (Name.substr(0, 3) != "GNU")
      return NoneType();

    std::string Str;
    raw_string_ostream OS(Str);
    auto CharIter = BinaryBuildID.bytes_begin();
    while (CharIter != BinaryBuildID.bytes_end()) {
      if (*CharIter < 0x10)
        OS << "0";
      OS << Twine::utohexstr(*CharIter);
      ++CharIter;
    }
    outs() << "BOLT-INFO: binary build-id is:     " << OS.str() << "\n";
    return OS.str();
  }
  return NoneType();
}

void RewriteInstance::run() {
  if (!BC) {
    errs() << "BOLT-ERROR: failed to create a binary context\n";
    return;
  }

  auto executeRewritePass = [&](const std::set<uint64_t> &NonSimpleFunctions) {
    discoverStorage();
    readSpecialSections();
    adjustCommandLineOptions();
    discoverFileObjects();
    readDebugInfo();
    disassembleFunctions();
    processProfileData();
    if (opts::AggregateOnly)
      return;
    postProcessFunctions();
    for (uint64_t Address : NonSimpleFunctions) {
      auto FI = BinaryFunctions.find(Address);
      assert(FI != BinaryFunctions.end() && "bad non-simple function address");
      FI->second.setSimple(false);
    }
    if (opts::DiffOnly)
      return;
    runOptimizationPasses();
    emitFunctions();
  };

  outs() << "BOLT-INFO: Target architecture: "
         << Triple::getArchTypeName(
                (llvm::Triple::ArchType)InputFile->getArch())
         << "\n";

  if (DA.started()) {
    if (auto FileBuildID = getBuildID()) {
      DA.processFileBuildID(*FileBuildID);
    } else {
      errs() << "BOLT-WARNING: build-id will not be checked because we could "
                "not read one from input binary\n";
    }
  }

  unsigned PassNumber = 1;
  executeRewritePass({});
  if (opts::AggregateOnly || opts::DiffOnly)
    return;

  if (opts::SplitFunctions == BinaryFunction::ST_LARGE &&
      checkLargeFunctions()) {
    ++PassNumber;
    // Emit again because now some functions have been split
    outs() << "BOLT: split-functions: starting pass " << PassNumber << "...\n";
    reset();
    executeRewritePass({});
  }

  // Emit functions again ignoring functions which still didn't fit in their
  // original space, so that we don't generate incorrect debugging information
  // for them (information that would reflect the optimized version).
  if (opts::UpdateDebugSections && opts::FixDebugInfoLargeFunctions &&
      checkLargeFunctions()) {
    ++PassNumber;
    outs() << "BOLT: starting pass (ignoring large functions) "
           << PassNumber << "...\n";
    reset();
    executeRewritePass(LargeFunctions);
  }

  if (opts::UpdateDebugSections)
    updateDebugInfo();

  addBoltInfoSection();

  // Copy allocatable part of the input.
  std::error_code EC;
  Out = llvm::make_unique<ToolOutputFile>(opts::OutputFilename, EC,
                                          sys::fs::F_None, 0777);
  check_error(EC, "cannot create output executable file");
  Out->os() << InputFile->getData().substr(0, FirstNonAllocatableOffset);

  // Rewrite allocatable contents and copy non-allocatable parts with mods.
  rewriteFile();
}

void RewriteInstance::discoverFileObjects() {
  NamedRegionTimer T("discoverFileObjects", "discover file objects",
                     TimerGroupName, TimerGroupDesc, opts::TimeRewrite);

  FileSymRefs.clear();
  BinaryFunctions.clear();
  BC->clearBinaryData();

  // For local symbols we want to keep track of associated FILE symbol name for
  // disambiguation by combined name.
  StringRef  FileSymbolName;
  bool SeenFileName = false;
  struct SymbolRefHash {
    std::size_t operator()(SymbolRef const &S) const {
      return std::hash<decltype(DataRefImpl::p)>{}(S.getRawDataRefImpl().p);
    }
  };
  std::unordered_map<SymbolRef, StringRef, SymbolRefHash> SymbolToFileName;
  for (const auto &Symbol : InputFile->symbols()) {
    auto NameOrError = Symbol.getName();
    if (NameOrError && NameOrError->startswith("__asan_init")) {
      errs() << "BOLT-ERROR: input file was compiled or linked with sanitizer "
                "support. Cannot optimize.\n";
      exit(1);
    }
    if (NameOrError && NameOrError->startswith("__llvm_coverage_mapping")) {
      errs() << "BOLT-ERROR: input file was compiled or linked with coverage "
                "support. Cannot optimize.\n";
      exit(1);
    }

    if (Symbol.getFlags() & SymbolRef::SF_Undefined)
      continue;

    if (cantFail(Symbol.getType()) == SymbolRef::ST_File) {
      auto Name =
          cantFail(std::move(NameOrError), "cannot get symbol name for file");
      // Ignore Clang LTO artificial FILE symbol as it is not always generated,
      // and this uncertainty is causing havoc in function name matching.
      if (Name == "ld-temp.o")
        continue;
      FileSymbolName = Name;
      SeenFileName = true;
      continue;
    }
    if (!FileSymbolName.empty() &&
        !(Symbol.getFlags() & SymbolRef::SF_Global)) {
      SymbolToFileName[Symbol] = FileSymbolName;
    }
  }

  // Sort symbols in the file by value.
  std::vector<SymbolRef> SortedFileSymbols(InputFile->symbol_begin(),
                                           InputFile->symbol_end());
  std::stable_sort(SortedFileSymbols.begin(), SortedFileSymbols.end(),
                   [](const SymbolRef &A, const SymbolRef &B) {
                     // FUNC symbols have higher precedence.
                     auto AddressA = cantFail(A.getAddress());
                     auto AddressB = cantFail(B.getAddress());
                     if (AddressA == AddressB) {
                       return cantFail(A.getType()) == SymbolRef::ST_Function &&
                              cantFail(B.getType()) != SymbolRef::ST_Function;
                     }
                     return AddressA < AddressB;
                   });

  // For aarch64, the ABI defines mapping symbols so we identify data in the
  // code section (see IHI0056B). $d identifies data contents.
  auto MarkersBegin = SortedFileSymbols.end();
  if (BC->isAArch64()) {
    MarkersBegin = std::stable_partition(
        SortedFileSymbols.begin(), SortedFileSymbols.end(),
        [](const SymbolRef &Symbol) {
          StringRef Name = cantFail(Symbol.getName());
          return !(cantFail(Symbol.getType()) == SymbolRef::ST_Unknown &&
                   (Name == "$d" || Name == "$x"));
        });
  }

  auto getNextAddress = [&](std::vector<SymbolRef>::const_iterator Itr) {
    auto Section = cantFail(Itr->getSection());
    const auto SymbolEndAddress =
        (cantFail(Itr->getAddress()) + ELFSymbolRef(*Itr).getSize());

    // absolute sym
    if (Section == InputFile->section_end())
      return SymbolEndAddress;

    while (Itr != MarkersBegin - 1 &&
           cantFail(std::next(Itr)->getSection()) == Section &&
           cantFail(std::next(Itr)->getAddress()) ==
               cantFail(Itr->getAddress())) {
      ++Itr;
    }

    if (Itr != MarkersBegin - 1 &&
        cantFail(std::next(Itr)->getSection()) == Section)
      return cantFail(std::next(Itr)->getAddress());

    const auto SectionEndAddress = Section->getAddress() + Section->getSize();
    if ((ELFSectionRef(*Section).getFlags() & ELF::SHF_TLS) ||
        SymbolEndAddress > SectionEndAddress)
      return SymbolEndAddress;

    return SectionEndAddress;
  };

  BinaryFunction *PreviousFunction = nullptr;
  unsigned AnonymousId = 0;

  for (auto ISym = SortedFileSymbols.begin(); ISym != MarkersBegin; ++ISym) {
    const auto &Symbol = *ISym;
    // Keep undefined symbols for pretty printing?
    if (Symbol.getFlags() & SymbolRef::SF_Undefined)
      continue;

    if (cantFail(Symbol.getType()) == SymbolRef::ST_File)
      continue;

    StringRef SymName = cantFail(Symbol.getName(), "cannot get symbol name");
    uint64_t Address =
        cantFail(Symbol.getAddress(), "cannot get symbol address");
    if (Address == 0) {
      if (opts::Verbosity >= 1 &&
          cantFail(Symbol.getType()) == SymbolRef::ST_Function)
        errs() << "BOLT-WARNING: function with 0 address seen\n";
      continue;
    }

    FileSymRefs[Address] = Symbol;

    /// It is possible we are seeing a globalized local. LLVM might treat it as
    /// a local if it has a "private global" prefix, e.g. ".L". Thus we have to
    /// change the prefix to enforce global scope of the symbol.
    std::string Name = SymName.startswith(BC->AsmInfo->getPrivateGlobalPrefix())
                           ? "PG" + std::string(SymName)
                           : std::string(SymName);

    // Disambiguate all local symbols before adding to symbol table.
    // Since we don't know if we will see a global with the same name,
    // always modify the local name.
    //
    // NOTE: the naming convention for local symbols should match
    //       the one we use for profile data.
    std::string UniqueName;
    std::string AlternativeName;
    if (Name.empty()) {
      if (PLTSection && PLTSection->getAddress() == Address) {
        // Don't register BOLT_PLT_PSEUDO twice.
        continue;
      }
      UniqueName = "ANONYMOUS." + std::to_string(AnonymousId++);
    } else if (Symbol.getFlags() & SymbolRef::SF_Global) {
      assert(!BC->getBinaryDataByName(Name) && "global name not unique");
      UniqueName = Name;
    } else {
      // If we have a local file name, we should create 2 variants for the
      // function name. The reason is that perf profile might have been
      // collected on a binary that did not have the local file name (e.g. as
      // a side effect of stripping debug info from the binary):
      //
      //   primary:     <function>/<id>
      //   alternative: <function>/<file>/<id2>
      //
      // The <id> field is used for disambiguation of local symbols since there
      // could be identical function names coming from identical file names
      // (e.g. from different directories).
      std::string Prefix = Name + "/";
      std::string AltPrefix;
      auto SFI = SymbolToFileName.find(Symbol);
      if (SFI != SymbolToFileName.end()) {
        AltPrefix = Prefix + std::string(SFI->second) + "/";
      }

      UniqueName = uniquifyName(*BC, Prefix);
      if (!AltPrefix.empty())
        AlternativeName = uniquifyName(*BC, AltPrefix);
    }

    uint64_t SymbolSize = ELFSymbolRef(Symbol).getSize();
    uint64_t NextAddress = getNextAddress(ISym);
    uint64_t TentativeSize = !SymbolSize ? NextAddress - Address : SymbolSize;
    uint64_t SymbolAlignment = Symbol.getAlignment();
    unsigned SymbolFlags = Symbol.getFlags();

    auto registerName = [&](uint64_t FinalSize) {
      // Register names even if it's not a function, e.g. for an entry point.
      BC->registerNameAtAddress(UniqueName, Address, FinalSize,
                                SymbolAlignment, SymbolFlags);
      if (!AlternativeName.empty())
        BC->registerNameAtAddress(AlternativeName, Address, FinalSize,
                                  SymbolAlignment, SymbolFlags);
    };

    section_iterator Section =
        cantFail(Symbol.getSection(), "cannot get symbol section");
    if (Section == InputFile->section_end()) {
      // Could be an absolute symbol. Could record for pretty printing.
      DEBUG(if (opts::Verbosity > 1) {
          dbgs() << "BOLT-INFO: absolute sym " << UniqueName << "\n";
        });
      registerName(TentativeSize);
      continue;
    }

    DEBUG(dbgs() << "BOLT-DEBUG: considering symbol " << UniqueName
                 << " for function\n");

    if (!Section->isText()) {
      assert(cantFail(Symbol.getType()) != SymbolRef::ST_Function &&
             "unexpected function inside non-code section");
      DEBUG(dbgs() << "BOLT-DEBUG: rejecting as symbol is not in code\n");
      registerName(TentativeSize);
      continue;
    }

    // Assembly functions could be ST_NONE with 0 size. Check that the
    // corresponding section is a code section and they are not inside any
    // other known function to consider them.
    //
    // Sometimes assembly functions are not marked as functions and neither are
    // their local labels. The only way to tell them apart is to look at
    // symbol scope - global vs local.
    if (cantFail(Symbol.getType()) != SymbolRef::ST_Function) {
      if (PreviousFunction) {
        if (PreviousFunction->getSize() == 0) {
          if (PreviousFunction->isSymbolValidInScope(Symbol, SymbolSize)) {
            DEBUG(dbgs() << "BOLT-DEBUG: symbol is a function local symbol\n");
            registerName(SymbolSize);
            continue;
          }
        } else if (PreviousFunction->containsAddress(Address)) {
          if (PreviousFunction->isSymbolValidInScope(Symbol, SymbolSize)) {
            DEBUG(dbgs() << "BOLT-DEBUG: symbol is a function local symbol\n");
            registerName(SymbolSize);
            continue;
          } else {
            if (Address == PreviousFunction->getAddress() && SymbolSize == 0) {
              DEBUG(dbgs() << "BOLT-DEBUG: ignoring symbol as a marker\n");
              registerName(SymbolSize);
              continue;
            }
            if (opts::Verbosity > 1) {
              errs() << "BOLT-WARNING: symbol " << UniqueName
                     << " seen in the middle of function "
                     << *PreviousFunction << ". Could be a new entry.\n";
            }
            registerName(SymbolSize);
            continue;
          }
        }
      }
    }

    if (PreviousFunction &&
        PreviousFunction->containsAddress(Address) &&
        PreviousFunction->getAddress() != Address) {
      if (PreviousFunction->isSymbolValidInScope(Symbol, SymbolSize)) {
        if (opts::Verbosity >= 1) {
          outs() << "BOLT-DEBUG: possibly another entry for function "
                 << *PreviousFunction << " : " << UniqueName << '\n';
        }
      } else {
        outs() << "BOLT-INFO: using " << UniqueName << " as another entry to "
               << "function " << *PreviousFunction << '\n';

        PreviousFunction->
          addEntryPointAtOffset(Address - PreviousFunction->getAddress());

        if (!BC->HasRelocations)
          PreviousFunction->setSimple(false);

        // Remove the symbol from FileSymRefs so that we can skip it from
        // in the future.
        auto SI = FileSymRefs.find(Address);
        assert(SI != FileSymRefs.end() && "symbol expected to be present");
        assert(SI->second == Symbol && "wrong symbol found");
        FileSymRefs.erase(SI);
      }
      registerName(SymbolSize);
      continue;
    }

    // Checkout for conflicts with function data from FDEs.
    bool IsSimple = true;
    auto FDEI = CFIRdWrt->getFDEs().lower_bound(Address);
    if (FDEI != CFIRdWrt->getFDEs().end()) {
      const auto &FDE = *FDEI->second;
      if (FDEI->first != Address) {
        // There's no matching starting address in FDE. Make sure the previous
        // FDE does not contain this address.
        if (FDEI != CFIRdWrt->getFDEs().begin()) {
          --FDEI;
          auto &PrevFDE = *FDEI->second;
          auto PrevStart = PrevFDE.getInitialLocation();
          auto PrevLength = PrevFDE.getAddressRange();
          if (Address > PrevStart && Address < PrevStart + PrevLength) {
            errs() << "BOLT-ERROR: function " << UniqueName
                   << " is in conflict with FDE ["
                   << Twine::utohexstr(PrevStart) << ", "
                   << Twine::utohexstr(PrevStart + PrevLength)
                   << "). Skipping.\n";
            IsSimple = false;
          }
        }
      } else if (FDE.getAddressRange() != SymbolSize) {
        if (SymbolSize) {
          // Function addresses match but sizes differ.
          errs() << "BOLT-WARNING: sizes differ for function " << UniqueName
                 << ". FDE : " << FDE.getAddressRange()
                 << "; symbol table : " << SymbolSize << ". Using max size.\n";
        }
        SymbolSize = std::max(SymbolSize, FDE.getAddressRange());
        if (BC->getBinaryDataAtAddress(Address)) {
          BC->setBinaryDataSize(Address, SymbolSize);
        } else {
          DEBUG(dbgs() << "BOLT-DEBUG: No BD @ 0x"
                       << Twine::utohexstr(Address) << "\n");
        }
      }
      TentativeSize = SymbolSize;
    }

    BinaryFunction *BF{nullptr};
    auto BFI = BinaryFunctions.find(Address);
    if (BFI != BinaryFunctions.end()) {
      BF = &BFI->second;
      // Duplicate function name. Make sure everything matches before we add
      // an alternative name.
      if (SymbolSize != BF->getSize()) {
        if (opts::Verbosity >= 1) {
          if (SymbolSize && BF->getSize()) {
            errs() << "BOLT-WARNING: size mismatch for duplicate entries "
                   << *BF << " and " << UniqueName << '\n';
          }
          outs() << "BOLT-INFO: adjusting size of function " << *BF
                 << " old " << BF->getSize() << " new " << SymbolSize << "\n";
        }
        BF->setSize(std::max(SymbolSize, BF->getSize()));
        BC->setBinaryDataSize(Address, BF->getSize());
      }
      BF->addAlternativeName(UniqueName);
    } else {
      auto Section = BC->getSectionForAddress(Address);
      assert(Section && "section for functions must be registered.");
      BF = createBinaryFunction(UniqueName, *Section, Address,
                                SymbolSize, IsSimple);
    }
    if (!AlternativeName.empty())
      BF->addAlternativeName(AlternativeName);

    registerName(SymbolSize);
    PreviousFunction = BF;
  }

  // Process PLT section.
  if (BC->TheTriple->getArch() == Triple::x86_64)
    disassemblePLT();

  // See if we missed any functions marked by FDE.
  for (const auto &FDEI : CFIRdWrt->getFDEs()) {
    const auto Address = FDEI.first;
    const auto *FDE = FDEI.second;
    const auto *BF = getBinaryFunctionAtAddress(Address);
    if (!BF) {
      if (const auto *PartialBF = getBinaryFunctionContainingAddress(Address)) {
        errs() << "BOLT-WARNING: FDE [0x" << Twine::utohexstr(Address) << ", 0x"
               << Twine::utohexstr(Address + FDE->getAddressRange())
               << ") conflicts with function " << *PartialBF << '\n';
      } else {
        if (opts::Verbosity >= 1) {
          errs() << "BOLT-WARNING: FDE [0x" << Twine::utohexstr(Address)
                 << ", 0x" << Twine::utohexstr(Address + FDE->getAddressRange())
                 << ") has no corresponding symbol table entry\n";
        }
        auto Section = BC->getSectionForAddress(Address);
        assert(Section && "cannot get section for address from FDE");
        std::string FunctionName =
          "__BOLT_FDE_FUNCat" + Twine::utohexstr(Address).str();
        createBinaryFunction(FunctionName, *Section, Address,
                             FDE->getAddressRange(), true);
      }
    }
  }

  if (!SeenFileName && BC->DR.hasLocalsWithFileName() && !opts::AllowStripped) {
    errs() << "BOLT-ERROR: input binary does not have local file symbols "
              "but profile data includes function names with embedded file "
              "names. It appears that the input binary was stripped while a "
              "profiled binary was not. If you know what you are doing and "
              "wish to proceed, use -allow-stripped option.\n";
    exit(1);
  }

  // Now that all the functions were created - adjust their boundaries.
  adjustFunctionBoundaries();

  // Annotate functions with code/data markers in AArch64
  for (auto ISym = MarkersBegin; ISym != SortedFileSymbols.end(); ++ISym) {
    const auto &Symbol = *ISym;
    uint64_t Address =
        cantFail(Symbol.getAddress(), "cannot get symbol address");
    auto SymbolSize = ELFSymbolRef(Symbol).getSize();
    auto *BF = getBinaryFunctionContainingAddress(Address, true, true);
    if (!BF) {
      // Stray marker
      continue;
    }
    const auto EntryOffset = Address - BF->getAddress();
    if (BF->isCodeMarker(Symbol, SymbolSize)) {
      BF->markCodeAtOffset(EntryOffset);
      continue;
    }
    if (BF->isDataMarker(Symbol, SymbolSize)) {
      BF->markDataAtOffset(EntryOffset);
      BC->AddressToConstantIslandMap[Address] = BF;
      continue;
    }
    llvm_unreachable("Unknown marker");
  }

  if (!BC->HasRelocations)
    return;

  // Read all relocations now that we have binary functions mapped.
  for (const auto &Section : InputFile->sections()) {
    if (Section.relocation_begin() != Section.relocation_end()) {
      readRelocations(Section);
    }
  }
}

void RewriteInstance::disassemblePLT() {
  if (!PLTSection)
    return;

  const auto PLTAddress = PLTSection->getAddress();
  StringRef PLTContents = PLTSection->getContents();
  ArrayRef<uint8_t> PLTData(
      reinterpret_cast<const uint8_t *>(PLTContents.data()),
      PLTSection->getSize());

  // Pseudo function for the start of PLT. The table could have a matching
  // FDE that we want to match to pseudo function.
  createBinaryFunction("__BOLT_PLT_PSEUDO", *PLTSection, PLTAddress, 0, false,
                       PLTSize, PLTAlignment);
  for (uint64_t Offset = 0; Offset < PLTSection->getSize(); Offset += PLTSize) {
    uint64_t InstrSize;
    MCInst Instruction;
    const uint64_t InstrAddr = PLTAddress + Offset;
    if (!BC->DisAsm->getInstruction(Instruction,
                                    InstrSize,
                                    PLTData.slice(Offset),
                                    InstrAddr,
                                    nulls(),
                                    nulls())) {
      errs() << "BOLT-ERROR: unable to disassemble instruction in .plt "
             << "at offset 0x" << Twine::utohexstr(Offset) << '\n';
      exit(1);
    }

    if (!BC->MIB->isIndirectBranch(Instruction))
      continue;

    uint64_t TargetAddress;
    if (!BC->MIB->evaluateMemOperandTarget(Instruction,
                                           TargetAddress,
                                           InstrAddr,
                                           InstrSize)) {
      errs() << "BOLT-ERROR: error evaluating PLT instruction at offset 0x"
             << Twine::utohexstr(InstrAddr) << '\n';
      exit(1);
    }

    // To get the name we have to read a relocation against the address.
    for (const auto &Rel : RelaPLTSection->getSectionRef().relocations()) {
      if (Rel.getType() != ELF::R_X86_64_JUMP_SLOT)
        continue;
      if (Rel.getOffset() == TargetAddress) {
        const auto SymbolIter = Rel.getSymbol();
        assert(SymbolIter != InputFile->symbol_end() &&
               "non-null symbol expected");
        const auto SymbolName = cantFail((*SymbolIter).getName());
        std::string Name = SymbolName.str() + "@PLT";
        const auto PtrSize = BC->AsmInfo->getCodePointerSize();
        auto *BF = createBinaryFunction(Name,
                                        *PLTSection,
                                        InstrAddr,
                                        0,
                                        /*IsSimple=*/false,
                                        PLTSize,
                                        PLTAlignment);
        auto TargetSymbol = BC->registerNameAtAddress(SymbolName.str() + "@GOT",
                                                      TargetAddress,
                                                      PtrSize,
                                                      PLTAlignment);
        BF->setPLTSymbol(TargetSymbol);
        break;
      }
    }
  }

  if (PLTGOTSection) {
    // Check if we need to create a function for .plt.got. Some linkers
    // (depending on the version) would mark it with FDE while others wouldn't.
    if (!getBinaryFunctionAtAddress(PLTGOTSection->getAddress())) {
      createBinaryFunction("__BOLT_PLT_GOT_PSEUDO",
                           *PLTGOTSection,
                           PLTGOTSection->getAddress(),
                           0,
                           false,
                           PLTAlignment);
    }
  }
}

void RewriteInstance::adjustFunctionBoundaries() {
  for (auto BFI = BinaryFunctions.begin(), BFE = BinaryFunctions.end();
       BFI != BFE; ++BFI) {
    auto &Function = BFI->second;

    // Check if there's a symbol or a function with a larger address in the
    // same section. If there is - it determines the maximum size for the
    // current function. Otherwise, it is the size of a containing section
    // the defines it.
    //
    // NOTE: ignore some symbols that could be tolerated inside the body
    //       of a function.
    auto NextSymRefI = FileSymRefs.upper_bound(Function.getAddress());
    while (NextSymRefI != FileSymRefs.end()) {
      auto &Symbol = NextSymRefI->second;
      auto SymbolSize = ELFSymbolRef(Symbol).getSize();

      if (!Function.isSymbolValidInScope(Symbol, SymbolSize))
        break;

      // This is potentially another entry point into the function.
      auto EntryOffset = NextSymRefI->first - Function.getAddress();
      DEBUG(dbgs() << "BOLT-DEBUG: adding entry point to function " << Function
                   << " at offset 0x" << Twine::utohexstr(EntryOffset) << '\n');
      Function.addEntryPointAtOffset(EntryOffset);
      // In non-relocation mode there's potentially an external undetectable
      // reference to the entry point and hence we cannot move this entry
      // point. Optimizing without moving could be difficult.
      if (!BC->HasRelocations)
        Function.setSimple(false);

      ++NextSymRefI;
    }

    // Function runs at most till the end of the containing section.
    uint64_t NextObjectAddress = Function.getSection().getEndAddress();
    // Or till the next object marked by a symbol.
    if (NextSymRefI != FileSymRefs.end()) {
      NextObjectAddress = std::min(NextSymRefI->first, NextObjectAddress);
    }
    // Or till the next function not marked by a symbol.
    if (std::next(BFI) != BFE) {
      const auto &NextFunction = std::next(BFI)->second;
      NextObjectAddress = std::min(NextFunction.getAddress(),
                                   NextObjectAddress);
    }

    const auto MaxSize = NextObjectAddress - Function.getAddress();
    if (MaxSize < Function.getSize()) {
      errs() << "BOLT-ERROR: symbol seen in the middle of the function "
             << Function << ". Skipping.\n";
      Function.setSimple(false);
      Function.setMaxSize(Function.getSize());
      continue;
    }
    Function.setMaxSize(MaxSize);
    if (!Function.getSize() && Function.isSimple()) {
      // Some assembly functions have their size set to 0, use the max
      // size as their real size.
      if (opts::Verbosity >= 1) {
        outs() << "BOLT-INFO: setting size of function " << Function
               << " to " << Function.getMaxSize() << " (was 0)\n";
      }
      Function.setSize(Function.getMaxSize());
    }
  }
}

void RewriteInstance::relocateEHFrameSection() {
  assert(EHFrameSection && "non-empty .eh_frame section expected");

  DWARFDebugFrame EHFrame(true, EHFrameSection->getAddress());
  DWARFDataExtractor DE(EHFrameSection->getContents(),
                        BC->AsmInfo->isLittleEndian(),
                        BC->AsmInfo->getCodePointerSize());
  auto createReloc = [&](uint64_t Value, uint64_t Offset, uint64_t DwarfType) {
    if (DwarfType == dwarf::DW_EH_PE_omit)
      return;

    if (!(DwarfType & dwarf::DW_EH_PE_pcrel) &&
        !(DwarfType & dwarf::DW_EH_PE_textrel) &&
        !(DwarfType & dwarf::DW_EH_PE_funcrel) &&
        !(DwarfType & dwarf::DW_EH_PE_datarel)) {
      return;
    }

    if (!(DwarfType & dwarf::DW_EH_PE_sdata4))
      return;

    uint64_t RelType;
    switch (DwarfType & 0x0f) {
    default:
      llvm_unreachable("unsupported DWARF encoding type");
    case dwarf::DW_EH_PE_sdata4:
    case dwarf::DW_EH_PE_udata4:
      RelType = ELF::R_X86_64_PC32;
      Offset -= 4;
      break;
    case dwarf::DW_EH_PE_sdata8:
    case dwarf::DW_EH_PE_udata8:
      RelType = ELF::R_X86_64_PC64;
      Offset -= 8;
      break;
    }

    auto *BD = BC->getBinaryDataContainingAddress(Value);
    auto *Symbol = BD ? BD->getSymbol() : nullptr;
    auto Addend = BD ? Value - BD->getAddress() : 0;
    if (!Symbol) {
      DEBUG(dbgs() << "BOLT-DEBUG: creating symbol for DWARF reference at 0x"
                   << Twine::utohexstr(Value) << '\n');
      Symbol = BC->getOrCreateGlobalSymbol(Value, 0, 0, "FUNCat");
    }

    DEBUG(dbgs() << "BOLT-DEBUG: adding DWARF reference against symbol "
                 << Symbol->getName() << '\n');

    EHFrameSection->addRelocation(Offset, Symbol, RelType, Addend);
  };

  EHFrame.parse(DE, createReloc);
}

BinaryFunction *RewriteInstance::createBinaryFunction(
    const std::string &Name, BinarySection &Section, uint64_t Address,
    uint64_t Size, bool IsSimple, uint64_t SymbolSize, uint16_t Alignment) {
  auto Result = BinaryFunctions.emplace(
      Address, BinaryFunction(Name, Section, Address, Size, *BC, IsSimple));
  assert(Result.second == true && "unexpected duplicate function");
  auto *BF = &Result.first->second;
  BC->registerNameAtAddress(Name,
                            Address,
                            SymbolSize ? SymbolSize : Size,
                            Alignment);
  BC->setSymbolToFunctionMap(BF->getSymbol(), BF);
  return BF;
}

ArrayRef<uint8_t> RewriteInstance::getLSDAData() {
  return ArrayRef<uint8_t>(LSDASection->getData(),
                           LSDASection->getContents().size());
}

uint64_t RewriteInstance::getLSDAAddress() {
  return LSDASection->getAddress();
}

void RewriteInstance::readSpecialSections() {
  NamedRegionTimer T("readSpecialSections", "read special sections",
                     TimerGroupName, TimerGroupDesc, opts::TimeRewrite);

  bool HasTextRelocations = false;

  // Process special sections.
  for (const auto &Section : InputFile->sections()) {
    StringRef SectionName;
    check_error(Section.getName(SectionName), "cannot get section name");

    // Only register sections with names.
    if (!SectionName.empty()) {
      BC->registerSection(Section);
      DEBUG(dbgs() << "BOLT-DEBUG: registering section " << SectionName
                   << " @ 0x" << Twine::utohexstr(Section.getAddress()) << ":0x"
                   << Twine::utohexstr(Section.getAddress() + Section.getSize())
                   << "\n");
    }
  }

  HasTextRelocations = (bool)BC->getUniqueSectionByName(".rela.text");
  LSDASection = BC->getUniqueSectionByName(".gcc_except_table");
  EHFrameSection = BC->getUniqueSectionByName(".eh_frame");
  GdbIndexSection = BC->getUniqueSectionByName(".gdb_index");
  PLTSection = BC->getUniqueSectionByName(".plt");
  GOTPLTSection = BC->getUniqueSectionByName(".got.plt");
  PLTGOTSection = BC->getUniqueSectionByName(".plt.got");
  RelaPLTSection = BC->getUniqueSectionByName(".rela.plt");

  if (opts::PrintSections) {
    outs() << "BOLT-INFO: Sections from original binary:\n";
    BC->printSections(outs());
  }

  if (opts::PrintSections) {
    outs() << "BOLT-INFO: Sections:\n";
    BC->printSections(outs());
  }

  if (opts::RelocationMode == cl::BOU_TRUE && !HasTextRelocations) {
    errs() << "BOLT-ERROR: relocations against code are missing from the input "
              "file. Cannot proceed in relocations mode (-relocs).\n";
    exit(1);
  }

  BC->HasRelocations = HasTextRelocations &&
                       (opts::RelocationMode != cl::BOU_FALSE);
  if (BC->HasRelocations) {
    outs() << "BOLT-INFO: enabling relocation mode\n";
  }

  // Process debug sections.
  EHFrame = BC->DwCtx->getEHFrame();
  if (opts::DumpEHFrame) {
    outs() << "BOLT-INFO: Dumping original binary .eh_frame\n";
    EHFrame->dump(outs(), &*BC->MRI, NoneType());
  }
  CFIRdWrt.reset(new CFIReaderWriter(*EHFrame));
}

void RewriteInstance::adjustCommandLineOptions() {
  if (BC->isAArch64() && opts::RelocationMode != cl::BOU_TRUE &&
      !opts::AggregateOnly) {
    errs() << "BOLT-WARNING: non-relocation mode for AArch64 is not fully "
              "supported\n";
  }

  if (opts::AlignMacroOpFusion != MFT_NONE && !BC->isX86()) {
    outs() << "BOLT-INFO: disabling -align-macro-fusion on non-x86 platform\n";
    opts::AlignMacroOpFusion = MFT_NONE;
  }
  if (opts::AlignMacroOpFusion != MFT_NONE &&
      !BC->HasRelocations) {
    outs() << "BOLT-INFO: disabling -align-macro-fusion in non-relocation "
              "mode\n";
    opts::AlignMacroOpFusion = MFT_NONE;
  }
  if (BC->isX86() && BC->HasRelocations &&
      opts::AlignMacroOpFusion == MFT_HOT &&
      !DA.started() && BC->DR.getAllFuncsData().empty() &&
      opts::BoltProfile.empty()) {
    outs() << "BOLT-INFO: enabling -align-macro-fusion=all since no profile "
              "was specified\n";
    opts::AlignMacroOpFusion = MFT_ALL;
  }
}

namespace {
template <typename ELFT>
int64_t getRelocationAddend(const ELFObjectFile<ELFT> *Obj,
                            const RelocationRef &RelRef) {
  int64_t Addend = 0;
  const ELFFile<ELFT> &EF = *Obj->getELFFile();
  DataRefImpl Rel = RelRef.getRawDataRefImpl();
  const auto *RelocationSection = cantFail(EF.getSection(Rel.d.a));
  switch (RelocationSection->sh_type) {
  default: llvm_unreachable("unexpected relocation section type");
  case ELF::SHT_REL:
    break;
  case ELF::SHT_RELA: {
    const auto *RelA = Obj->getRela(Rel);
    Addend = RelA->r_addend;
    break;
  }
  }

  return Addend;
}

int64_t getRelocationAddend(const ELFObjectFileBase *Obj,
                         const RelocationRef &Rel) {
  if (auto *ELF32LE = dyn_cast<ELF32LEObjectFile>(Obj))
    return getRelocationAddend(ELF32LE, Rel);
  if (auto *ELF64LE = dyn_cast<ELF64LEObjectFile>(Obj))
    return getRelocationAddend(ELF64LE, Rel);
  if (auto *ELF32BE = dyn_cast<ELF32BEObjectFile>(Obj))
    return getRelocationAddend(ELF32BE, Rel);
  auto *ELF64BE = cast<ELF64BEObjectFile>(Obj);
  return getRelocationAddend(ELF64BE, Rel);
}
} // anonymous namespace

bool RewriteInstance::analyzeRelocation(const RelocationRef &Rel,
                                        SectionRef RelocatedSection,
                                        std::string &SymbolName,
                                        uint64_t &SymbolAddress,
                                        int64_t &Addend,
                                        uint64_t &ExtractedValue) const {
  if (!Relocation::isSupported(Rel.getType()))
    return false;

  const bool IsAArch64 = BC->isAArch64();
  const bool IsFromCode = RelocatedSection.isText();

  // For value extraction.
  StringRef RelocatedSectionContents;
  RelocatedSection.getContents(RelocatedSectionContents);
  DataExtractor DE(RelocatedSectionContents,
                   BC->AsmInfo->isLittleEndian(),
                   BC->AsmInfo->getCodePointerSize());

  const bool IsPCRelative = Relocation::isPCRelative(Rel.getType());
  auto SymbolIter = Rel.getSymbol();
  assert(SymbolIter != InputFile->symbol_end() &&
         "relocation symbol must exist");
  auto Symbol = *SymbolIter;
  SymbolName = cantFail(Symbol.getName());
  SymbolAddress = cantFail(Symbol.getAddress());
  Addend = getRelocationAddend(InputFile, Rel);

  uint32_t RelocationOffset =
    Rel.getOffset() - RelocatedSection.getAddress();
  const auto RelSize = Relocation::getSizeForType(Rel.getType());
  ExtractedValue =
    static_cast<uint64_t>(DE.getSigned(&RelocationOffset, RelSize));

  if (IsAArch64) {
    ExtractedValue = Relocation::extractValue(Rel.getType(),
                                              ExtractedValue,
                                              Rel.getOffset());
  }

  // Section symbols are marked as ST_Debug.
  const bool SymbolIsSection =
      (cantFail(Symbol.getType()) == SymbolRef::ST_Debug);
  const auto PCRelOffset = IsPCRelative && !IsAArch64 ? Rel.getOffset() : 0;

  // If no symbol has been found or if it is a relocation requiring the
  // creation of a GOT entry, do not link against the symbol but against
  // whatever address was extracted from the instruction itself. We are
  // not creating a GOT entry as this was already processed by the linker.
  if (!SymbolAddress || Relocation::isGOT(Rel.getType())) {
    assert(!SymbolIsSection);
    if (ExtractedValue) {
      SymbolAddress = ExtractedValue - Addend + PCRelOffset;
    } else {
      // This is weird case.  The extracted value is zero but the addend is
      // non-zero and the relocation is not pc-rel.  Using the previous logic,
      // the SymbolAddress would end up as a huge number.  Seen in
      // exceptions_pic.test.
      DEBUG(dbgs() << "BOLT-DEBUG: relocation @ 0x"
                   << Twine::utohexstr(Rel.getOffset())
                   << " value does not match addend for "
                   << "relocation to undefined symbol.\n");
      SymbolAddress += PCRelOffset;
      return true;
    }
  } else if (SymbolIsSection) {
    auto Section = Symbol.getSection();
    if (Section && *Section != InputFile->section_end()) {
      SymbolName = "section " + std::string(getSectionName(**Section));
      if (!IsAArch64) {
        assert(SymbolAddress == (*Section)->getAddress() &&
               "section symbol address must be the same as section address");
        // Convert section symbol relocations to regular relocations inside
        // non-section symbols.
        if (IsPCRelative) {
          Addend = ExtractedValue - (SymbolAddress - PCRelOffset);
        } else {
          SymbolAddress = ExtractedValue;
          Addend = 0;
        }
      }
    }
  }

  if (!IsPCRelative && Addend != 0 && IsFromCode && !SymbolIsSection) {
    // TODO: RefSection should be the same as **(Symbol.getSection()).
    auto RefSection = BC->getSectionForAddress(SymbolAddress);
    if (RefSection && RefSection->isText()) {
      if (opts::Verbosity > 1) {
        SmallString<16> TypeName;
        Rel.getTypeName(TypeName);
        errs() << "BOLT-WARNING: detected absolute reference from code into "
               << "a middle of a function:\n"
               << " offset = 0x" << Twine::utohexstr(Rel.getOffset())
               << "; type = " << Rel.getType()
               << "; type name = " << TypeName
               << "; value = 0x" << Twine::utohexstr(ExtractedValue)
               << "; symbol = " << SymbolName
               << "; symbol address = 0x" << Twine::utohexstr(SymbolAddress)
               << "; symbol section = " << RefSection->getName()
               << "; addend = 0x" << Twine::utohexstr(Addend)
               << "; address = 0x" << Twine::utohexstr(SymbolAddress + Addend)
               << '\n';
      }
      assert(truncateToSize(ExtractedValue, RelSize) ==
               truncateToSize(SymbolAddress + Addend, RelSize) &&
             "value mismatch");
    }
  }

  DEBUG(
    if (!Relocation::isTLS(Rel.getType()) &&
        SymbolName != "__hot_start" &&
        SymbolName != "__hot_end" &&
        truncateToSize(ExtractedValue, RelSize) !=
          truncateToSize(SymbolAddress + Addend - PCRelOffset, RelSize)) {
      auto Section = cantFail(Symbol.getSection());
      SmallString<16> TypeName;
      Rel.getTypeName(TypeName);
      dbgs() << "BOLT-DEBUG: Mismatch between extracted value and relocation "
             << "data:\n"
             << "BOLT-DEBUG: offset = 0x"
             << Twine::utohexstr(Rel.getOffset())
             << "; type = " << Rel.getType()
             << "; type name = " << TypeName
             << "; value = 0x" << Twine::utohexstr(ExtractedValue)
             << "; symbol = " << SymbolName
             << "; symbol type = " << cantFail(Symbol.getType())
             << "; symbol address = 0x" << Twine::utohexstr(SymbolAddress)
             << "; orig symbol address = 0x"
             << Twine::utohexstr(cantFail(Symbol.getAddress()))
             << "; symbol section = " << getSectionName(*Section)
             << "; addend = 0x" << Twine::utohexstr(Addend)
             << "; original addend = 0x"
             << Twine::utohexstr(getRelocationAddend(InputFile, Rel))
             << '\n';
    });

  assert((IsAArch64 ||
          Relocation::isTLS(Rel.getType()) ||
          SymbolName == "__hot_start" ||
          SymbolName == "__hot_end" ||
          truncateToSize(ExtractedValue, RelSize) ==
            truncateToSize(SymbolAddress + Addend - PCRelOffset, RelSize)) &&
         "extracted relocation value should match relocation components");

  return true;
}

void RewriteInstance::readRelocations(const SectionRef &Section) {
  StringRef SectionName;
  Section.getName(SectionName);
  DEBUG(dbgs() << "BOLT-DEBUG: relocations for section "
               << SectionName << ":\n");
  if (ELFSectionRef(Section).getFlags() & ELF::SHF_ALLOC) {
    DEBUG(dbgs() << "BOLT-DEBUG: ignoring runtime relocations\n");
    return;
  }
  auto SecIter = Section.getRelocatedSection();
  assert(SecIter != InputFile->section_end() && "relocated section expected");
  auto RelocatedSection = *SecIter;

  StringRef RelocatedSectionName;
  RelocatedSection.getName(RelocatedSectionName);
  DEBUG(dbgs() << "BOLT-DEBUG: relocated section is "
               << RelocatedSectionName << '\n');

  if (!(ELFSectionRef(RelocatedSection).getFlags() & ELF::SHF_ALLOC)) {
    DEBUG(dbgs() << "BOLT-DEBUG: ignoring relocations against "
                 << "non-allocatable section\n");
    return;
  }
  const bool SkipRelocs = StringSwitch<bool>(RelocatedSectionName)
    .Cases(".plt", ".rela.plt", ".got.plt", ".eh_frame", true)
    .Default(false);
  if (SkipRelocs) {
    DEBUG(dbgs() << "BOLT-DEBUG: ignoring relocations against known section\n");
    return;
  }

  const bool IsAArch64 = BC->isAArch64();
  const bool IsFromCode = RelocatedSection.isText();

  auto printRelocationInfo = [&](const RelocationRef &Rel,
                                 StringRef SymbolName,
                                 uint64_t SymbolAddress,
                                 uint64_t Addend,
                                 uint64_t ExtractedValue) {
    SmallString<16> TypeName;
    Rel.getTypeName(TypeName);
    const auto Address = SymbolAddress + Addend;
    auto Section = BC->getSectionForAddress(SymbolAddress);
    dbgs() << "Relocation: offset = 0x"
           << Twine::utohexstr(Rel.getOffset())
           << "; type = " << Rel.getType()
           << "; type name = " << TypeName
           << "; value = 0x" << Twine::utohexstr(ExtractedValue)
           << "; symbol = " << SymbolName
           << " (" << (Section ? Section->getName() : "") << ")"
           << "; symbol address = 0x" << Twine::utohexstr(SymbolAddress)
           << "; addend = 0x" << Twine::utohexstr(Addend)
           << "; address = 0x" << Twine::utohexstr(Address)
           << "; in = ";
    if (auto *Func = getBinaryFunctionContainingAddress(Rel.getOffset(),
                                                        false,
                                                        IsAArch64)) {
      dbgs() << Func->getPrintName() << "\n";
    } else {
      dbgs() << BC->getSectionForAddress(Rel.getOffset())->getName() << "\n";
    }
  };

  for (const auto &Rel : Section.relocations()) {
    SmallString<16> TypeName;
    Rel.getTypeName(TypeName);

    std::string SymbolName;
    uint64_t SymbolAddress;
    int64_t Addend;
    uint64_t ExtractedValue;

    if (!analyzeRelocation(Rel,
                           RelocatedSection,
                           SymbolName,
                           SymbolAddress,
                           Addend,
                           ExtractedValue)) {
      DEBUG(dbgs() << "BOLT-DEBUG: skipping relocation @ offset = 0x"
                 << Twine::utohexstr(Rel.getOffset())
                 << "; type name = " << TypeName
                 << '\n');
      continue;
    }

    const auto Address = SymbolAddress + Addend;
    const bool ForceRelocation =
      (opts::HotText && (SymbolName == "__hot_start" ||
                         SymbolName == "__hot_end")) ||
      (opts::HotData && (SymbolName == "__hot_data_start" ||
                         SymbolName == "__hot_data_end")) ||
      SymbolName == "_end" ||
      Rel.getType() == ELF::R_AARCH64_ADR_GOT_PAGE;

    DEBUG(
       dbgs() << "BOLT-DEBUG: ";
       printRelocationInfo(Rel,
                           SymbolName,
                           SymbolAddress,
                           Addend,
                           ExtractedValue));

    BinaryFunction *ContainingBF = nullptr;
    if (IsFromCode) {
      ContainingBF =
        getBinaryFunctionContainingAddress(Rel.getOffset(),
                                           /*CheckPastEnd*/ false,
                                           /*UseMaxSize*/ IsAArch64);
      assert(ContainingBF && "cannot find function for address in code");
    }

    // PC-relative relocations from data to code are tricky since the original
    // information is typically lost after linking even with '--emit-relocs'.
    // They are normally used by PIC-style jump tables and reference both
    // the jump table and jump destination by computing the difference
    // between the two. If we blindly apply the relocation it will appear
    // that it references an arbitrary location in the code, possibly even
    // in a different function from that containing the jump table.
    if (!IsAArch64 && Relocation::isPCRelative(Rel.getType())) {
      // Just register the fact that we have PC-relative relocation at a given
      // address. The actual referenced label/address cannot be determined
      // from linker data alone.
      if (IsFromCode) {
        ContainingBF->addPCRelativeRelocationAddress(Rel.getOffset());
      }
      DEBUG(dbgs() << "BOLT-DEBUG: not creating PC-relative relocation at 0x"
                   << Twine::utohexstr(Rel.getOffset()) << " for " << SymbolName
                   << "\n");
      continue;
    }

    // TODO: RefSection should be the same as **Rel.getSymbol().getSection()
    auto RefSection = BC->getSectionForAddress(SymbolAddress);
    if (!RefSection && !ForceRelocation) {
      DEBUG(dbgs() << "BOLT-DEBUG: cannot determine referenced section.\n");
      continue;
    }

    const bool IsToCode = RefSection && RefSection->isText();
    const bool IsSectionRelocation =
      (cantFail(Rel.getSymbol()->getType()) == SymbolRef::ST_Debug);

    // Occasionally we may see a reference past the last byte of the function
    // typically as a result of __builtin_unreachable(). Check it here.
    auto *ReferencedBF = getBinaryFunctionContainingAddress(
        Address, /*CheckPastEnd*/ true, /*UseMaxSize*/ IsAArch64);

    if (!IsSectionRelocation) {
      if (auto *BF = getBinaryFunctionContainingAddress(SymbolAddress)) {
        if (BF != ReferencedBF) {
          // It's possible we are referencing a function without referencing any
          // code, e.g. when taking a bitmask action on a function address.
          errs() << "BOLT-WARNING: non-standard function reference (e.g. "
                    "bitmask) detected against function " << *BF;
          if (IsFromCode) {
            errs() << " from function " << *ContainingBF << '\n';
          } else {
            errs() << " from data section at 0x"
                   << Twine::utohexstr(Rel.getOffset()) << '\n';
          }
          DEBUG(printRelocationInfo(Rel,
                                    SymbolName,
                                    SymbolAddress,
                                    Addend,
                                    ExtractedValue)
          );
          ReferencedBF = BF;
        }
      }
    }

    uint64_t RefFunctionOffset = 0;
    MCSymbol *ReferencedSymbol = nullptr;
    if (ForceRelocation) {
      auto Name = Relocation::isGOT(Rel.getType()) ? "Zero" : SymbolName;
      ReferencedSymbol = BC->registerNameAtAddress(Name, 0, 0, 0);
      SymbolAddress = 0;
      Addend = Address;
      DEBUG(dbgs() << "BOLT-DEBUG: creating relocations for huge pages against"
                      " symbol " << SymbolName << " with addend " << Addend
                   << '\n');
    } else if (ReferencedBF) {
      ReferencedSymbol = ReferencedBF->getSymbol();

      // Adjust the point of reference to a code location inside a function.
      if (ReferencedBF->containsAddress(Address, /*UseMaxSize = */true)) {
        RefFunctionOffset = Address - ReferencedBF->getAddress();
        if (RefFunctionOffset) {
          ReferencedSymbol =
            ReferencedBF->getOrCreateLocalLabel(Address,
                                                /*CreatePastEnd =*/ true);
        }
        SymbolAddress = Address;
        Addend = 0;
      }
      DEBUG(
        dbgs() << "  referenced function " << *ReferencedBF;
        if (Address != ReferencedBF->getAddress())
          dbgs() << " at offset 0x" << Twine::utohexstr(RefFunctionOffset);
        dbgs() << '\n'
      );
    } else {
      if (RefSection && RefSection->isText() && SymbolAddress) {
        // This can happen e.g. with PIC-style jump tables.
        DEBUG(dbgs() << "BOLT-DEBUG: no corresponding function for "
                        "relocation against code\n");
      }

      // In AArch64 there are zero reasons to keep a reference to the
      // "original" symbol plus addend. The original symbol is probably just a
      // section symbol. If we are here, this means we are probably accessing
      // data, so it is imperative to keep the original address.
      if (IsAArch64) {
        SymbolName = ("SYMBOLat0x" + Twine::utohexstr(Address)).str();
        SymbolAddress = Address;
        Addend = 0;
      }

      // This function makes sure that symbols referenced by ambiguous
      // relocations are marked as unmoveable.  For now, if a section
      // relocation points at the boundary between two symbols then
      // those symbols are marked as unmoveable.
      auto markAmbiguousRelocations = [&](BinaryData *BD) {
        if (Address == BD->getAddress()) {
          BD = BD->getAtomicRoot();
          DEBUG(if (BD->isMoveable()) {
            dbgs() << "BOLT-DEBUG: setting " << *BD << " as unmoveable "
                   << "due to ambiguous relocation (0x"
                   << Twine::utohexstr(Address) << ") @ 0x"
                   << Twine::utohexstr(Rel.getOffset()) << "\n";
          });
          BD->setIsMoveable(false);

          // set previous symbol as unmoveable
          auto *Prev = BC->getBinaryDataContainingAddress(Address-1);
          if (Prev && Prev->getEndAddress() == BD->getAddress()) {
            Prev = Prev->getAtomicRoot();
            DEBUG(if (Prev->isMoveable()) {
              dbgs() << "BOLT-DEBUG: setting " << *Prev << " as unmoveable "
                     << "due to ambiguous relocation (0x"
                     << Twine::utohexstr(Address) << ") @ 0x"
                     << Twine::utohexstr(Rel.getOffset()) << "\n";
            });
            Prev->setIsMoveable(false);
          }
        }

        if (Address == BD->getEndAddress()) {
          BD = BD->getAtomicRoot();
          DEBUG(if (BD->isMoveable()) {
            dbgs() << "BOLT-DEBUG: setting " << *BD << " as unmoveable "
                   << "due to ambiguous relocation (0x"
                   << Twine::utohexstr(Address) << ") @ 0x"
                   << Twine::utohexstr(Rel.getOffset()) << "\n";
          });
          BD->setIsMoveable(false);

          // set next symbol as unmoveable
          auto *Next = BC->getBinaryDataContainingAddress(BD->getEndAddress());
          if (Next && Next->getAddress() == BD->getEndAddress()) {
            Next = Next->getAtomicRoot();
            DEBUG(if (Next->isMoveable()) {
              dbgs() << "BOLT-DEBUG: setting " << *Next << " as unmoveable "
                     << "due to ambiguous relocation (0x"
                     << Twine::utohexstr(Address) << ") @ 0x"
                     << Twine::utohexstr(Rel.getOffset()) << "\n";
            });
            Next->setIsMoveable(false);
          }
        }
      };

      // If we are allowing section relocations, we assign relocations
      // that are pointing to the end of a symbol to that symbol rather
      // than the following symbol.
      const auto IncludeEnd =
        opts::AllowSectionRelocations && IsSectionRelocation;

      if (auto *BD = BC->getBinaryDataContainingAddress(SymbolAddress,
                                                        IncludeEnd)) {
        assert(!IncludeEnd ||
               (BD == BC->getBinaryDataContainingAddress(SymbolAddress) ||
                !BC->getBinaryDataContainingAddress(SymbolAddress) ||
                (IsSectionRelocation && BD->getEndAddress() ==
                 BC->getBinaryDataContainingAddress(SymbolAddress)->
                    getAddress())));

        // Note: this assertion is trying to check sanity of BinaryData objects
        // but AArch64 has inferred and incomplete object locations coming from
        // GOT/TLS or any other non-trivial relocation (that requires creation
        // of sections and whose symbol address is not really what should be
        // encoded in the instruction). So we essentially disabled this check
        // for AArch64 and live with bogus names for objects.
        assert((IsAArch64 ||
                IsSectionRelocation ||
                BD->nameStartsWith(SymbolName) ||
                BD->nameStartsWith("PG" + SymbolName) ||
                (BD->nameStartsWith("ANONYMOUS") &&
                 (BD->getSectionName().startswith(".plt") ||
                  BD->getSectionName().endswith(".plt")))) &&
               "BOLT symbol names of all non-section relocations must match "
               "up with symbol names referenced in the relocation");

        if (!opts::AllowSectionRelocations && IsSectionRelocation) {
          markAmbiguousRelocations(BD);
        }

        ReferencedSymbol = BD->getSymbol();
        Addend += (SymbolAddress - BD->getAddress());
        SymbolAddress = BD->getAddress();
        assert(Address == SymbolAddress + Addend);
      } else {
        auto Symbol = *Rel.getSymbol();
        // These are mostly local data symbols but undefined symbols
        // in relocation sections can get through here too, from .plt.
        assert((IsAArch64 ||
                IsSectionRelocation ||
                BC->getSectionNameForAddress(SymbolAddress)->startswith(".plt"))
               && "known symbols should not resolve to anonymous locals");

        const uint64_t SymbolSize =
            IsAArch64 ? 0 : ELFSymbolRef(Symbol).getSize();
        const uint64_t SymbolAlignment = IsAArch64 ? 1 : Symbol.getAlignment();
        const unsigned SymbolFlags = Symbol.getFlags();

        if (!IsSectionRelocation) {
          std::string Name;
          if (Symbol.getFlags() & SymbolRef::SF_Global) {
            Name = SymbolName;
          } else {
            Name = uniquifyName(*BC, StringRef(SymbolName).startswith(
                                         BC->AsmInfo->getPrivateGlobalPrefix())
                                         ? "PG" + SymbolName + "/"
                                         : SymbolName + "/");
          }
          ReferencedSymbol = BC->registerNameAtAddress(Name,
                                                       SymbolAddress,
                                                       SymbolSize,
                                                       SymbolAlignment,
                                                       SymbolFlags);
        } else {
          ReferencedSymbol = BC->getOrCreateGlobalSymbol(SymbolAddress,
                                                         SymbolSize,
                                                         SymbolAlignment,
                                                         "SYMBOLat",
                                                         SymbolFlags);
        }

        if (!opts::AllowSectionRelocations && IsSectionRelocation) {
          auto *BD = BC->getBinaryDataByName(ReferencedSymbol->getName());
          markAmbiguousRelocations(BD);
        }
      }
    }

    auto checkMaxDataRelocations = [&]() {
      ++NumDataRelocations;
      if (opts::MaxDataRelocations &&
          NumDataRelocations + 1 == opts::MaxDataRelocations) {
          dbgs() << "BOLT-DEBUG: processing ending on data relocation "
                 << NumDataRelocations << ": ";
          printRelocationInfo(Rel,
                              ReferencedSymbol->getName(),
                              SymbolAddress,
                              Addend,
                              ExtractedValue);
      }

      return (!opts::MaxDataRelocations ||
              NumDataRelocations < opts::MaxDataRelocations);
    };

    if (IsFromCode) {
      if (ReferencedBF || ForceRelocation || IsAArch64 ||
          refersToReorderedSection(RefSection) ||
          (opts::ForceToDataRelocations && checkMaxDataRelocations())) {
        ContainingBF->addRelocation(Rel.getOffset(),
                                    ReferencedSymbol,
                                    Rel.getType(),
                                    Addend,
                                    ExtractedValue);
      } else {
        DEBUG(dbgs() << "BOLT-DEBUG: ignoring relocation from code to data "
                     << ReferencedSymbol->getName() << "\n");
      }
    } else if (IsToCode) {
      BC->addRelocation(Rel.getOffset(), ReferencedSymbol, Rel.getType(),
                        Addend);
    } else if (refersToReorderedSection(RefSection) ||
               (opts::ForceToDataRelocations && checkMaxDataRelocations())) {
      BC->addRelocation(Rel.getOffset(),
                        ReferencedSymbol,
                        Rel.getType(),
                        Addend);
    } else {
      DEBUG(dbgs() << "BOLT-DEBUG: ignoring relocation from data to data\n");
    }
  }
}

void RewriteInstance::readDebugInfo() {
  NamedRegionTimer T("readDebugInfo", "read debug info", TimerGroupName,
                     TimerGroupDesc, opts::TimeRewrite);
  if (!opts::UpdateDebugSections)
    return;

  BC->preprocessDebugInfo(BinaryFunctions);
}

void RewriteInstance::processProfileData() {
  if (DA.started()) {
    NamedRegionTimer T("aggregate", "aggregate data", TimerGroupName,
                       TimerGroupDesc, opts::TimeRewrite);
    DA.aggregate(*BC.get(), BinaryFunctions);

    for (auto &BFI : BinaryFunctions) {
      auto &Function = BFI.second;
      Function.convertBranchData();
    }

    if (opts::AggregateOnly) {
      if (std::error_code EC = DA.writeAggregatedFile()) {
        check_error(EC, "cannot create output data file");
      }
    }
  } else {
    NamedRegionTimer T("readprofile", "read profile data", TimerGroupName,
                       TimerGroupDesc, opts::TimeRewrite);

    if (!opts::BoltProfile.empty()) {
      ProfileReader PR;
      auto EC = PR.readProfile(opts::BoltProfile, BinaryFunctions);
      check_error(EC, "cannot read profile");

      return;
    }

    // Preliminary match profile data to functions.
    if (!BC->DR.getAllFuncsData().empty()) {
      for (auto &BFI : BinaryFunctions) {
        auto &Function = BFI.second;
        if (auto *MemData = BC->DR.getFuncMemData(Function.getNames())) {
          Function.MemData = MemData;
          MemData->Used = true;
        }
        if (auto *FuncData = BC->DR.getFuncBranchData(Function.getNames())) {
          Function.BranchData = FuncData;
          Function.ExecutionCount = FuncData->ExecutionCount;
          FuncData->Used = true;
        }
      }
    }

    for (auto &BFI : BinaryFunctions) {
      auto &Function = BFI.second;
      Function.readProfile();
    }
  }

  if (!opts::SaveProfile.empty()) {
    ProfileWriter PW(opts::SaveProfile);
    PW.writeProfile(*this);
  }
}

void RewriteInstance::disassembleFunctions() {
  NamedRegionTimer T("disassembleFunctions", "disassemble functions",
                     TimerGroupName, TimerGroupDesc, opts::TimeRewrite);
  for (auto &BFI : BinaryFunctions) {
    BinaryFunction &Function = BFI.second;

    // If we have to relocate the code we have to disassemble all functions.
    if (!BC->HasRelocations && !opts::shouldProcess(Function)) {
      DEBUG(dbgs() << "BOLT: skipping processing function "
                   << Function << " per user request.\n");
      continue;
    }

    auto FunctionData = BC->getFunctionData(Function);
    if (!FunctionData) {
      // When could it happen?
      errs() << "BOLT-ERROR: corresponding section is non-executable or "
             << "empty for function " << Function << '\n';
      continue;
    }

    // Treat zero-sized functions as non-simple ones.
    if (Function.getSize() == 0) {
      Function.setSimple(false);
      continue;
    }

    // Offset of the function in the file.
    const auto *FileBegin =
      reinterpret_cast<const uint8_t*>(InputFile->getData().data());
    Function.setFileOffset(FunctionData->begin() - FileBegin);

    Function.disassemble(*FunctionData);

    if (!Function.isSimple() && BC->HasRelocations) {
      BC->exitWithBugReport("function cannot be properly disassembled. "
                            "Unable to continue in relocation mode.",
                            Function);
    }

    if (opts::PrintAll || opts::PrintDisasm)
      Function.print(outs(), "after disassembly", true);

    // Post-process inter-procedural references ASAP as it may affect
    // functions we are about to disassemble next.
    for (const auto Addr : BC->InterproceduralReferences) {
      auto *ContainingFunction = getBinaryFunctionContainingAddress(Addr);
      if (ContainingFunction && ContainingFunction->getAddress() != Addr) {
        ContainingFunction->addEntryPoint(Addr);
        if (!BC->HasRelocations) {
          if (opts::Verbosity >= 1) {
            errs() << "BOLT-WARNING: Function " << *ContainingFunction
                   << " has internal BBs that are target of a reference located"
                   << " in another function. Skipping the function.\n";
          }
          ContainingFunction->setSimple(false);
        }
      } else if (!ContainingFunction && Addr) {
        // Check if address falls in function padding space - this could be
        // unmarked data in code. In this case adjust the padding space size.
        auto Section = BC->getSectionForAddress(Addr);
        assert(Section && "cannot get section for referenced address");

        if (!Section->isText())
          continue;

        // PLT requires special handling and could be ignored in this context.
        StringRef SectionName = Section->getName();
        if (SectionName == ".plt" || SectionName == ".plt.got")
          continue;

        if (BC->HasRelocations) {
          errs() << "BOLT-ERROR: cannot process binaries with unmarked "
                 << "object in code at address 0x"
                 << Twine::utohexstr(Addr) << " belonging to section "
                 << SectionName << " in relocation mode.\n";
          exit(1);
        }

        ContainingFunction =
          getBinaryFunctionContainingAddress(Addr,
                                             /*CheckPastEnd=*/false,
                                             /*UseMaxSize=*/true);
        // We are not going to overwrite non-simple functions, but for simple
        // ones - adjust the padding size.
        if (ContainingFunction && ContainingFunction->isSimple()) {
          errs() << "BOLT-WARNING: function " << *ContainingFunction
                 << " has an object detected in a padding region at address 0x"
                 << Twine::utohexstr(Addr) << '\n';
          ContainingFunction->setMaxSize(Addr -
                                         ContainingFunction->getAddress());
        }
      }
    }
    BC->InterproceduralReferences.clear();
  }

  for (auto &BFI : BinaryFunctions) {
    BinaryFunction &Function = BFI.second;

    if (!BC->HasRelocations && !opts::shouldProcess(Function)) {
      DEBUG(dbgs() << "BOLT: skipping processing function "
                   << Function << " per user request.\n");
      continue;
    }

    if (!Function.isSimple()) {
      assert((!BC->HasRelocations || Function.getSize() == 0) &&
             "unexpected non-simple function in relocation mode");
      continue;
    }

    // Fill in CFI information for this function
    if (!Function.trapsOnEntry()) {
      if (!CFIRdWrt->fillCFIInfoFor(Function)) {
        if (BC->HasRelocations) {
          BC->exitWithBugReport("unable to fill CFI.", Function);
        } else {
          errs() << "BOLT-WARNING: unable to fill CFI for function "
                 << Function << ". Skipping.\n";
          Function.setSimple(false);
          continue;
        }
      }
    }

    // Parse LSDA.
    if (Function.getLSDAAddress() != 0)
      Function.parseLSDA(getLSDAData(), getLSDAAddress());

    if (!Function.buildCFG())
      continue;

    if (opts::PrintAll)
      Function.print(outs(), "while building cfg", true);

  } // Iterate over all functions

  BC->postProcessSymbolTable();
}

void RewriteInstance::postProcessFunctions() {
  BC->TotalScore = 0;
  BC->SumExecutionCount = 0;
  for (auto &BFI : BinaryFunctions) {
    BinaryFunction &Function = BFI.second;

    if (Function.empty())
      continue;

    Function.postProcessCFG();

    if (opts::PrintAll || opts::PrintCFG)
      Function.print(outs(), "after building cfg", true);

    if (opts::DumpDotAll)
      Function.dumpGraphForPass("build-cfg");

    if (opts::PrintLoopInfo) {
      Function.calculateLoopInfo();
      Function.printLoopInfo(outs());
    }

    BC->TotalScore += Function.getFunctionScore();
    BC->SumExecutionCount += Function.getKnownExecutionCount();
  }

  if (opts::PrintGlobals) {
    outs() << "BOLT-INFO: Global symbols:\n";
    BC->printGlobalSymbols(outs());
  }
}

void RewriteInstance::runOptimizationPasses() {
  NamedRegionTimer T("runOptimizationPasses", "run optimization passes",
                     TimerGroupName, TimerGroupDesc, opts::TimeRewrite);
  BinaryFunctionPassManager::runAllPasses(*BC, BinaryFunctions, LargeFunctions);
}

// Helper function to emit the contents of a function via a MCStreamer object.
void RewriteInstance::emitFunction(MCStreamer &Streamer,
                                   BinaryFunction &Function,
                                   bool EmitColdPart) {
  if (Function.getSize() == 0)
    return;

  if (Function.getState() == BinaryFunction::State::Empty)
    return;

  MCSection *Section;
  if (BC->HasRelocations) {
    Section = BC->MOFI->getTextSection();
  } else {
    // Each fuction is emmitted into its own section.
    Section =
        BC->Ctx->getELFSection(EmitColdPart ? Function.getColdCodeSectionName()
                                            : Function.getCodeSectionName(),
                              ELF::SHT_PROGBITS,
                              ELF::SHF_EXECINSTR | ELF::SHF_ALLOC);
  }

  Section->setHasInstructions(true);

  BC->Ctx->addGenDwarfSection(Section);

  Streamer.SwitchSection(Section);

  if (BC->HasRelocations) {
    Streamer.EmitCodeAlignment(BinaryFunction::MinAlign);
    auto MaxAlignBytes = EmitColdPart
      ? Function.getMaxColdAlignmentBytes()
      : Function.getMaxAlignmentBytes();
    if (MaxAlignBytes > 0)
      Streamer.EmitCodeAlignment(Function.getAlignment(), MaxAlignBytes);
  } else {
    Streamer.EmitCodeAlignment(Function.getAlignment());
  }

  MCContext &Context = Streamer.getContext();
  const MCAsmInfo *MAI = Context.getAsmInfo();

  // Emit all names the function is known under.
  for (const auto &Name : Function.getNames()) {
    Twine EmitName = EmitColdPart ? Twine(Name).concat(".cold") : Name;
    auto *EmitSymbol = BC->Ctx->getOrCreateSymbol(EmitName);
    Streamer.EmitSymbolAttribute(EmitSymbol, MCSA_ELF_TypeFunction);
    DEBUG(dbgs() << "emitting symbol " << EmitSymbol->getName()
                 << " for function " << Function << '\n');
    Streamer.EmitLabel(EmitSymbol);
  }

  // Emit CFI start
  if (Function.hasCFI() && (BC->HasRelocations || Function.isSimple())) {
    Streamer.EmitCFIStartProc(/*IsSimple=*/false);
    if (Function.getPersonalityFunction() != nullptr) {
      Streamer.EmitCFIPersonality(Function.getPersonalityFunction(),
                                  Function.getPersonalityEncoding());
    }
    auto *LSDASymbol = EmitColdPart ? Function.getColdLSDASymbol()
                                    : Function.getLSDASymbol();
    if (LSDASymbol) {
      Streamer.EmitCFILsda(LSDASymbol, BC->MOFI->getLSDAEncoding());
    } else {
      Streamer.EmitCFILsda(0, dwarf::DW_EH_PE_omit);
    }
    // Emit CFI instructions relative to the CIE
    for (const auto &CFIInstr : Function.cie()) {
      // Only write CIE CFI insns that LLVM will not already emit
      const std::vector<MCCFIInstruction> &FrameInstrs =
          MAI->getInitialFrameState();
      if (std::find(FrameInstrs.begin(), FrameInstrs.end(), CFIInstr) ==
          FrameInstrs.end())
        Streamer.EmitCFIInstruction(CFIInstr);
    }
  }

  assert((Function.empty() || !(*Function.begin()).isCold()) &&
         "first basic block should never be cold");

  // Emit UD2 at the beginning if requested by user.
  if (!opts::BreakFunctionNames.empty()) {
    for (auto &Name : opts::BreakFunctionNames) {
      if (Function.hasName(Name)) {
        Streamer.EmitIntValue(0x0B0F, 2); // UD2: 0F 0B
        break;
      }
    }
  }

  // Emit code.
  Function.emitBody(Streamer, EmitColdPart);

  // Emit padding if requested.
  if (auto Padding = opts::padFunction(Function)) {
    DEBUG(dbgs() << "BOLT-DEBUG: padding function " << Function << " with "
                 << Padding << " bytes\n");
    Streamer.emitFill(Padding, MAI->getTextAlignFillValue());
  }

  if (opts::MarkFuncs) {
    Streamer.EmitIntValue(MAI->getTrapFillValue(), 1);
  }

  // Emit CFI end
  if (Function.hasCFI() && (BC->HasRelocations || Function.isSimple()))
    Streamer.EmitCFIEndProc();

  Streamer.EmitLabel(EmitColdPart ? Function.getFunctionColdEndLabel()
                                  : Function.getFunctionEndLabel());

  // Exception handling info for the function.
  Function.emitLSDA(&Streamer, EmitColdPart);

  if (!EmitColdPart && opts::JumpTables > JTS_NONE)
    Function.emitJumpTables(&Streamer);

  Function.setEmitted();
}

namespace {

template <typename T>
std::vector<T> singletonSet(T t) {
  std::vector<T> Vec;
  Vec.push_back(std::move(t));
  return Vec;
}

} // anonymous namespace

void RewriteInstance::emitFunctions() {
  NamedRegionTimer T("emitFunctions", "emit functions", TimerGroupName,
                     TimerGroupDesc, opts::TimeRewrite);
  std::error_code EC;

  // This is an object file, which we keep for debugging purposes.
  // Once we decide it's useless, we should create it in memory.
  std::unique_ptr<ToolOutputFile> TempOut =
    llvm::make_unique<ToolOutputFile>(opts::OutputFilename + ".bolt.o",
                                      EC, sys::fs::F_None);
  check_error(EC, "cannot create output object file");

  std::unique_ptr<buffer_ostream> BOS =
      make_unique<buffer_ostream>(TempOut->os());
  raw_pwrite_stream *OS = BOS.get();

  // Implicitly MCObjectStreamer takes ownership of MCAsmBackend (MAB)
  // and MCCodeEmitter (MCE). ~MCObjectStreamer() will delete these
  // two instances.
  auto MCE = BC->TheTarget->createMCCodeEmitter(*BC->MII, *BC->MRI, *BC->Ctx);
  auto MAB =
      BC->TheTarget->createMCAsmBackend(*BC->STI, *BC->MRI, MCTargetOptions());
  std::unique_ptr<MCStreamer> Streamer(BC->TheTarget->createMCObjectStreamer(
      *BC->TheTriple, *BC->Ctx, std::unique_ptr<MCAsmBackend>(MAB), *OS,
      std::unique_ptr<MCCodeEmitter>(MCE), *BC->STI,
      /* RelaxAll */ false,
      /* IncrementalLinkerCompatible */ false,
      /* DWARFMustBeAtTheEnd */ false));

  Streamer->InitSections(false);

  // Mark beginning of "hot text".
  if (BC->HasRelocations && opts::HotText)
    Streamer->EmitLabel(BC->Ctx->getOrCreateSymbol("__hot_start"));

  // Sort functions for the output.
  std::vector<BinaryFunction *> SortedFunctions =
      BinaryContext::getSortedFunctions(BinaryFunctions);

  DEBUG(
    if (!BC->HasRelocations) {
      auto SortedIt = SortedFunctions.begin();
      for (auto &It : BinaryFunctions) {
        assert(&It.second == *SortedIt);
        ++SortedIt;
      }
    });

  uint32_t LastHotIndex = -1u;
  uint32_t CurrentIndex = 0;
  for (auto *BF : SortedFunctions) {
    if (!BF->hasValidIndex() && LastHotIndex == -1u) {
      LastHotIndex = CurrentIndex;
    }
    assert(LastHotIndex == -1u || !BF->hasValidIndex());
    assert(!BF->hasValidIndex() || CurrentIndex == BF->getIndex());
    ++CurrentIndex;
  }
  CurrentIndex = 0;
  DEBUG(dbgs() << "BOLT-DEBUG: LastHotIndex = " << LastHotIndex << "\n");

  bool ColdFunctionSeen = false;

  // Output functions one by one.
  for (auto *FunctionPtr : SortedFunctions) {
    auto &Function = *FunctionPtr;

    // Emit all cold function split parts at the border of hot and
    // cold functions.
    if (BC->HasRelocations && !ColdFunctionSeen &&
        CurrentIndex >= LastHotIndex) {
      // Mark the end of "hot" stuff.
      if (opts::HotText) {
        Streamer->SwitchSection(BC->MOFI->getTextSection());
        Streamer->EmitLabel(BC->Ctx->getOrCreateSymbol("__hot_end"));
      }

      ColdFunctionSeen = true;
      if (opts::SplitFunctions != BinaryFunction::ST_NONE) {
        DEBUG(dbgs() << "BOLT-DEBUG: generating code for split functions\n");
        for (auto *FPtr : SortedFunctions) {
          if (!FPtr->isSplit() || !FPtr->isSimple())
            continue;
          emitFunction(*Streamer, *FPtr, /*EmitColdPart=*/true);
        }
      }
      DEBUG(dbgs() << "BOLT-DEBUG: first cold function: " << Function << '\n');
    }

    if (!BC->HasRelocations &&
        (!Function.isSimple() || !opts::shouldProcess(Function))) {
      ++CurrentIndex;
      continue;
    }

    DEBUG(dbgs() << "BOLT: generating code for function \""
                 << Function << "\" : "
                 << Function.getFunctionNumber() << '\n');

    emitFunction(*Streamer, Function, /*EmitColdPart=*/false);

    if (!BC->HasRelocations && Function.isSplit())
      emitFunction(*Streamer, Function, /*EmitColdPart=*/true);

    ++CurrentIndex;
  }

  if (!ColdFunctionSeen && opts::HotText) {
    Streamer->SwitchSection(BC->MOFI->getTextSection());
    Streamer->EmitLabel(BC->Ctx->getOrCreateSymbol("__hot_end"));
  }

  if (!BC->HasRelocations && opts::UpdateDebugSections)
    updateDebugLineInfoForNonSimpleFunctions();

  emitDataSections(Streamer.get());

  // Relocate .eh_frame to .eh_frame_old.
  if (EHFrameSection) {
    relocateEHFrameSection();
    emitDataSection(Streamer.get(), *EHFrameSection, ".eh_frame_old");
  }

  // Update _end if needed.
  if (opts::UpdateEnd) {
    Streamer->EmitLabel(BC->Ctx->getOrCreateSymbol("_end"));
  }

  Streamer->Finish();

  //////////////////////////////////////////////////////////////////////////////
  // Assign addresses to new sections.
  //////////////////////////////////////////////////////////////////////////////

  if (opts::UpdateDebugSections) {
    // Compute offsets of tables in .debug_line for each compile unit.
    updateLineTableOffsets();
  }

  // Get output object as ObjectFile.
  std::unique_ptr<MemoryBuffer> ObjectMemBuffer =
      MemoryBuffer::getMemBuffer(BOS->str(), "in-memory object file", false);
  std::unique_ptr<object::ObjectFile> Obj = cantFail(
      object::ObjectFile::createObjectFile(ObjectMemBuffer->getMemBufferRef()),
      "error creating in-memory object");

  auto Resolver = orc::createLegacyLookupResolver(
      [&](const std::string &Name) -> JITSymbol {
        DEBUG(dbgs() << "BOLT: looking for " << Name << "\n");
        if (auto *I = BC->getBinaryDataByName(Name)) {
          const uint64_t Address = I->isMoved() && !I->isJumpTable()
                                 ? I->getOutputAddress()
                                 : I->getAddress();
          return JITSymbol(Address, JITSymbolFlags());
        }
        return JITSymbol(nullptr);
      },
      [](Error Err) { cantFail(std::move(Err), "lookup failed"); });
  Resolver->setAllowsZeroSymbols(true);

  MCAsmLayout FinalLayout(
        static_cast<MCObjectStreamer *>(Streamer.get())->getAssembler());

  SSP.reset(new decltype(SSP)::element_type());
  ES.reset(new decltype(ES)::element_type(*SSP));
  OLT.reset(new decltype(OLT)::element_type(
      *ES,
      [this, &Resolver](orc::VModuleKey Key) {
        orc::RTDyldObjectLinkingLayer::Resources R;
        R.MemMgr = EFMM;
        R.Resolver = Resolver;
        // Get memory manager
        return R;
      },
      // Loaded notifier
      [&](orc::VModuleKey Key, const object::ObjectFile &Obj,
          const RuntimeDyld::LoadedObjectInfo &) {
        // Assign addresses to all sections.
        mapFileSections(Key);
      },
      // Finalized notifier
      [&](orc::VModuleKey Key) {
        // Update output addresses based on the new section map and
        // layout.
        updateOutputValues(FinalLayout);
      }));

  OLT->setProcessAllSections(true);
  auto K = ES->allocateVModule();
  cantFail(OLT->addObject(K, std::move(ObjectMemBuffer)));

  cantFail(OLT->emitAndFinalize(K));

  if (opts::PrintCacheMetrics) {
    outs() << "BOLT-INFO: cache metrics after emitting functions:\n";
    CacheMetrics::printAll(SortedFunctions);
  }

  if (opts::KeepTmp)
    TempOut->keep();
}

void RewriteInstance::mapFileSections(orc::VModuleKey Key) {
  mapTextSections(Key);
  mapDataSections(Key);
}

void RewriteInstance::mapTextSections(orc::VModuleKey Key) {
  NewTextSectionStartAddress = NextAvailableAddress;
  if (BC->HasRelocations) {
    auto TextSection = BC->getUniqueSectionByName(".text");
    assert(TextSection && ".text not found in output");

    uint64_t NewTextSectionOffset = 0;
    if (opts::UseOldText &&
        TextSection->getOutputSize() <= BC->OldTextSectionSize) {
      outs() << "BOLT-INFO: using original .text for new code\n";
      // Utilize the original .text for storage.
      NewTextSectionStartAddress = BC->OldTextSectionAddress;
      NewTextSectionOffset = BC->OldTextSectionOffset;
      auto Padding = OffsetToAlignment(NewTextSectionStartAddress, PageAlign);
      if (Padding + TextSection->getOutputSize() <= BC->OldTextSectionSize) {
        outs() << "BOLT-INFO: using 0x200000 alignment\n";
        NewTextSectionStartAddress += Padding;
        NewTextSectionOffset += Padding;
      }
    } else {
      if (opts::UseOldText) {
        errs() << "BOLT-ERROR: original .text too small to fit the new code. "
               << TextSection->getOutputSize() << " bytes needed, have "
               << BC->OldTextSectionSize << " bytes available.\n";
      }
      auto Padding = OffsetToAlignment(NewTextSectionStartAddress, PageAlign);
      NextAvailableAddress += Padding;
      NewTextSectionStartAddress = NextAvailableAddress;
      NewTextSectionOffset = getFileOffsetForAddress(NextAvailableAddress);
      NextAvailableAddress += Padding + TextSection->getOutputSize();
    }
    TextSection->setFileAddress(NewTextSectionStartAddress);
    TextSection->setFileOffset(NewTextSectionOffset);

    DEBUG(dbgs() << "BOLT: mapping .text 0x"
                 << Twine::utohexstr(TextSection->getAllocAddress())
                 << " to 0x" << Twine::utohexstr(NewTextSectionStartAddress)
                 << '\n');
    OLT->mapSectionAddress(Key, TextSection->getSectionID(),
                           NewTextSectionStartAddress);
  } else {
    for (auto &BFI : BinaryFunctions) {
      auto &Function = BFI.second;
      if (!Function.isSimple() || !opts::shouldProcess(Function))
        continue;

      auto TooLarge = false;
      auto FuncSection =
        BC->getUniqueSectionByName(Function.getCodeSectionName());
      assert(FuncSection && "cannot find section for function");
      DEBUG(dbgs() << "BOLT: mapping 0x"
                   << Twine::utohexstr(FuncSection->getAllocAddress())
                   << " to 0x" << Twine::utohexstr(Function.getAddress())
                   << '\n');
      OLT->mapSectionAddress(Key, FuncSection->getSectionID(),
                             Function.getAddress());
      Function.setImageAddress(FuncSection->getAllocAddress());
      Function.setImageSize(FuncSection->getOutputSize());
      if (Function.getImageSize() > Function.getMaxSize()) {
        TooLarge = true;
        FailedAddresses.emplace_back(Function.getAddress());
      }

      // Map jump tables if updating in-place.
      if (opts::JumpTables == JTS_BASIC) {
        for (auto &JTI : Function.JumpTables) {
          auto *JT = JTI.second;
          auto &Section = JT->getOutputSection();
          Section.setFileAddress(JT->getAddress());
          DEBUG(dbgs() << "BOLT-DEBUG: mapping " << Section.getName()
                       << " to 0x" << Twine::utohexstr(JT->getAddress())
                       << '\n');
          OLT->mapSectionAddress(Key, Section.getSectionID(),
                                 JT->getAddress());
        }
      }

      if (!Function.isSplit())
        continue;

      auto ColdSection =
        BC->getUniqueSectionByName(Function.getColdCodeSectionName());
      assert(ColdSection && "cannot find section for cold part");
      // Cold fragments are aligned at 16 bytes.
      NextAvailableAddress = alignTo(NextAvailableAddress, 16);
      auto &ColdPart = Function.cold();
      if (TooLarge) {
        // The corresponding FDE will refer to address 0.
        ColdPart.setAddress(0);
        ColdPart.setImageAddress(0);
        ColdPart.setImageSize(0);
        ColdPart.setFileOffset(0);
      } else {
        ColdPart.setAddress(NextAvailableAddress);
        ColdPart.setImageAddress(ColdSection->getAllocAddress());
        ColdPart.setImageSize(ColdSection->getOutputSize());
        ColdPart.setFileOffset(getFileOffsetForAddress(NextAvailableAddress));
      }

      DEBUG(dbgs() << "BOLT: mapping cold fragment 0x"
                   << Twine::utohexstr(ColdPart.getImageAddress())
                   << " to 0x"
                   << Twine::utohexstr(ColdPart.getAddress())
                   << " with size "
                   << Twine::utohexstr(ColdPart.getImageSize()) << '\n');
      OLT->mapSectionAddress(Key, ColdSection->getSectionID(),
                             ColdPart.getAddress());

      NextAvailableAddress += ColdPart.getImageSize();
    }

    // Add the new text section aggregating all existing code sections.
    // This is pseudo-section that serves a purpose of creating a corresponding
    // entry in section header table.
    auto NewTextSectionSize = NextAvailableAddress - NewTextSectionStartAddress;
    if (NewTextSectionSize) {
      const auto Flags = BinarySection::getFlags(/*IsReadOnly=*/true,
                                                 /*IsText=*/true,
                                                 /*IsAllocatable=*/true);
      auto &Section = BC->registerOrUpdateSection(BOLTSecPrefix + ".text",
                                                  ELF::SHT_PROGBITS,
                                                  Flags,
                                                  nullptr,
                                                  NewTextSectionSize,
                                                  16,
                                                  true /*IsLocal*/);
      Section.setFileAddress(NewTextSectionStartAddress);
      Section.setFileOffset(
        getFileOffsetForAddress(NewTextSectionStartAddress));
    }
  }
}

void RewriteInstance::mapDataSections(orc::VModuleKey Key) {
  // Map special sections to their addresses in the output image.
  // These are the sections that we generate via MCStreamer.
  // The order is important.
  std::vector<std::string> Sections = { ".eh_frame", ".eh_frame_old",
                                        ".gcc_except_table",
                                        ".rodata", ".rodata.cold" };
  for (auto &SectionName : Sections) {
    auto Section = BC->getUniqueSectionByName(SectionName);
    if (!Section || !Section->isAllocatable() || !Section->isFinalized())
      continue;
    NextAvailableAddress = alignTo(NextAvailableAddress,
                                   Section->getAlignment());
    DEBUG(dbgs() << "BOLT: mapping section " << SectionName << " (0x"
                 << Twine::utohexstr(Section->getAllocAddress())
                 << ") to 0x" << Twine::utohexstr(NextAvailableAddress)
                 << ":0x" << Twine::utohexstr(NextAvailableAddress +
                                              Section->getOutputSize())
                 << '\n');

    OLT->mapSectionAddress(Key, Section->getSectionID(), NextAvailableAddress);
    Section->setFileAddress(NextAvailableAddress);
    Section->setFileOffset(getFileOffsetForAddress(NextAvailableAddress));

    NextAvailableAddress += Section->getOutputSize();
  }

  // Handling for sections with relocations.
  for (const auto &Section : BC->sections()) {
    if (!Section.hasRelocations() || !Section.hasSectionRef())
      continue;

    StringRef SectionName = Section.getName();
    auto OrgSection =
      BC->getUniqueSectionByName(OrgSecPrefix + std::string(SectionName));
    if (!OrgSection ||
        !OrgSection->isAllocatable() ||
        !OrgSection->isFinalized())
      continue;

    if (OrgSection->getFileAddress()) {
      DEBUG(dbgs() << "BOLT-DEBUG: section " << SectionName
                   << " is already mapped at 0x"
                   << Twine::utohexstr(OrgSection->getFileAddress()) << '\n');
      continue;
    }
    DEBUG(dbgs() << "BOLT: mapping original section " << SectionName << " (0x"
                 << Twine::utohexstr(OrgSection->getAllocAddress())
                 << ") to 0x" << Twine::utohexstr(Section.getAddress())
                 << '\n');

    OLT->mapSectionAddress(Key, OrgSection->getSectionID(),
                           Section.getAddress());

    OrgSection->setFileAddress(Section.getAddress());
    OrgSection->setFileOffset(Section.getContents().data() -
                              InputFile->getData().data());
  }
}

void RewriteInstance::updateOutputValues(const MCAsmLayout &Layout) {
  for (auto &BFI : BinaryFunctions) {
    auto &Function = BFI.second;

    if (!Function.isEmitted()) {
      Function.setOutputAddress(Function.getAddress());
      Function.setOutputSize(Function.getSize());
      continue;
    }

    if (BC->HasRelocations) {
      const auto BaseAddress = NewTextSectionStartAddress;
      const auto StartOffset = Layout.getSymbolOffset(*Function.getSymbol());
      const auto EndOffset =
        Layout.getSymbolOffset(*Function.getFunctionEndLabel());
      if (Function.hasConstantIsland()) {
        const auto DataOffset =
            Layout.getSymbolOffset(*Function.getFunctionConstantIslandLabel());
        Function.setOutputDataAddress(BaseAddress + DataOffset);
      }
      Function.setOutputAddress(BaseAddress + StartOffset);
      Function.setOutputSize(EndOffset - StartOffset);
      if (Function.isSplit()) {
        const auto *ColdStartSymbol = Function.getColdSymbol();
        assert(ColdStartSymbol && ColdStartSymbol->isDefined() &&
               "split function should have defined cold symbol");
        const auto *ColdEndSymbol = Function.getFunctionColdEndLabel();
        assert(ColdEndSymbol && ColdEndSymbol->isDefined() &&
               "split function should have defined cold end symbol");
        const auto ColdStartOffset = Layout.getSymbolOffset(*ColdStartSymbol);
        const auto ColdEndOffset = Layout.getSymbolOffset(*ColdEndSymbol);
        Function.cold().setAddress(BaseAddress + ColdStartOffset);
        Function.cold().setImageSize(ColdEndOffset - ColdStartOffset);
        if (Function.hasConstantIsland()) {
          const auto DataOffset = Layout.getSymbolOffset(
              *Function.getFunctionColdConstantIslandLabel());
          Function.setOutputColdDataAddress(BaseAddress + DataOffset);
        }
      }
    } else {
      Function.setOutputAddress(Function.getAddress());
      Function.setOutputSize(
          Layout.getSymbolOffset(*Function.getFunctionEndLabel()));
    }

    // Update basic block output ranges only for the debug info.
    if (!opts::UpdateDebugSections)
      continue;

    // Output ranges should match the input if the body hasn't changed.
    if (!Function.isSimple() && !BC->HasRelocations)
      continue;

    // AArch64 may have functions that only contains a constant island (no code)
    if (Function.layout_begin() == Function.layout_end())
      continue;

    BinaryBasicBlock *PrevBB = nullptr;
    for (auto BBI = Function.layout_begin(), BBE = Function.layout_end();
         BBI != BBE; ++BBI) {
      auto *BB = *BBI;
      assert(BB->getLabel()->isDefined() && "symbol should be defined");
      uint64_t BaseAddress;
      if (BC->HasRelocations) {
        BaseAddress = NewTextSectionStartAddress;
      } else {
        BaseAddress = BB->isCold() ? Function.cold().getAddress()
                                   : Function.getOutputAddress();
      }
      uint64_t Address = BaseAddress + Layout.getSymbolOffset(*BB->getLabel());
      BB->setOutputStartAddress(Address);

      if (PrevBB) {
        auto PrevBBEndAddress = Address;
        if (BB->isCold() != PrevBB->isCold()) {
          PrevBBEndAddress =
            Function.getOutputAddress() + Function.getOutputSize();
        }
        PrevBB->setOutputEndAddress(PrevBBEndAddress);
      }
      PrevBB = BB;
    }
    PrevBB->setOutputEndAddress(PrevBB->isCold() ?
        Function.cold().getAddress() + Function.cold().getImageSize() :
        Function.getOutputAddress() + Function.getOutputSize());
  }
}

void RewriteInstance::emitDataSection(MCStreamer *Streamer,
                                      const BinarySection &Section,
                                      StringRef NewName) {
  StringRef SectionName = !NewName.empty() ? NewName : Section.getName();
  StringRef SectionContents = Section.getContents();
  auto *ELFSection = BC->Ctx->getELFSection(SectionName,
                                            Section.getELFType(),
                                            Section.getELFFlags());

  Streamer->SwitchSection(ELFSection);
  Streamer->EmitValueToAlignment(Section.getAlignment());

  if (BC->HasRelocations && opts::HotData && Section.isReordered())
    Streamer->EmitLabel(BC->Ctx->getOrCreateSymbol("__hot_data_start"));

  DEBUG(dbgs() << "BOLT-DEBUG: emitting "
               << (Section.isAllocatable() ? "" : "non-")
               << "allocatable data section " << SectionName << '\n');

  if (!Section.hasRelocations()) {
    Streamer->EmitBytes(SectionContents);
  } else {
    uint64_t SectionOffset = 0;
    for (auto &Relocation : Section.relocations()) {
      assert(Relocation.Offset < SectionContents.size() && "overflow detected");
      if (SectionOffset < Relocation.Offset) {
        Streamer->EmitBytes(
            SectionContents.substr(SectionOffset,
                                   Relocation.Offset - SectionOffset));
        SectionOffset = Relocation.Offset;
      }
      DEBUG(dbgs() << "BOLT-DEBUG: emitting relocation for symbol "
            << Relocation.Symbol->getName() << " at offset 0x"
            << Twine::utohexstr(Relocation.Offset)
            << " with size "
            << Relocation::getSizeForType(Relocation.Type) << '\n');
      auto RelocationSize = Relocation.emit(Streamer);
      SectionOffset += RelocationSize;
    }
    assert(SectionOffset <= SectionContents.size() && "overflow error");
    if (SectionOffset < SectionContents.size()) {
      Streamer->EmitBytes(SectionContents.substr(SectionOffset));
    }
  }

  if (BC->HasRelocations && opts::HotData && Section.isReordered())
    Streamer->EmitLabel(BC->Ctx->getOrCreateSymbol("__hot_data_end"));
}

void RewriteInstance::emitDataSections(MCStreamer *Streamer) {
  for (const auto &Section : BC->sections()) {
    if (!Section.hasRelocations() || !Section.hasSectionRef())
      continue;

    StringRef SectionName = Section.getName();
    assert(SectionName != ".eh_frame" && "should not emit .eh_frame as data");
    std::string EmitName = Section.isReordered()
      ? std::string(Section.getOutputName())
      : OrgSecPrefix + std::string(SectionName);
    emitDataSection(Streamer, Section, EmitName);
  }
}

bool RewriteInstance::checkLargeFunctions() {
  if (BC->HasRelocations)
    return false;

  LargeFunctions.clear();
  for (auto &BFI : BinaryFunctions) {
    auto &Function = BFI.second;

    // Ignore this function if we failed to map it to the output binary
    if (Function.getImageAddress() == 0 || Function.getImageSize() == 0)
      continue;

    if (Function.getImageSize() <= Function.getMaxSize())
      continue;

    LargeFunctions.insert(BFI.first);
  }
  return !LargeFunctions.empty();
}

void RewriteInstance::patchELFPHDRTable() {
  auto ELF64LEFile = dyn_cast<ELF64LEObjectFile>(InputFile);
  if (!ELF64LEFile) {
    errs() << "BOLT-ERROR: only 64-bit LE ELF binaries are supported\n";
    exit(1);
  }
  auto Obj = ELF64LEFile->getELFFile();
  auto &OS = Out->os();

  // Write/re-write program headers.
  Phnum = Obj->getHeader()->e_phnum;
  if (PHDRTableOffset) {
    // Writing new pheader table.
    Phnum += 1; // only adding one new segment
    // Segment size includes the size of the PHDR area.
    NewTextSegmentSize = NextAvailableAddress - PHDRTableAddress;
  } else {
    assert(!PHDRTableAddress && "unexpected address for program header table");
    // Update existing table.
    PHDRTableOffset = Obj->getHeader()->e_phoff;
    NewTextSegmentSize = NextAvailableAddress - NewTextSegmentAddress;
  }
  OS.seek(PHDRTableOffset);

  bool ModdedGnuStack = false;
  (void)ModdedGnuStack;
  bool AddedSegment = false;
  (void)AddedSegment;

  // Copy existing program headers with modifications.
  for (auto &Phdr : cantFail(Obj->program_headers())) {
    auto NewPhdr = Phdr;
    if (PHDRTableAddress && Phdr.p_type == ELF::PT_PHDR) {
      NewPhdr.p_offset = PHDRTableOffset;
      NewPhdr.p_vaddr = PHDRTableAddress;
      NewPhdr.p_paddr = PHDRTableAddress;
      NewPhdr.p_filesz = sizeof(NewPhdr) * Phnum;
      NewPhdr.p_memsz = sizeof(NewPhdr) * Phnum;
    } else if (Phdr.p_type == ELF::PT_GNU_EH_FRAME) {
      auto EHFrameHdrSec = BC->getUniqueSectionByName(".eh_frame_hdr");
      if (EHFrameHdrSec &&
          EHFrameHdrSec->isAllocatable() &&
          EHFrameHdrSec->isFinalized()) {
        NewPhdr.p_offset = EHFrameHdrSec->getFileOffset();
        NewPhdr.p_vaddr = EHFrameHdrSec->getFileAddress();
        NewPhdr.p_paddr = EHFrameHdrSec->getFileAddress();
        NewPhdr.p_filesz = EHFrameHdrSec->getOutputSize();
        NewPhdr.p_memsz = EHFrameHdrSec->getOutputSize();
      }
    } else if (opts::UseGnuStack && Phdr.p_type == ELF::PT_GNU_STACK) {
      NewPhdr.p_type = ELF::PT_LOAD;
      NewPhdr.p_offset = NewTextSegmentOffset;
      NewPhdr.p_vaddr = NewTextSegmentAddress;
      NewPhdr.p_paddr = NewTextSegmentAddress;
      NewPhdr.p_filesz = NewTextSegmentSize;
      NewPhdr.p_memsz = NewTextSegmentSize;
      NewPhdr.p_flags = ELF::PF_X | ELF::PF_R;
      NewPhdr.p_align = PageAlign;
      ModdedGnuStack = true;
    } else if (!opts::UseGnuStack && Phdr.p_type == ELF::PT_DYNAMIC) {
      // Insert new pheader
      ELFFile<ELF64LE>::Elf_Phdr NewTextPhdr;
      NewTextPhdr.p_type = ELF::PT_LOAD;
      NewTextPhdr.p_offset = PHDRTableOffset;
      NewTextPhdr.p_vaddr = PHDRTableAddress;
      NewTextPhdr.p_paddr = PHDRTableAddress;
      NewTextPhdr.p_filesz = NewTextSegmentSize;
      NewTextPhdr.p_memsz = NewTextSegmentSize;
      NewTextPhdr.p_flags = ELF::PF_X | ELF::PF_R;
      NewTextPhdr.p_align = PageAlign;
      OS.write(reinterpret_cast<const char *>(&NewTextPhdr),
               sizeof(NewTextPhdr));
      AddedSegment = true;
    }
    OS.write(reinterpret_cast<const char *>(&NewPhdr), sizeof(NewPhdr));
  }

  assert((!opts::UseGnuStack || ModdedGnuStack) &&
         "could not find GNU_STACK program header to modify");

  assert((opts::UseGnuStack || AddedSegment) &&
         "could not add program header for the new segment");
}

namespace {

/// Write padding to \p OS such that its current \p Offset becomes aligned
/// at \p Alignment. Return new (aligned) offset.
uint64_t appendPadding(raw_pwrite_stream &OS,
                       uint64_t Offset,
                       uint64_t Alignment) {
  if (!Alignment)
    return Offset;

  const auto PaddingSize = OffsetToAlignment(Offset, Alignment);
  for (unsigned I = 0; I < PaddingSize; ++I)
    OS.write((unsigned char)0);
  return Offset + PaddingSize;
}

}

void RewriteInstance::rewriteNoteSections() {
  auto ELF64LEFile = dyn_cast<ELF64LEObjectFile>(InputFile);
  if (!ELF64LEFile) {
    errs() << "BOLT-ERROR: only 64-bit LE ELF binaries are supported\n";
    exit(1);
  }
  auto Obj = ELF64LEFile->getELFFile();
  auto &OS = Out->os();

  uint64_t NextAvailableOffset = getFileOffsetForAddress(NextAvailableAddress);
  assert(NextAvailableOffset >= FirstNonAllocatableOffset &&
         "next available offset calculation failure");
  OS.seek(NextAvailableOffset);

  // Copy over non-allocatable section contents and update file offsets.
  for (auto &Section : cantFail(Obj->sections())) {
    if (Section.sh_type == ELF::SHT_NULL)
      continue;
    if (Section.sh_flags & ELF::SHF_ALLOC)
      continue;

    if (Section.sh_type == ELF::SHT_RELA)
      continue;

    // Insert padding as needed.
    NextAvailableOffset =
      appendPadding(OS, NextAvailableOffset, Section.sh_addralign);

    StringRef SectionName =
        cantFail(Obj->getSectionName(&Section), "cannot get section name");

    // New section size.
    uint64_t Size = 0;

    // Copy over section contents unless it's one of the sections we overwrite.
    if (!willOverwriteSection(SectionName)) {
      Size = Section.sh_size;
      std::string Data = InputFile->getData().substr(Section.sh_offset, Size);
      auto SectionPatchersIt = SectionPatchers.find(SectionName);
      if (SectionPatchersIt != SectionPatchers.end()) {
        (*SectionPatchersIt->second).patchBinary(Data);
      }
      OS << Data;

      // Add padding as the section extension might rely on the alignment.
      Size = appendPadding(OS, Size, Section.sh_addralign);
    }

    // Perform section post-processing.
    auto BSec = BC->getUniqueSectionByName(SectionName);
    uint8_t *SectionData = nullptr;
    if (BSec && !BSec->isAllocatable()) {
      assert(BSec->getAlignment() <= Section.sh_addralign &&
             "alignment exceeds value in file");

      if (BSec->getAllocAddress()) {
        SectionData = BSec->getOutputData();
        DEBUG(dbgs() << "BOLT-DEBUG: " << (Size ? "appending" : "writing")
                     << " contents to section "
                     << SectionName << '\n');
        OS.write(reinterpret_cast<char *>(SectionData),
                 BSec->getOutputSize());
        Size += BSec->getOutputSize();
      }

      if (BSec->hasPendingRelocations()) {
        DEBUG(dbgs() << "BOLT-DEBUG: processing relocs for section "
                     << SectionName << '\n');
        for (auto &Reloc : BSec->pendingRelocations()) {
          DEBUG(dbgs() << "BOLT-DEBUG: writing value 0x"
                       << Twine::utohexstr(Reloc.Addend)
                       << " of size " << Relocation::getSizeForType(Reloc.Type)
                       << " at offset 0x"
                       << Twine::utohexstr(Reloc.Offset) << '\n');
          assert(Reloc.Type == ELF::R_X86_64_32 &&
                 "only R_X86_64_32 relocations are supported at the moment");
          uint32_t Value = Reloc.Addend;
          OS.pwrite(reinterpret_cast<const char*>(&Value),
                    Relocation::getSizeForType(Reloc.Type),
                    NextAvailableOffset + Reloc.Offset);
        }
      }
    }

    // Set/modify section info.
    auto &NewSection =
      BC->registerOrUpdateNoteSection(SectionName,
                                      SectionData,
                                      Size,
                                      Section.sh_addralign,
                                      BSec ? BSec->isReadOnly() : false,
                                      BSec ? BSec->getELFType()
                                           : ELF::SHT_PROGBITS,
                                      BSec ? BSec->isLocal() : false);
    NewSection.setFileAddress(0);
    NewSection.setFileOffset(NextAvailableOffset);

    NextAvailableOffset += Size;
  }

  // Write new note sections.
  for (auto &Section : BC->nonAllocatableSections()) {
    if (Section.getFileOffset() || !Section.getAllocAddress())
      continue;

    assert(!Section.hasPendingRelocations() && "cannot have pending relocs");

    NextAvailableOffset = appendPadding(OS, NextAvailableOffset,
                                        Section.getAlignment());
    Section.setFileOffset(NextAvailableOffset);

    DEBUG(dbgs() << "BOLT-DEBUG: writing out new section "
                 << Section.getName() << " of size " << Section.getOutputSize()
                 << " at offset 0x" << Twine::utohexstr(Section.getFileOffset())
                 << '\n');

    OS.write(Section.getOutputContents().data(), Section.getOutputSize());
    NextAvailableOffset += Section.getOutputSize();
  }
}

template <typename ELFT>
void RewriteInstance::finalizeSectionStringTable(ELFObjectFile<ELFT> *File) {
  auto *Obj = File->getELFFile();

  // Pre-populate section header string table.
  for (auto &Section : cantFail(Obj->sections())) {
    StringRef SectionName =
        cantFail(Obj->getSectionName(&Section), "cannot get section name");
    SHStrTab.add(SectionName);
    if (willOverwriteSection(SectionName)) {
      AllSHStrTabStrings.emplace_back(
          SHStrTabPool.intern(OrgSecPrefix + SectionName.str()));
      SHStrTab.add(*AllSHStrTabStrings.back());
    }
  }
  for (const auto &Section : BC->sections()) {
    SHStrTab.add(Section.getName());
  }
  SHStrTab.finalize();

  const auto SHStrTabSize = SHStrTab.getSize();
  uint8_t *DataCopy = new uint8_t[SHStrTabSize];
  memset(DataCopy, 0, SHStrTabSize);
  SHStrTab.write(DataCopy);
  BC->registerOrUpdateNoteSection(".shstrtab",
                                  DataCopy,
                                  SHStrTabSize,
                                  /*Alignment=*/1,
                                  /*IsReadOnly=*/true,
                                  ELF::SHT_STRTAB);
}

void RewriteInstance::addBoltInfoSection() {
  if (opts::AddBoltInfo) {
    std::string DescStr;
    raw_string_ostream DescOS(DescStr);

    DescOS << "BOLT revision: " << BoltRevision << ", " << "command line:";
    for (auto I = 0; I < Argc; ++I) {
      DescOS << " " << Argv[I];
    }
    DescOS.flush();

    std::string Str;
    raw_string_ostream OS(Str);
    std::string NameStr = "GNU";
    const uint32_t NameSz = NameStr.size() + 1;
    const uint32_t DescSz = DescStr.size();
    const uint32_t Type = 4; // NT_GNU_GOLD_VERSION (gold version)
    OS.write(reinterpret_cast<const char*>(&(NameSz)), 4);
    OS.write(reinterpret_cast<const char*>(&(DescSz)), 4);
    OS.write(reinterpret_cast<const char*>(&(Type)), 4);
    OS << NameStr;
    for (uint64_t I = NameStr.size();
         I < alignTo(NameStr.size(), 4); ++I) {
      OS << '\0';
    }
    OS << DescStr;
    for (uint64_t I = DescStr.size();
         I < alignTo(DescStr.size(), 4); ++I) {
      OS << '\0';
    }

    const auto BoltInfo = OS.str();
    BC->registerOrUpdateNoteSection(".note.bolt_info",
                                    copyByteArray(BoltInfo),
                                    BoltInfo.size(),
                                    /*Alignment=*/1,
                                    /*IsReadOnly=*/true,
                                    ELF::SHT_NOTE);
  }
}

// Provide a mapping of the existing input binary sections to the output binary
// section header table.
// Return the map from the section header old index to its new index. Optionally
// return in OutputSections an ordered list of the output sections. This is
// optional because for reference updating in the symbol table we only need the
// map of input to output indices, not the real output section list.
template <typename ELFT, typename ELFShdrTy>
std::vector<uint32_t> RewriteInstance::getOutputSections(
   ELFObjectFile<ELFT> *File,
   std::vector<ELFShdrTy> *OutputSections,
   std::map<std::string, uint32_t> *SectionNameMap
) {
  auto *Obj = File->getELFFile();
  auto Sections = cantFail(Obj->sections());

  std::vector<uint32_t> NewSectionIndex(
      std::distance(Sections.begin(), Sections.end()), 0);
  NewTextSectionIndex = 0;
  uint32_t CurIndex{0};

  // Copy over entries for original allocatable sections with minor
  // modifications (e.g. name).
  for (auto &Section : Sections) {
    // Always ignore this section.
    if (Section.sh_type == ELF::SHT_NULL) {
      NewSectionIndex[0] = CurIndex++;
      if (OutputSections)
        OutputSections->emplace_back(Section);
      continue;
    }

    // Is this our new text? Then update our pointer indicating the new output
    // text section
    if (opts::UseOldText && Section.sh_flags & ELF::SHF_ALLOC &&
        Section.sh_addr <= NewTextSectionStartAddress &&
        Section.sh_addr + Section.sh_size > NewTextSectionStartAddress) {
      NewTextSectionIndex = CurIndex;
    }

    // Skip non-allocatable sections.
    if (!(Section.sh_flags & ELF::SHF_ALLOC))
      continue;

    StringRef SectionName =
        cantFail(Obj->getSectionName(&Section), "cannot get section name");

    if (SectionNameMap && !SectionNameMap->count(SectionName)) {
      (*SectionNameMap)[SectionName] = CurIndex;
    }
    const auto OldIdx = std::distance(Sections.begin(), &Section);
    assert(NewSectionIndex[OldIdx] == 0);
    NewSectionIndex[OldIdx] = CurIndex++;

    // If only computing the map, we're done with this iteration
    if (!OutputSections)
      continue;

    auto NewSection = Section;
    if (SectionName == ".bss") {
      // .bss section offset matches that of the next section.
      NewSection.sh_offset = NewTextSegmentOffset;
    }

    if (willOverwriteSection(SectionName)) {
      NewSection.sh_name = SHStrTab.getOffset(OrgSecPrefix +
                                              SectionName.str());
    } else {
      NewSection.sh_name = SHStrTab.getOffset(SectionName);
    }

    OutputSections->emplace_back(NewSection);
  }

  // If we are creating our own .text section, it should be the first section
  // we created in BinaryContext, so this is the correct index.
  if (!opts::UseOldText) {
    NewTextSectionIndex = CurIndex;
  }

  // Process entries for all new allocatable sections.  Make sure
  // allocatable sections follow the same order as in mapDataSections so
  // that the section indices are consistent.
  std::vector<const BinarySection *> AllocatableSections;
  std::vector<std::string> SectionNames = { ".eh_frame",
                                            ".gcc_except_table",
                                            ".rodata",
                                            ".rodata.cold" };
  for (const auto &SectionName : SectionNames) {
    auto Section = BC->getUniqueSectionByName(SectionName);
    if (Section && Section->isFinalized()) {
      AllocatableSections.push_back(&*Section);
    }
  }
  for (auto &Section : BC->allocatableSections()) {
    if (!Section.isFinalized())
      continue;

    if (std::find_if(AllocatableSections.begin(),
                     AllocatableSections.end(),
                     [&Section](const BinarySection *BSec) {
                       return BSec == &Section;
                     }) == AllocatableSections.end()) {
      AllocatableSections.push_back(&Section);
    }
  }

  for (const auto *Section : AllocatableSections) {
    // Ignore function sections.
    if (Section->getFileAddress() < NewTextSegmentAddress) {
      if (opts::Verbosity)
        outs() << "BOLT-INFO: not writing section header for existing section "
               << Section->getName() << '\n';
      continue;
    }

    if (SectionNameMap) {
      (*SectionNameMap)[Section->getName()] = CurIndex;
    }
    ++CurIndex;

    // If only computing the map, we're done with this iteration
    if (!OutputSections)
      continue;

    if (opts::Verbosity >= 1)
      outs() << "BOLT-INFO: writing section header for "
             << Section->getName() << '\n';
    ELFShdrTy NewSection;
    NewSection.sh_name = SHStrTab.getOffset(Section->getName());
    NewSection.sh_type = ELF::SHT_PROGBITS;
    NewSection.sh_addr = Section->getFileAddress();
    NewSection.sh_offset = Section->getFileOffset();
    NewSection.sh_size = Section->getOutputSize();
    NewSection.sh_entsize = 0;
    NewSection.sh_flags = Section->getELFFlags();
    NewSection.sh_link = 0;
    NewSection.sh_info = 0;
    NewSection.sh_addralign = Section->getAlignment();
    OutputSections->emplace_back(NewSection);
  }

  uint64_t LastFileOffset = 0;

  // Copy over entries for non-allocatable sections performing necessary
  // adjustments.
  for (auto &Section : Sections) {
    if (Section.sh_type == ELF::SHT_NULL)
      continue;
    if (Section.sh_flags & ELF::SHF_ALLOC)
      continue;
    // Strip non-allocatable relocation sections.
    if (Section.sh_type == ELF::SHT_RELA)
      continue;

    StringRef SectionName =
        cantFail(Obj->getSectionName(&Section), "cannot get section name");

    if (SectionNameMap && !SectionNameMap->count(SectionName)) {
      (*SectionNameMap)[SectionName] = CurIndex;
    }
    const auto OldIdx = std::distance(Sections.begin(), &Section);
    assert(NewSectionIndex[OldIdx] == 0);
    NewSectionIndex[OldIdx] = CurIndex++;

    // If only computing the map, we're done with this iteration
    if (!OutputSections)
      continue;

    auto BSec = BC->getUniqueSectionByName(SectionName);
    assert(BSec && "missing section info for non-allocatable section");

    auto NewSection = Section;
    NewSection.sh_offset = BSec->getFileOffset();
    NewSection.sh_size = BSec->getOutputSize();
    NewSection.sh_name = SHStrTab.getOffset(SectionName);

    OutputSections->emplace_back(NewSection);

    LastFileOffset = BSec->getFileOffset();
  }

  // Map input -> output is ready. Early return if that's all we need.
  if (!OutputSections)
    return NewSectionIndex;

  // Create entries for new non-allocatable sections.
  for (auto &Section : BC->nonAllocatableSections()) {
    if (Section.getFileOffset() <= LastFileOffset)
      continue;

    if (opts::Verbosity >= 1) {
      outs() << "BOLT-INFO: writing section header for "
             << Section.getName() << '\n';
    }
    ELFShdrTy NewSection;
    NewSection.sh_name = SHStrTab.getOffset(Section.getName());
    NewSection.sh_type = Section.getELFType();
    NewSection.sh_addr = 0;
    NewSection.sh_offset = Section.getFileOffset();
    NewSection.sh_size = Section.getOutputSize();
    NewSection.sh_entsize = 0;
    NewSection.sh_flags = Section.getELFFlags();
    NewSection.sh_link = 0;
    NewSection.sh_info = 0;
    NewSection.sh_addralign = Section.getAlignment();
    OutputSections->emplace_back(NewSection);
  }

  return NewSectionIndex;
}

// Rewrite section header table inserting new entries as needed. The sections
// header table size itself may affect the offsets of other sections,
// so we are placing it at the end of the binary.
//
// As we rewrite entries we need to track how many sections were inserted
// as it changes the sh_link value. We map old indices to new ones for
// existing sections.
//
// The following are assumptions about file modifications:
//    * There are no modifications done to address and/or size of existing
//      allocatable sections.
//    * All new allocatable sections are written immediately after existing
//      allocatable sections.
//    * There could be modifications done to non-allocatable sections, e.g.
//      size could be increased.
//    * New non-allocatable sections are added to the end of the file.
template <typename ELFT>
void RewriteInstance::patchELFSectionHeaderTable(ELFObjectFile<ELFT> *File) {
  using Elf_Shdr = typename ELFObjectFile<ELFT>::Elf_Shdr;
  std::vector<Elf_Shdr> OutputSections;
  auto &OS = Out->os();
  auto *Obj = File->getELFFile();

  auto NewSectionIndex = getOutputSections(File, &OutputSections);

  // Sort sections by their offset prior to writing. Only newly created sections
  // were unsorted, hence this wouldn't ruin indices in NewSectionIndex.
  std::stable_sort(OutputSections.begin(), OutputSections.end(),
      [] (Elf_Shdr A, Elf_Shdr B) {
        return A.sh_offset < B.sh_offset;
      });

  DEBUG(
    dbgs() << "BOLT-DEBUG: old to new section index mapping:\n";
    for (uint64_t I = 0; I < NewSectionIndex.size(); ++I) {
      dbgs() << "  " << I << " -> " << NewSectionIndex[I] << '\n';
    }
  );

  // Align starting address for section header table.
  auto SHTOffset = OS.tell();
  SHTOffset = appendPadding(OS, SHTOffset, sizeof(Elf_Shdr));

  // Write all section header entries while patching section references.
  for (uint64_t Index = 0; Index < OutputSections.size(); ++Index) {
    auto &Section = OutputSections[Index];
    Section.sh_link = NewSectionIndex[Section.sh_link];
    if (Section.sh_type == ELF::SHT_REL || Section.sh_type == ELF::SHT_RELA) {
      if (Section.sh_info)
        Section.sh_info = NewSectionIndex[Section.sh_info];
    }
    OS.write(reinterpret_cast<const char *>(&Section), sizeof(Section));
  }

  // Fix ELF header.
  auto NewEhdr = *Obj->getHeader();

  if (BC->HasRelocations) {
    NewEhdr.e_entry = getNewFunctionAddress(NewEhdr.e_entry);
    assert(NewEhdr.e_entry && "cannot find new address for entry point");
  }
  NewEhdr.e_phoff = PHDRTableOffset;
  NewEhdr.e_phnum = Phnum;
  NewEhdr.e_shoff = SHTOffset;
  NewEhdr.e_shnum = OutputSections.size();
  NewEhdr.e_shstrndx = NewSectionIndex[NewEhdr.e_shstrndx];
  OS.pwrite(reinterpret_cast<const char *>(&NewEhdr), sizeof(NewEhdr), 0);
}

template <typename ELFT>
void RewriteInstance::patchELFSymTabs(ELFObjectFile<ELFT> *File) {
  auto *Obj = File->getELFFile();
  // Set pointer at the end of the output file, so we can pwrite old symbol
  // tables if we need to.
  uint64_t NextAvailableOffset = getFileOffsetForAddress(NextAvailableAddress);
  assert(NextAvailableOffset >= FirstNonAllocatableOffset &&
         "next available offset calculation failure");
  Out->os().seek(NextAvailableOffset);

  using Elf_Shdr = typename ELFObjectFile<ELFT>::Elf_Shdr;
  using Elf_Sym  = typename ELFObjectFile<ELFT>::Elf_Sym;

  // Compute a preview of how section indices will change after rewriting, so
  // we can properly update the symbol table.
  std::map<std::string, uint32_t> SectionNameMap;
  auto NewSectionIndex =
    getOutputSections(File, (std::vector<Elf_Shdr> *)nullptr, &SectionNameMap);

  DEBUG(dbgs() << "BOLT-DEBUG: SectionNameMap:\n";
        for (auto &Entry : SectionNameMap) {
          dbgs() << "BOLT-DEBUG: " << Entry.first << " -> "
                 << Entry.second << "\n";
        });

  auto updateSymbolTable =
    [&](bool PatchExisting,
        const Elf_Shdr *Section,
        std::function<void(size_t, const char *, size_t)>
        Write,
        std::function<size_t(StringRef)> AddToStrTab) {
    auto StringSection = cantFail(Obj->getStringTableForSymtab(*Section));
    unsigned IsHotTextUpdated = 0;
    unsigned IsHotDataUpdated = 0;

    std::map<const BinaryFunction *, uint64_t> IslandSizes;
    auto getConstantIslandSize = [&IslandSizes](const BinaryFunction *BF) {
      auto Itr = IslandSizes.find(BF);
      if (Itr != IslandSizes.end())
        return Itr->second;
      return IslandSizes[BF] = BF->estimateConstantIslandSize();
    };

    for (const Elf_Sym &Symbol : cantFail(Obj->symbols(Section))) {
      auto NewSymbol = Symbol;
      const auto *Function = getBinaryFunctionAtAddress(Symbol.st_value);
      // Some section symbols may be mistakenly associated with the first
      // function emitted in the section. Dismiss if it is a section symbol.
      if (Function &&
          !Function->getPLTSymbol() &&
          NewSymbol.getType() != ELF::STT_SECTION) {
        NewSymbol.st_value = Function->getOutputAddress();
        NewSymbol.st_size = Function->getOutputSize();
        if (BC->HasRelocations)
          NewSymbol.st_shndx = NewTextSectionIndex;
        else
          NewSymbol.st_shndx = NewSectionIndex[NewSymbol.st_shndx];
        if (!PatchExisting && Function->isSplit()) {
          auto NewColdSym = NewSymbol;
          SmallVector<char, 256> Buf;
          NewColdSym.st_name =
              AddToStrTab(Twine(cantFail(Symbol.getName(StringSection)))
                              .concat(".cold.0")
                              .toStringRef(Buf));
          NewColdSym.st_value = Function->cold().getAddress();
          NewColdSym.st_size = Function->cold().getImageSize();
          Write(0, reinterpret_cast<const char *>(&NewColdSym),
                sizeof(NewColdSym));
        }
        if (!PatchExisting && Function->hasConstantIsland()) {
          auto DataMark = Function->getOutputDataAddress();
          auto CISize = getConstantIslandSize(Function);
          auto CodeMark = DataMark + CISize;
          auto DataMarkSym = NewSymbol;
          DataMarkSym.st_name = AddToStrTab("$d");
          DataMarkSym.st_value = DataMark;
          DataMarkSym.st_size = 0;
          DataMarkSym.setType(ELF::STT_NOTYPE);
          DataMarkSym.setBinding(ELF::STB_LOCAL);
          auto CodeMarkSym = DataMarkSym;
          CodeMarkSym.st_name = AddToStrTab("$x");
          CodeMarkSym.st_value = CodeMark;
          Write(0, reinterpret_cast<const char *>(&DataMarkSym),
                sizeof(DataMarkSym));
          Write(0, reinterpret_cast<const char *>(&CodeMarkSym),
                sizeof(CodeMarkSym));
        }
        if (!PatchExisting && Function->hasConstantIsland() &&
            Function->isSplit()) {
          auto DataMark = Function->getOutputColdDataAddress();
          auto CISize = getConstantIslandSize(Function);
          auto CodeMark = DataMark + CISize;
          auto DataMarkSym = NewSymbol;
          DataMarkSym.st_name = AddToStrTab("$d");
          DataMarkSym.st_value = DataMark;
          DataMarkSym.st_size = 0;
          DataMarkSym.setType(ELF::STT_NOTYPE);
          DataMarkSym.setBinding(ELF::STB_LOCAL);
          auto CodeMarkSym = DataMarkSym;
          CodeMarkSym.st_name = AddToStrTab("$x");
          CodeMarkSym.st_value = CodeMark;
          Write(0, reinterpret_cast<const char *>(&DataMarkSym),
                sizeof(DataMarkSym));
          Write(0, reinterpret_cast<const char *>(&CodeMarkSym),
                sizeof(CodeMarkSym));
        }
      } else {
        uint32_t OldSectionIndex = NewSymbol.st_shndx;
        auto *BD = !Function ? BC->getBinaryDataAtAddress(NewSymbol.st_value)
                             : nullptr;
        if (BD && BD->isMoved() && !BD->isJumpTable()) {
          assert((!BD->getSize() ||
                  !NewSymbol.st_size ||
                  NewSymbol.st_size == BD->getSize()) &&
                 "sizes must match");

          auto &OutputSection = BD->getOutputSection();

          assert(SectionNameMap.count(OutputSection.getName()));
          DEBUG(dbgs() << "BOLT-DEBUG: moving " << BD->getName() << " from "
                       << *BC->getSectionNameForAddress(NewSymbol.st_value)
                       << " (" << NewSymbol.st_shndx << ") to "
                       << OutputSection.getName() << " ("
                       << SectionNameMap[OutputSection.getName()] << ")\n");
          OldSectionIndex = ELF::SHN_LORESERVE;
          NewSymbol.st_shndx = SectionNameMap[OutputSection.getName()];

          // TODO: use getNewValueForSymbol()?
          NewSymbol.st_value = BD->getOutputAddress();
        }

        if (OldSectionIndex < ELF::SHN_LORESERVE) {
          NewSymbol.st_shndx = NewSectionIndex[OldSectionIndex];
        }

        // Detect local syms in the text section that we didn't update
        // and were preserved by the linker to support relocations against
        // .text (t15274167). Remove then from the symtab.
        if (NewSymbol.getType() == ELF::STT_NOTYPE &&
            NewSymbol.getBinding() == ELF::STB_LOCAL &&
            NewSymbol.st_size == 0) {
          auto ExpectedSec = File->getELFFile()->getSection(NewSymbol.st_shndx);
          if (ExpectedSec) {
            auto Section = *ExpectedSec;
            if (Section->sh_type == ELF::SHT_PROGBITS &&
                Section->sh_flags & ELF::SHF_ALLOC &&
                Section->sh_flags & ELF::SHF_EXECINSTR) {
              // This will cause the symbol to not be emitted if we are
              // creating a new symtab from scratch instead of patching one.
              if (!PatchExisting)
                continue;
              // If patching an existing symtab, patch this value to zero.
              NewSymbol.st_value = 0;
            }
          } else {
            consumeError(ExpectedSec.takeError());
          }
        }
      }

      auto SymbolName = Symbol.getName(StringSection);
      assert(SymbolName && "cannot get symbol name");

      auto updateSymbolValue = [&](const StringRef Name, unsigned &IsUpdated) {
        NewSymbol.st_value = getNewValueForSymbol(Name);
        NewSymbol.st_shndx = ELF::SHN_ABS;
        outs() << "BOLT-INFO: setting " << Name << " to 0x"
               << Twine::utohexstr(NewSymbol.st_value) << '\n';
        ++IsUpdated;
        return true;
      };

      if (opts::HotText && (*SymbolName == "__hot_start" ||
                            *SymbolName == "__hot_end"))
        updateSymbolValue(*SymbolName, IsHotTextUpdated);

      if (opts::HotData && (*SymbolName == "__hot_data_start" ||
                            *SymbolName == "__hot_data_end"))
        updateSymbolValue(*SymbolName, IsHotDataUpdated);

      if (opts::UpdateEnd && *SymbolName == "_end") {
        NewSymbol.st_value = getNewValueForSymbol(*SymbolName);
        NewSymbol.st_shndx = ELF::SHN_ABS;
        outs() << "BOLT-INFO: setting " << *SymbolName << " to 0x"
               << Twine::utohexstr(NewSymbol.st_value) << '\n';
      }

      Write((&Symbol - cantFail(Obj->symbols(Section)).begin()) *
                sizeof(Elf_Sym),
            reinterpret_cast<const char *>(&NewSymbol), sizeof(NewSymbol));
    }

    assert((!IsHotTextUpdated || IsHotTextUpdated == 2) &&
           "either none or both __hot_start/__hot_end symbols were expected");
    assert((!IsHotDataUpdated || IsHotDataUpdated == 2) &&
           "either none or both __hot_data_start/__hot_data_end symbols were expected");

    auto addSymbol = [&](const std::string &Name) {
      Elf_Sym Symbol;
      Symbol.st_value = getNewValueForSymbol(Name);
      Symbol.st_shndx = ELF::SHN_ABS;
      Symbol.st_name = AddToStrTab(Name);
      Symbol.st_size = 0;
      Symbol.st_other = 0;
      Symbol.setBindingAndType(ELF::STB_WEAK, ELF::STT_NOTYPE);

      outs() << "BOLT-INFO: setting " << Name << " to 0x"
             << Twine::utohexstr(Symbol.st_value) << '\n';

      Write(0, reinterpret_cast<const char *>(&Symbol), sizeof(Symbol));
    };

    if (opts::HotText && !IsHotTextUpdated && !PatchExisting) {
      addSymbol("__hot_start");
      addSymbol("__hot_end");
    }

    if (opts::HotData && !IsHotDataUpdated && !PatchExisting) {
      addSymbol("__hot_data_start");
      addSymbol("__hot_data_end");
    }
  };

  // Update dynamic symbol table.
  const Elf_Shdr *DynSymSection = nullptr;
  for (const Elf_Shdr &Section : cantFail(Obj->sections())) {
    if (Section.sh_type == ELF::SHT_DYNSYM) {
      DynSymSection = &Section;
      break;
    }
  }
  assert(DynSymSection && "no dynamic symbol table found");
  updateSymbolTable(/*patch existing table?*/ true,
                    DynSymSection,
                    [&](size_t Offset, const char *Buf, size_t Size) {
                      Out->os().pwrite(Buf, Size,
                                       DynSymSection->sh_offset + Offset);
                    },
                    [](StringRef) -> size_t { return 0; });

  // (re)create regular symbol table.
  const Elf_Shdr *SymTabSection = nullptr;
  for (const auto &Section : cantFail(Obj->sections())) {
    if (Section.sh_type == ELF::SHT_SYMTAB) {
      SymTabSection = &Section;
      break;
    }
  }
  if (!SymTabSection) {
    errs() << "BOLT-WARNING: no symbol table found\n";
    return;
  }

  const Elf_Shdr *StrTabSection =
      cantFail(Obj->getSection(SymTabSection->sh_link));
  std::string NewContents;
  std::string NewStrTab =
      File->getData().substr(StrTabSection->sh_offset, StrTabSection->sh_size);
  auto SecName = cantFail(Obj->getSectionName(SymTabSection));
  auto StrSecName = cantFail(Obj->getSectionName(StrTabSection));

  updateSymbolTable(/*patch existing table?*/ false,
                    SymTabSection,
                    [&](size_t Offset, const char *Buf, size_t Size) {
                      NewContents.append(Buf, Size);
                    },
                    [&](StringRef Str) {
                      size_t Idx = NewStrTab.size();
                      NewStrTab.append(Str.data(), Str.size());
                      NewStrTab.append(1, '\0');
                      return Idx;
                    });

  BC->registerOrUpdateNoteSection(SecName,
                                  copyByteArray(NewContents),
                                  NewContents.size(),
                                  /*Alignment=*/1,
                                  /*IsReadOnly=*/true,
                                  ELF::SHT_SYMTAB);

  BC->registerOrUpdateNoteSection(StrSecName,
                                  copyByteArray(NewStrTab),
                                  NewStrTab.size(),
                                  /*Alignment=*/1,
                                  /*IsReadOnly=*/true,
                                  ELF::SHT_STRTAB);
}

template <typename ELFT>
void RewriteInstance::patchELFRelaPLT(ELFObjectFile<ELFT> *File) {
  auto &OS = Out->os();

  if (!RelaPLTSection) {
    errs() << "BOLT-INFO: no .rela.plt section found\n";
    return;
  }

  for (const auto &Rel : RelaPLTSection->getSectionRef().relocations()) {
    if (Rel.getType() == ELF::R_X86_64_IRELATIVE) {
      DataRefImpl DRI = Rel.getRawDataRefImpl();
      const auto *RelA = File->getRela(DRI);
      auto Address = RelA->r_addend;
      auto NewAddress = getNewFunctionAddress(Address);
      DEBUG(dbgs() << "BOLT-DEBUG: patching IRELATIVE .rela.plt entry 0x"
                   << Twine::utohexstr(Address) << " with 0x"
                   << Twine::utohexstr(NewAddress) << '\n');
      auto NewRelA = *RelA;
      NewRelA.r_addend = NewAddress;
      OS.pwrite(reinterpret_cast<const char *>(&NewRelA), sizeof(NewRelA),
        reinterpret_cast<const char *>(RelA) - File->getData().data());
    }
  }
}

template <typename ELFT>
void RewriteInstance::patchELFGOT(ELFObjectFile<ELFT> *File) {
  auto &OS = Out->os();

  SectionRef GOTSection;
  for (const auto &Section : File->sections()) {
    StringRef SectionName;
    Section.getName(SectionName);
    if (SectionName == ".got") {
      GOTSection = Section;
      break;
    }
  }
  if (!GOTSection.getObject()) {
    errs() << "BOLT-INFO: no .got section found\n";
    return;
  }

  StringRef GOTContents;
  GOTSection.getContents(GOTContents);
  for (const uint64_t *GOTEntry =
        reinterpret_cast<const uint64_t *>(GOTContents.data());
       GOTEntry < reinterpret_cast<const uint64_t *>(GOTContents.data() +
                                                            GOTContents.size());
       ++GOTEntry) {
    if (auto NewAddress = getNewFunctionAddress(*GOTEntry)) {
      DEBUG(dbgs() << "BOLT-DEBUG: patching GOT entry 0x"
                   << Twine::utohexstr(*GOTEntry) << " with 0x"
                   << Twine::utohexstr(NewAddress) << '\n');
      OS.pwrite(reinterpret_cast<const char *>(&NewAddress), sizeof(NewAddress),
        reinterpret_cast<const char *>(GOTEntry) - File->getData().data());
    }
  }
}

template <typename ELFT>
void RewriteInstance::patchELFDynamic(ELFObjectFile<ELFT> *File) {
  auto *Obj = File->getELFFile();
  auto &OS = Out->os();

  using Elf_Phdr = typename ELFFile<ELFT>::Elf_Phdr;
  using Elf_Dyn  = typename ELFFile<ELFT>::Elf_Dyn;

  // Locate DYNAMIC by looking through program headers.
  uint64_t DynamicOffset = 0;
  const Elf_Phdr *DynamicPhdr = 0;
  for (auto &Phdr : cantFail(Obj->program_headers())) {
    if (Phdr.p_type == ELF::PT_DYNAMIC) {
      DynamicOffset = Phdr.p_offset;
      DynamicPhdr = &Phdr;
      assert(Phdr.p_memsz == Phdr.p_filesz && "dynamic sizes should match");
      break;
    }
  }
  assert(DynamicPhdr && "missing dynamic in ELF binary");

  bool ZNowSet = false;

  // Go through all dynamic entries and patch functions addresses with
  // new ones.
  const Elf_Dyn *DTB = cantFail(Obj->dynamic_table_begin(DynamicPhdr),
                                "error accessing dynamic table");
  const Elf_Dyn *DTE = cantFail(Obj->dynamic_table_end(DynamicPhdr),
                                "error accessing dynamic table");
  for (auto *DE = DTB; DE != DTE; ++DE) {
    auto NewDE = *DE;
    bool ShouldPatch = true;
    switch (DE->getTag()) {
    default:
      ShouldPatch = false;
      break;
    case ELF::DT_INIT:
    case ELF::DT_FINI:
      if (BC->HasRelocations) {
        if (auto NewAddress = getNewFunctionAddress(DE->getPtr())) {
          DEBUG(dbgs() << "BOLT-DEBUG: patching dynamic entry of type "
                       << DE->getTag() << '\n');
          NewDE.d_un.d_ptr = NewAddress;
        }
      }
      break;
    case ELF::DT_FLAGS:
      if (BC->RequiresZNow) {
        NewDE.d_un.d_val |= ELF::DF_BIND_NOW;
        ZNowSet = true;
      }
      break;
    case ELF::DT_FLAGS_1:
      if (BC->RequiresZNow) {
        NewDE.d_un.d_val |= ELF::DF_1_NOW;
        ZNowSet = true;
      }
      break;
    }
    if (ShouldPatch) {
      OS.pwrite(reinterpret_cast<const char *>(&NewDE), sizeof(NewDE),
                DynamicOffset + (DE - DTB) * sizeof(*DE));
    }
  }

  if (BC->RequiresZNow && !ZNowSet) {
    errs() << "BOLT-ERROR: output binary requires immediate relocation "
              "processing which depends on DT_FLAGS or DT_FLAGS_1 presence in "
              ".dynamic. Please re-link the binary with -znow.\n";
    exit(1);
  }
}

uint64_t RewriteInstance::getNewFunctionAddress(uint64_t OldAddress) {
  const auto *Function = getBinaryFunctionAtAddress(OldAddress);
  if (!Function)
    return 0;
  return Function->getOutputAddress();
}

void RewriteInstance::rewriteFile() {
  auto &OS = Out->os();

  // We obtain an asm-specific writer so that we can emit nops in an
  // architecture-specific way at the end of the function.
  auto MCE = BC->TheTarget->createMCCodeEmitter(*BC->MII, *BC->MRI, *BC->Ctx);
  auto MAB =
      BC->TheTarget->createMCAsmBackend(*BC->STI, *BC->MRI, MCTargetOptions());
  std::unique_ptr<MCStreamer> Streamer(BC->TheTarget->createMCObjectStreamer(
      *BC->TheTriple, *BC->Ctx, std::unique_ptr<MCAsmBackend>(MAB), OS,
      std::unique_ptr<MCCodeEmitter>(MCE), *BC->STI,
      /* RelaxAll */ false,
      /*IncrementalLinkerCompatible */ false,
      /* DWARFMustBeAtTheEnd */ false));

  auto &Writer = static_cast<MCObjectStreamer *>(Streamer.get())
                     ->getAssembler()
                     .getWriter();

  // Make sure output stream has enough reserved space, otherwise
  // pwrite() will fail.
  auto Offset = OS.seek(getFileOffsetForAddress(NextAvailableAddress));
  (void)Offset;
  assert(Offset == getFileOffsetForAddress(NextAvailableAddress) &&
         "error resizing output file");

  if (!BC->HasRelocations) {
    // Overwrite functions in the output file.
    uint64_t CountOverwrittenFunctions = 0;
    uint64_t OverwrittenScore = 0;
    for (auto &BFI : BinaryFunctions) {
      auto &Function = BFI.second;

      if (Function.getImageAddress() == 0 || Function.getImageSize() == 0)
        continue;

      if (Function.getImageSize() > Function.getMaxSize()) {
        if (opts::Verbosity >= 1) {
          errs() << "BOLT-WARNING: new function size (0x"
                 << Twine::utohexstr(Function.getImageSize())
                 << ") is larger than maximum allowed size (0x"
                 << Twine::utohexstr(Function.getMaxSize())
                 << ") for function " << Function << '\n';
        }
        FailedAddresses.emplace_back(Function.getAddress());
        continue;
      }

      if (Function.isSplit() && (Function.cold().getImageAddress() == 0 ||
                                 Function.cold().getImageSize() == 0))
        continue;

      OverwrittenScore += Function.getFunctionScore();
      // Overwrite function in the output file.
      if (opts::Verbosity >= 2) {
        outs() << "BOLT: rewriting function \"" << Function << "\"\n";
      }
      OS.pwrite(reinterpret_cast<char *>(Function.getImageAddress()),
                Function.getImageSize(),
                Function.getFileOffset());

      // Write nops at the end of the function.
      auto Pos = OS.tell();
      OS.seek(Function.getFileOffset() + Function.getImageSize());
      MAB->writeNopData(Function.getMaxSize() - Function.getImageSize(),
                        &Writer);
      OS.seek(Pos);

      // Write jump tables if updating in-place.
      if (opts::JumpTables == JTS_BASIC) {
        for (auto &JTI : Function.JumpTables) {
          auto *JT = JTI.second;
          auto &Section = JT->getOutputSection();
          Section.setFileOffset(getFileOffsetForAddress(JT->getAddress()));
          assert(Section.getFileOffset() && "no matching offset in file");
          OS.pwrite(reinterpret_cast<const char*>(Section.getOutputData()),
                    Section.getOutputSize(),
                    Section.getFileOffset());
        }
      }

      if (!Function.isSplit()) {
        ++CountOverwrittenFunctions;
        if (opts::MaxFunctions &&
            CountOverwrittenFunctions == opts::MaxFunctions) {
          outs() << "BOLT: maximum number of functions reached\n";
          break;
        }
        continue;
      }

      // Write cold part
      if (opts::Verbosity >= 2) {
        outs() << "BOLT: rewriting function \"" << Function
               << "\" (cold part)\n";
      }
      OS.pwrite(reinterpret_cast<char*>(Function.cold().getImageAddress()),
                Function.cold().getImageSize(),
                Function.cold().getFileOffset());

      // FIXME: write nops after cold part too.

      ++CountOverwrittenFunctions;
      if (opts::MaxFunctions &&
          CountOverwrittenFunctions == opts::MaxFunctions) {
        outs() << "BOLT: maximum number of functions reached\n";
        break;
      }
    }

    // Print function statistics.
    outs() << "BOLT: " << CountOverwrittenFunctions
           << " out of " << BinaryFunctions.size()
           << " functions were overwritten.\n";
    if (BC->TotalScore != 0) {
      double Coverage = OverwrittenScore / (double) BC->TotalScore * 100.0;
      outs() << format("BOLT: Rewritten functions cover %.2lf", Coverage)
             << "% of the execution count of simple functions of "
                "this binary.\n";
    }
  }

  if (BC->HasRelocations && opts::TrapOldCode) {
    auto SavedPos = OS.tell();
    // Overwrite function body to make sure we never execute these instructions.
    for (auto &BFI : BinaryFunctions) {
      auto &BF = BFI.second;
      if (!BF.getFileOffset())
        continue;
      OS.seek(BF.getFileOffset());
      for (unsigned I = 0; I < BF.getMaxSize(); ++I)
        OS.write((unsigned char)
            Streamer->getContext().getAsmInfo()->getTrapFillValue());
    }
    OS.seek(SavedPos);
  }

  // Write all non-local sections, i.e. those not emitted with the function.
  for (auto &Section : BC->allocatableSections()) {
    if (!Section.isFinalized() || Section.isLocal())
      continue;
    if (opts::Verbosity >= 1) {
      outs() << "BOLT: writing new section " << Section.getName()
             << "\n data at 0x" << Twine::utohexstr(Section.getAllocAddress())
             << "\n of size " << Section.getOutputSize()
             << "\n at offset " << Section.getFileOffset() << '\n';
    }
    OS.pwrite(reinterpret_cast<const char*>(Section.getOutputData()),
              Section.getOutputSize(),
              Section.getFileOffset());
  }

  // If .eh_frame is present create .eh_frame_hdr.
  if (EHFrameSection && EHFrameSection->isFinalized()) {
    writeEHFrameHeader();
  }

  // Patch program header table.
  patchELFPHDRTable();

  // Finalize memory image of section string table.
  finalizeSectionStringTable();

  // Update symbol tables.
  patchELFSymTabs();

  // Copy non-allocatable sections once allocatable part is finished.
  rewriteNoteSections();

  // Patch dynamic section/segment.
  patchELFDynamic();

  if (BC->HasRelocations) {
    patchELFRelaPLT();
    patchELFGOT();
  }

  // Update ELF book-keeping info.
  patchELFSectionHeaderTable();

  if (opts::PrintSections) {
    outs() << "BOLT-INFO: Sections after processing:\n";
    BC->printSections(outs());
  }

  Out->keep();

  // If requested, open again the binary we just wrote to dump its EH Frame
  if (opts::DumpEHFrame) {
    Expected<OwningBinary<Binary>> BinaryOrErr =
        createBinary(opts::OutputFilename);
    if (auto E = BinaryOrErr.takeError())
      report_error(opts::OutputFilename, std::move(E));
    Binary &Binary = *BinaryOrErr.get().getBinary();

    if (auto *E = dyn_cast<ELFObjectFileBase>(&Binary)) {
      auto DwCtx = DWARFContext::create(*E);
      const auto &EHFrame = DwCtx->getEHFrame();
      outs() << "BOLT-INFO: Dumping rewritten .eh_frame\n";
      EHFrame->dump(outs(), &*BC->MRI, NoneType());
    }
  }
}

void RewriteInstance::writeEHFrameHeader() {
  DWARFDebugFrame NewEHFrame(true, EHFrameSection->getFileAddress());
  NewEHFrame.parse(DWARFDataExtractor(EHFrameSection->getOutputContents(),
                                      BC->AsmInfo->isLittleEndian(),
                                      BC->AsmInfo->getCodePointerSize()));

  auto OldEHFrameSection = BC->getUniqueSectionByName(".eh_frame_old");
  assert(OldEHFrameSection && "expected .eh_frame_old to be present");
  DWARFDebugFrame OldEHFrame(true, OldEHFrameSection->getFileAddress());
  OldEHFrame.parse(DWARFDataExtractor(OldEHFrameSection->getOutputContents(),
                                      BC->AsmInfo->isLittleEndian(),
                                      BC->AsmInfo->getCodePointerSize()));

  DEBUG(dbgs() << "BOLT: writing a new .eh_frame_hdr\n");

  NextAvailableAddress =
    appendPadding(Out->os(), NextAvailableAddress, EHFrameHdrAlign);

  const auto EHFrameHdrFileAddress = NextAvailableAddress;
  const auto EHFrameHdrFileOffset =
    getFileOffsetForAddress(NextAvailableAddress);

  auto NewEHFrameHdr =
      CFIRdWrt->generateEHFrameHeader(OldEHFrame,
                                      NewEHFrame,
                                      EHFrameHdrFileAddress,
                                      FailedAddresses);

  assert(Out->os().tell() == EHFrameHdrFileOffset && "offset mismatch");
  Out->os().write(NewEHFrameHdr.data(), NewEHFrameHdr.size());

  const auto Flags = BinarySection::getFlags(/*IsReadOnly=*/true,
                                             /*IsText=*/false,
                                             /*IsAllocatable=*/true);
  auto &EHFrameHdrSec = BC->registerOrUpdateSection(".eh_frame_hdr",
                                                    ELF::SHT_PROGBITS,
                                                    Flags,
                                                    nullptr,
                                                    NewEHFrameHdr.size(),
                                                    /*Alignment=*/1);
  EHFrameHdrSec.setFileOffset(EHFrameHdrFileOffset);
  EHFrameHdrSec.setFileAddress(EHFrameHdrFileAddress);

  NextAvailableAddress += EHFrameHdrSec.getOutputSize();

  // Merge .eh_frame and .eh_frame_old so that gdb can locate all FDEs.
  const auto EHFrameSectionSize = (OldEHFrameSection->getFileAddress() +
                                   OldEHFrameSection->getOutputSize() -
                                   EHFrameSection->getFileAddress());

  EHFrameSection =
    BC->registerOrUpdateSection(".eh_frame",
                                EHFrameSection->getELFType(),
                                EHFrameSection->getELFFlags(),
                                EHFrameSection->getOutputData(),
                                EHFrameSectionSize,
                                EHFrameSection->getAlignment(),
                                EHFrameSection->isLocal());

  BC->deregisterSection(*OldEHFrameSection);

  DEBUG(dbgs() << "BOLT-DEBUG: size of .eh_frame after merge is "
               << EHFrameSection->getOutputSize() << '\n');
}

uint64_t RewriteInstance::getFileOffsetForAddress(uint64_t Address) const {
  // Check if it's possibly part of the new segment.
  if (Address >= NewTextSegmentAddress) {
    return Address - NewTextSegmentAddress + NewTextSegmentOffset;
  }

  // Find an existing segment that matches the address.
  const auto SegmentInfoI = EFMM->SegmentMapInfo.upper_bound(Address);
  if (SegmentInfoI == EFMM->SegmentMapInfo.begin())
    return 0;

  const auto &SegmentInfo = std::prev(SegmentInfoI)->second;
  if (Address < SegmentInfo.Address ||
      Address >= SegmentInfo.Address + SegmentInfo.FileSize)
    return 0;

  return  SegmentInfo.FileOffset + Address - SegmentInfo.Address;
}

bool RewriteInstance::willOverwriteSection(StringRef SectionName) {
  for (auto &OverwriteName : SectionsToOverwrite) {
    if (SectionName == OverwriteName)
      return true;
  }

  auto Section = BC->getUniqueSectionByName(SectionName);
  return Section && Section->isAllocatable() && Section->isFinalized();
}

BinaryFunction *
RewriteInstance::getBinaryFunctionContainingAddress(uint64_t Address,
                                                    bool CheckPastEnd,
                                                    bool UseMaxSize) {
  auto FI = BinaryFunctions.upper_bound(Address);
  if (FI == BinaryFunctions.begin())
    return nullptr;
  --FI;

  const auto UsedSize = UseMaxSize ? FI->second.getMaxSize()
                                   : FI->second.getSize();

  if (Address >= FI->first + UsedSize + (CheckPastEnd ? 1 : 0))
    return nullptr;
  return &FI->second;
}

const BinaryFunction *
RewriteInstance::getBinaryFunctionAtAddress(uint64_t Address) const {
  if (const auto *BD = BC->getBinaryDataAtAddress(Address))
    return BC->getFunctionForSymbol(BD->getSymbol());
  return nullptr;
}

DWARFAddressRangesVector RewriteInstance::translateModuleAddressRanges(
      const DWARFAddressRangesVector &InputRanges) const {
  DWARFAddressRangesVector OutputRanges;

  for (const auto Range : InputRanges) {
    auto BFI = BinaryFunctions.lower_bound(Range.LowPC);
    while (BFI != BinaryFunctions.end()) {
      const auto &Function = BFI->second;
      if (Function.getAddress() >= Range.HighPC)
        break;
      const auto FunctionRanges = Function.getOutputAddressRanges();
      std::move(std::begin(FunctionRanges),
                std::end(FunctionRanges),
                std::back_inserter(OutputRanges));
      std::advance(BFI, 1);
    }
  }

  return OutputRanges;
}
