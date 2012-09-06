// Copyright 2012 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Implementation of the code coverage DLL.
#include "syzygy/agent/coverage/coverage.h"

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/environment.h"
#include "base/file_path.h"
#include "base/lazy_instance.h"
#include "base/utf_string_conversions.h"
#include "base/memory/scoped_ptr.h"
#include "sawbuck/common/com_utils.h"
#include "syzygy/agent/common/process_utils.h"
#include "syzygy/agent/common/scoped_last_error_keeper.h"
#include "syzygy/common/logging.h"
#include "syzygy/trace/protocol/call_trace_defs.h"

extern "C" void __declspec(naked) _indirect_penter_dllmain() {
  __asm {
    // Stack: ..., arg0, ret_addr.

    // Stash volatile registers.
    push eax
    push ecx
    push edx
    pushfd

    // Stack: ..., arg0, ret_addr, eax, ecx, edx, fd.

    // Retrieve the address pushed by our caller.
    mov eax, DWORD PTR[esp + 0x10]
    push eax

    // Stack: ..., arg0, ret_addr, eax, ecx, edx, fd, call_addr.

    // Calculate the position of the return address on stack, and
    // push it. This becomes the EntryFrame argument.
    lea eax, DWORD PTR[esp + 0x18]
    push eax

    // Stack: ..., arg0, ret_addr, eax, ecx, edx, fd, call_addr, entry_frame.

    call agent::coverage::Coverage::EntryHook

    // Stack: ..., arg0, ret_addr, eax, ecx, edx, fd.

    // Restore volatile registers.
    popfd
    pop edx
    pop ecx
    pop eax

    // Stack: ..., arg0, ret_addr.

    // Return to the address pushed by our caller.
    ret
  }
}

BOOL WINAPI DllMain(HMODULE instance, DWORD reason, LPVOID reserved) {
  using agent::coverage::Coverage;

  // Our AtExit manager required by base.
  static base::AtExitManager* at_exit;

  switch (reason) {
    case DLL_PROCESS_ATTACH:
      DCHECK(at_exit == NULL);
      at_exit = new base::AtExitManager();

      CommandLine::Init(0, NULL);
      common::InitLoggingForDll(L"coverage");
      LOG(INFO) << "Initialized coverage client library.";
      break;

    case DLL_THREAD_DETACH:
      break;

    case DLL_PROCESS_DETACH:
      DCHECK(at_exit != NULL);
      delete at_exit;
      at_exit = NULL;
      break;

    default:
      break;
  }

  return TRUE;
}

namespace agent {
namespace coverage {

namespace {

using agent::common::ScopedLastErrorKeeper;
using ::common::BasicBlockFrequencyData;
using ::common::kBasicBlockCoverageAgentId;
using ::common::kBasicBlockFrequencySectionName;
using ::common::kBasicBlockFrequencyDataVersion;

// All tracing runs through this object.
base::LazyInstance<agent::coverage::Coverage> static_coverage_instance =
    LAZY_INSTANCE_INITIALIZER;

bool FindCoverageData(const base::win::PEImage& image,
                      BasicBlockFrequencyData** coverage_data) {
  DCHECK(coverage_data != NULL);

  *coverage_data = NULL;

  size_t comparison_length = std::min(
      ::strlen(kBasicBlockFrequencySectionName),
      static_cast<size_t>(IMAGE_SIZEOF_SHORT_NAME));

  size_t section_count = image.GetNTHeaders()->FileHeader.NumberOfSections;
  for (size_t i = 0; i < section_count; ++i) {
    const IMAGE_SECTION_HEADER* section = image.GetSectionHeader(i);
    DCHECK(section != NULL);

    if (::memcmp(section->Name, kBasicBlockFrequencySectionName,
                 comparison_length) == 0 &&
        section->SizeOfRawData >= sizeof(BasicBlockFrequencyData)) {
      if (*coverage_data != NULL) {
        LOG(ERROR) << "Encountered multiple \""
                   << kBasicBlockFrequencySectionName
                   << "\" sections.";
        return false;
      }
      *coverage_data = reinterpret_cast<BasicBlockFrequencyData*>(
          image.RVAToAddr(section->VirtualAddress));
    }
  }

  if (coverage_data == NULL) {
    LOG(ERROR) << "Did not find \""
               << kBasicBlockFrequencySectionName
               << "\" section.";
    return false;
  }

  return true;
}

}  // namespace

Coverage* Coverage::Instance() {
  return static_coverage_instance.Pointer();
}

Coverage::Coverage() {
  scoped_ptr<base::Environment> env(base::Environment::Create());
  std::string id;
  env->GetVar(::kSyzygyRpcInstanceIdEnvVar, &id);
  session_.set_instance_id(UTF8ToWide(id));
  session_.CreateSession(&segment_);
}

Coverage::~Coverage() {
}

void WINAPI Coverage::EntryHook(EntryFrame *entry_frame, FuncAddr function) {
  ScopedLastErrorKeeper scoped_last_error_keeper;

  // Get the address of the module. We do this by querying for the allocation
  // that contains the address of the function we intercepted. This must lie
  // within the instrumented module, and be part of the single allocation in
  // which the image of the module lies. The base of the module will be the
  // base address of the allocation.
  MEMORY_BASIC_INFORMATION mem_info = {};
  if (::VirtualQuery(function, &mem_info, sizeof(mem_info)) == 0) {
    DWORD error = ::GetLastError();
    LOG(ERROR) << "VirtualQuery failed: " << com::LogWe(error) << ".";
    return;
  }
  HMODULE module = reinterpret_cast<HMODULE>(mem_info.AllocationBase);

  // Get the coverage singleton.
  Coverage* coverage = Coverage::Instance();
  DCHECK(coverage != NULL);

  // If the call trace client is not running we simply abort. This is not an
  // error, however, as the instrumented module can still run.
  if (!coverage->session_.IsTracing()) {
    LOG(WARNING) << "Unable to initialize coverage client as we are not "
                 << "tracing.";
    return;
  }

  // Find the section containing the coverage data.
  base::win::PEImage image(module);
  BasicBlockFrequencyData* coverage_data = NULL;
  if (!FindCoverageData(image, &coverage_data))
    return;

  // Prevent repeated initializations. We don't log on this so as to keep the
  // spew down for processes that create lots of threads. The first entry to
  // this is under the loader lock, so we don't need to protect the write.
  // After that we are only ever reading the value.
  if (coverage_data->initialization_attempted != 0)
    return;
  coverage_data->initialization_attempted = 1;

  // Log the module. This is required in order to associate basic-block
  // frequency with a module and PDB file during post-processing.
  if (!agent::common::LogModule(module, &coverage->session_,
                                &coverage->segment_)) {
    LOG(ERROR) << "Failed to log module.";
    return;
  }

  // We immediately flush the segment containing the module data so that it
  // appears prior to the coverage data in the trace file. This makes parsing
  // easier.
  if (!coverage->session_.ReturnBuffer(&coverage->segment_)) {
    LOG(ERROR) << "Failed to return module event buffer.";
    return;
  }

  // Initialize the coverage data for this module.
  if (!coverage->InitializeCoverageData(image, coverage_data)) {
    LOG(ERROR) << "Failed to initialize coverage data.";
    return;
  }

  LOG(INFO) << "Coverage client initialized.";
}

bool Coverage::InitializeCoverageData(const base::win::PEImage& image,
                                      BasicBlockFrequencyData* coverage_data) {
  DCHECK(coverage_data != NULL);

  // We can only handle this if it looks right.
  if (coverage_data->agent_id != kBasicBlockCoverageAgentId ||
      coverage_data->version != kBasicBlockFrequencyDataVersion ||
      coverage_data->frequency_size != 1U) {
    LOG(ERROR) << "Unexpected values in the coverage data structures.";
    return false;
  }

  // Nothing to allocate? We're done!
  if (coverage_data->num_basic_blocks == 0) {
    LOG(WARNING) << "Module contains no instrumented basic blocks, not "
                 << "allocating coverage data segment.";
    return true;
  }

  // Determine the size of the basic block frequency struct.
  size_t bb_freq_size = sizeof(TraceBasicBlockFrequencyData) +
      coverage_data->num_basic_blocks - 1;

  // Determine the size of the buffer we need. We need room for the basic block
  // frequency struct plus a single RecordPrefix header.
  size_t segment_size = bb_freq_size + sizeof(RecordPrefix);

  // Allocate the actual segment for the coverage data.
  trace::client::TraceFileSegment coverage_segment;
  if (!session_.AllocateBuffer(segment_size, &coverage_segment)) {
    LOG(ERROR) << "Failed to allocate coverage data segment.";
    return false;
  }

  // Ensure it's big enough to allocation the basic-block frequency data
  // we want. This automatically accounts for the RecordPrefix overhead.
  if (!coverage_segment.CanAllocate(bb_freq_size)) {
    LOG(ERROR) << "Returned coverage data segment smaller than expected.";
    return false;
  }

  // Allocate the basic-block frequency data. We will leave this allocated and
  // let it get flushed during tear-down of the call-trace client.
  TraceBasicBlockFrequencyData* trace_coverage_data =
      reinterpret_cast<TraceBasicBlockFrequencyData*>(
          coverage_segment.AllocateTraceRecordImpl(
              TRACE_BASIC_BLOCK_FREQUENCY,
              bb_freq_size));
  DCHECK(trace_coverage_data != NULL);

  // Initialize the coverage data struct.
  const IMAGE_NT_HEADERS* nt_headers = image.GetNTHeaders();
  trace_coverage_data->module_base_addr =
      reinterpret_cast<ModuleAddr>(image.module());
  trace_coverage_data->module_base_size =
      nt_headers->OptionalHeader.SizeOfImage;
  trace_coverage_data->module_checksum = nt_headers->OptionalHeader.CheckSum;
  trace_coverage_data->module_time_date_stamp =
      nt_headers->FileHeader.TimeDateStamp;
  trace_coverage_data->frequency_size = 1;
  trace_coverage_data->num_basic_blocks = coverage_data->num_basic_blocks;

  // Hook up the newly allocated buffer to the call-trace instrumentation.
  coverage_data->frequency_data =
      trace_coverage_data->frequency_data;

  return true;
}

}  // namespace coverage
}  // namespace agent