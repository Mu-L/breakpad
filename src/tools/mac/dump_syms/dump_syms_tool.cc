// -*- mode: c++ -*-

// Copyright 2011 Google LLC
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google LLC nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// dump_syms_tool.cc: Command line tool that uses the DumpSymbols class.
// TODO(waylonis): accept stdin

#ifdef HAVE_CONFIG_H
#include <config.h>  // Must come first
#endif

#include <mach-o/arch.h>
#include <unistd.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include "common/mac/dump_syms.h"
#include "common/mac/arch_utilities.h"
#include "common/mac/macho_utilities.h"

using google_breakpad::DumpSymbols;
using google_breakpad::Module;
using std::vector;

struct Options {
  Options() = default;

  string srcPath;
  string dsymPath;
  std::optional<ArchInfo> arch;
  bool header_only = false;
  bool cfi = true;
  bool handle_inter_cu_refs = true;
  bool handle_inlines = false;
  bool enable_multiple = false;
  string module_name;
  bool prefer_extern_name = false;
  bool report_warnings = false;
};

static bool StackFrameEntryComparator(const Module::StackFrameEntry* a,
                                      const Module::StackFrameEntry* b) {
  return a->address < b->address;
}

// Copy the CFI data from |from_module| into |to_module|, for any non-
// overlapping ranges.
static void CopyCFIDataBetweenModules(Module* to_module,
                                      const Module* from_module) {
  typedef vector<Module::StackFrameEntry*>::const_iterator Iterator;

  // Get the CFI data from both the source and destination modules and ensure
  // it is sorted by start address.
  vector<Module::StackFrameEntry*> from_data;
  from_module->GetStackFrameEntries(&from_data);
  std::sort(from_data.begin(), from_data.end(), &StackFrameEntryComparator);

  vector<Module::StackFrameEntry*> to_data;
  to_module->GetStackFrameEntries(&to_data);
  std::sort(to_data.begin(), to_data.end(), &StackFrameEntryComparator);

  Iterator to_it = to_data.begin();

  for (Iterator it = from_data.begin(); it != from_data.end(); ++it) {
    Module::StackFrameEntry* from_entry = *it;
    Module::Address from_entry_end = from_entry->address + from_entry->size;

    // Find the first CFI record in the |to_module| that does not have an
    // address less than the entry to be copied.
    while (to_it != to_data.end()) {
      if (from_entry->address > (*to_it)->address)
        ++to_it;
      else
        break;
    }

    // If the entry does not overlap, then it is safe to copy to |to_module|.
    if (to_it == to_data.end() || (from_entry->address < (*to_it)->address &&
            from_entry_end < (*to_it)->address)) {
      to_module->AddStackFrameEntry(
          std::make_unique<Module::StackFrameEntry>(*from_entry));
    }
  }
}

static bool SetArchitecture(DumpSymbols& dump_symbols,
                            const ArchInfo& arch,
                            const std::string& filename) {
  if (!dump_symbols.SetArchitecture(arch)) {
    fprintf(stderr, "%s: no architecture '%s' is present in file.\n",
            filename.c_str(),
            GetNameFromCPUType(arch.cputype, arch.cpusubtype));
    size_t available_size;
    const SuperFatArch* available =
        dump_symbols.AvailableArchitectures(&available_size);
    if (available_size == 1)
      fprintf(stderr, "the file's architecture is: ");
    else
      fprintf(stderr, "architectures present in the file are:\n");
    for (size_t i = 0; i < available_size; i++) {
      const SuperFatArch* arch = &available[i];
      fprintf(stderr, "%s\n",
              GetNameFromCPUType(arch->cputype, arch->cpusubtype));
    }
    return false;
  }
  return true;
}

static bool Start(const Options& options) {
  SymbolData symbol_data =
      (options.handle_inlines ? INLINES : NO_DATA) |
      (options.cfi ? CFI : NO_DATA) | SYMBOLS_AND_FILES;
  DumpSymbols dump_symbols(symbol_data, options.handle_inter_cu_refs,
                           options.enable_multiple, options.module_name,
                           options.prefer_extern_name);

  // For x86_64 binaries, the CFI data is in the __TEXT,__eh_frame of the
  // Mach-O file, which is not copied into the dSYM. Whereas in i386, the CFI
  // data is in the __DWARF,__debug_frame section, which is moved into the
  // dSYM. Therefore, to get x86_64 CFI data, dump_syms needs to look at both
  // the dSYM and the Mach-O file. If both paths are present and CFI was
  // requested, then consider the Module as "split" and dump all the debug data
  // from the primary debug info file, the dSYM, and then dump additional CFI
  // data from the source Mach-O file.
  bool split_module =
    !options.dsymPath.empty() && !options.srcPath.empty() && options.cfi;
  const string& primary_file =
    split_module ? options.dsymPath : options.srcPath;

  dump_symbols.SetReportWarnings(options.report_warnings);

  if (!dump_symbols.Read(primary_file))
    return false;

  if (options.arch &&
      !SetArchitecture(dump_symbols, *options.arch, primary_file)) {
    return false;
  }

  if (options.header_only)
    return dump_symbols.WriteSymbolFileHeader(std::cout);

  // Read the primary file into a Breakpad Module.
  Module* module = nullptr;
  if (!dump_symbols.ReadSymbolData(&module))
    return false;
  std::unique_ptr<Module> scoped_module(module);

  // If this is a split module, read the secondary Mach-O file, from which the
  // CFI data will be extracted.
  if (split_module && primary_file == options.dsymPath) {
    if (!dump_symbols.Read(options.srcPath))
      return false;

    if (options.arch &&
        !SetArchitecture(dump_symbols, *options.arch, options.srcPath)) {
      return false;
    }
    Module* cfi_module = nullptr;
    if (!dump_symbols.ReadSymbolData(&cfi_module))
      return false;
    std::unique_ptr<Module> scoped_cfi_module(cfi_module);

    bool name_matches;
    if (!options.module_name.empty()) {
      // Ignore the basename of the dSYM and binary and use the passed-in module
      // name.
      name_matches = true;
    } else {
      name_matches = cfi_module->name() == module->name();
    }

    // Ensure that the modules are for the same debug code file.
    if (!name_matches || cfi_module->os() != module->os() ||
        cfi_module->architecture() != module->architecture() ||
        cfi_module->identifier() != module->identifier()) {
      fprintf(stderr, "Cannot generate a symbol file from split sources that do"
                      " not match.\n");
      if (!name_matches) {
        fprintf(stderr, "Name mismatch: binary=[%s], dSYM=[%s]\n",
                cfi_module->name().c_str(), module->name().c_str());
      }
      if (cfi_module->os() != module->os()) {
        fprintf(stderr, "OS mismatch: binary=[%s], dSYM=[%s]\n",
                cfi_module->os().c_str(), module->os().c_str());
      }
      if (cfi_module->architecture() != module->architecture()) {
        fprintf(stderr, "Architecture mismatch: binary=[%s], dSYM=[%s]\n",
                cfi_module->architecture().c_str(),
                module->architecture().c_str());
      }
      if (cfi_module->identifier() != module->identifier()) {
        fprintf(stderr, "Identifier mismatch: binary=[%s], dSYM=[%s]\n",
                cfi_module->identifier().c_str(), module->identifier().c_str());
      }
      return false;
    }

    CopyCFIDataBetweenModules(module, cfi_module);
  }

  return module->Write(std::cout, symbol_data);
}

//=============================================================================
static void Usage(int argc, const char *argv[]) {
  fprintf(stderr, "Output a Breakpad symbol file from a Mach-o file.\n");
  fprintf(stderr,
          "Usage: %s [-a ARCHITECTURE] [-c] [-g dSYM path] "
          "[-n MODULE] [-x] <Mach-o file>\n",
          argv[0]);
  fprintf(stderr, "\t-i: Output module header information only.\n");
  fprintf(stderr, "\t-w: Output warning information.\n");
  fprintf(stderr, "\t-a: Architecture type [default: native, or whatever is\n");
  fprintf(stderr, "\t    in the file, if it contains only one architecture]\n");
  fprintf(stderr, "\t-g: Debug symbol file (dSYM) to dump in addition to the "
                  "Mach-o file\n");
  fprintf(stderr, "\t-c: Do not generate CFI section\n");
  fprintf(stderr, "\t-r: Do not handle inter-compilation unit references\n");
  fprintf(stderr, "\t-d: Generate INLINE and INLINE_ORIGIN records\n");
  fprintf(stderr,
          "\t-m: Enable writing the optional 'm' field on FUNC "
          "and PUBLIC, denoting multiple symbols for the address.\n");
  fprintf(stderr,
          "\t-n: Use MODULE as the name of the module rather than \n"
          "the basename of the Mach-O file/dSYM.\n");
  fprintf(stderr,
          "\t-x: Prefer the PUBLIC (extern) name over the FUNC if\n"
          "they do not match.\n");
  fprintf(stderr, "\t-h: Usage\n");
  fprintf(stderr, "\t-?: Usage\n");
}

//=============================================================================
static void SetupOptions(int argc, const char *argv[], Options *options) {
  extern int optind;
  signed char ch;

  while ((ch = getopt(argc, (char* const*)argv, "iwa:g:crdm?hn:x")) != -1) {
    switch (ch) {
      case 'i':
        options->header_only = true;
        break;
      case 'w':
        options->report_warnings = true;
        break;
      case 'a': {
        std::optional<ArchInfo> arch_info = GetArchInfoFromName(optarg);
        if (!arch_info) {
          fprintf(stderr, "%s: Invalid architecture: %s\n", argv[0], optarg);
          Usage(argc, argv);
          exit(1);
        }
        options->arch = arch_info;
        break;
      }
      case 'g':
        options->dsymPath = optarg;
        break;
      case 'c':
        options->cfi = false;
        break;
      case 'r':
        options->handle_inter_cu_refs = false;
        break;
      case 'd':
        options->handle_inlines = true;
        break;
      case 'm':
        options->enable_multiple = true;
        break;
      case 'n':
        options->module_name = optarg;
        break;
      case 'x':
        options->prefer_extern_name = true;
        break;
      case '?':
      case 'h':
        Usage(argc, argv);
        exit(0);
        break;
    }
  }

  if ((argc - optind) != 1) {
    fprintf(stderr, "Must specify Mach-o file\n");
    Usage(argc, argv);
    exit(1);
  }

  options->srcPath = argv[optind];
}

//=============================================================================
int main (int argc, const char * argv[]) {
  Options options;
  bool result;

  SetupOptions(argc, argv, &options);
  result = Start(options);

  return !result;
}
