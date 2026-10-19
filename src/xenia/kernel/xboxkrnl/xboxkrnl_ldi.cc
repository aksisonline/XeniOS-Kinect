/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Project. All rights reserved.                         *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// LDI: block-wise LZX decompression exports, used by titles for their own
// compressed asset bundles. The call shapes follow the Microsoft MDI/LDI
// interface (create / decompress one block / reset / destroy); they are not
// documented for the Xbox 360, so the first calls log what the title passes.
//
// Each LDIDecompress call carries one compressed LZX frame (<= 32 KiB output),
// byte aligned on its own, while the window, repeat offsets and Huffman tree
// state carry over from the previous call.

#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>

#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/mutex.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/xbox.h"

#include "third_party/mspack/lzx.h"
#include "third_party/mspack/mspack.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

namespace {

constexpr uint32_t kLdiSuccess = 0;
constexpr uint32_t kLdiError = 1;
constexpr uint32_t kBlockMax = 0x8000;
// Largest compressed frame is the block size plus a little overhead.
constexpr uint32_t kSrcBufferMin = kBlockMax + 6144;
constexpr uint32_t kDefaultWindowBits = 17;

struct MemFile {
  uint8_t* data = nullptr;
  size_t size = 0;
  size_t pos = 0;
};

int MemRead(mspack_file* file, void* buffer, int bytes) {
  auto* f = reinterpret_cast<MemFile*>(file);
  size_t n = std::min<size_t>(bytes, f->size - f->pos);
  std::memcpy(buffer, f->data + f->pos, n);
  f->pos += n;
  return static_cast<int>(n);
}

int MemWrite(mspack_file* file, void* buffer, int bytes) {
  auto* f = reinterpret_cast<MemFile*>(file);
  size_t n = std::min<size_t>(bytes, f->size - f->pos);
  std::memcpy(f->data + f->pos, buffer, n);
  f->pos += n;
  return static_cast<int>(n);
}

void* MemAlloc(mspack_system*, size_t bytes) { return std::calloc(bytes, 1); }
void MemFree(void* ptr) { std::free(ptr); }
void MemCopy(void* src, void* dest, size_t bytes) {
  std::memcpy(dest, src, bytes);
}

struct LdiContext {
  uint32_t window_bits = kDefaultWindowBits;
  mspack_system sys = {};
  MemFile in;
  MemFile out;
  lzxd_stream* lzx = nullptr;
  uint32_t calls = 0;

  LdiContext() {
    sys.read = MemRead;
    sys.write = MemWrite;
    sys.alloc = MemAlloc;
    sys.free = MemFree;
    sys.copy = MemCopy;
  }
  ~LdiContext() { Free(); }

  void Free() {
    if (lzx) {
      lzxd_free(lzx);
      lzx = nullptr;
    }
  }

  // (Re)starts the stream: empty window, default offsets and trees.
  bool Reset() {
    Free();
    lzx = lzxd_init(&sys, reinterpret_cast<mspack_file*>(&in),
                    reinterpret_cast<mspack_file*>(&out), window_bits, 0,
                    kBlockMax, 0, 0);
    return lzx != nullptr;
  }

  // Decodes one frame of `out_bytes` from `src`. Returns an mspack error.
  int Decompress(uint8_t* src, size_t src_bytes, uint8_t* dst,
                 size_t out_bytes) {
    in = {src, src_bytes, 0};
    out = {dst, out_bytes, 0};
    // Every frame starts on a fresh input byte: drop leftover bits/bytes.
    lzx->i_ptr = lzx->i_end = lzx->inbuf;
    lzx->bit_buffer = 0;
    lzx->bits_left = 0;
    lzx->input_end = 0;
    // Bound the decoder to this call so it doesn't start on a next frame.
    lzxd_set_output_length(lzx, lzx->offset + static_cast<off_t>(out_bytes));
    // mspack decodes while frame < (offset + out_bytes) / 32768 + 1. Keep the
    // frame counter one behind that so each call decodes exactly one frame,
    // including a short final one.
    lzx->frame = static_cast<unsigned int>(
        (lzx->offset + static_cast<off_t>(out_bytes)) / 0x8000);
    return lzxd_decompress(lzx, static_cast<off_t>(out_bytes));
  }
};

xe_mutex g_ldi_mutex;
std::map<uint32_t, std::unique_ptr<LdiContext>> g_ldi_contexts;
uint32_t g_next_ldi_handle = 1;

// The configuration struct isn't documented: take the first value that looks
// like a window size (a power of two, or a bit count 15-25).
uint32_t GuessWindowBits(const uint32_t* config_words, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    uint32_t v = xe::byte_swap(config_words[i]);
    if (v >= 15 && v <= 25) {
      return v;
    }
    if (v >= (1u << 15) && v <= (1u << 25) && (v & (v - 1)) == 0) {
      uint32_t bits = 0;
      xe::bit_scan_forward(v, &bits);
      return bits;
    }
  }
  return kDefaultWindowBits;
}

}  // namespace

dword_result_t LDICreateDecompression_entry(
    lpdword_t pcb_data_block_max, lpvoid_t pv_configuration, lpvoid_t pfn_alloc,
    lpvoid_t pfn_free, lpvoid_t pv_memory, lpdword_t pcb_src_buffer_min,
    lpdword_t ph_decompression) {
  // Decoder memory comes from the host, so pv_memory is unused. The title
  // passes its own buffer size in pcb_src_buffer_min and expects at least that
  // back.
  auto context = std::make_unique<LdiContext>();
  if (pv_configuration) {
    auto* words = pv_configuration.as<uint32_t*>();
    context->window_bits = GuessWindowBits(words, 4);
    XELOGI("LDICreateDecompression: block_max_in={:08X} src_min_in={:08X} "
           "config={:08X} {:08X} {:08X} {:08X} -> window_bits={}",
           pcb_data_block_max ? uint32_t(*pcb_data_block_max) : 0,
           pcb_src_buffer_min ? uint32_t(*pcb_src_buffer_min) : 0,
           uint32_t(xe::byte_swap(words[0])), uint32_t(xe::byte_swap(words[1])),
           uint32_t(xe::byte_swap(words[2])), uint32_t(xe::byte_swap(words[3])),
           context->window_bits);
  }
  if (!context->Reset()) {
    return kLdiError;
  }
  if (pcb_data_block_max) {
    *pcb_data_block_max = kBlockMax;
  }
  if (pcb_src_buffer_min && uint32_t(*pcb_src_buffer_min) < kSrcBufferMin) {
    *pcb_src_buffer_min = kSrcBufferMin;
  }
  std::lock_guard<xe_mutex> lock(g_ldi_mutex);
  const uint32_t handle = g_next_ldi_handle++;
  g_ldi_contexts[handle] = std::move(context);
  if (ph_decompression) {
    *ph_decompression = handle;
  }
  return kLdiSuccess;
}
DECLARE_XBOXKRNL_EXPORT1(LDICreateDecompression, kNone, kImplemented);

dword_result_t LDIDecompress_entry(dword_t h_decompression, lpvoid_t pb_src,
                                   dword_t cb_src, lpvoid_t pb_dst,
                                   lpdword_t pcb_decompressed) {
  std::lock_guard<xe_mutex> lock(g_ldi_mutex);
  auto it = g_ldi_contexts.find(h_decompression);
  if (it == g_ldi_contexts.end() || !pb_src || !pb_dst || !pcb_decompressed) {
    return kLdiError;
  }
  LdiContext& context = *it->second;
  // In: the frame's uncompressed size, out: bytes produced.
  uint32_t out_bytes = *pcb_decompressed;
  if (out_bytes == 0 || out_bytes > kBlockMax) {
    out_bytes = kBlockMax;
  }
  if (context.calls++ < 3) {
    XELOGI("LDIDecompress: src={} bytes, dst_size={}", cb_src.value(),
           out_bytes);
  }
  int result = context.Decompress(pb_src.as<uint8_t*>(), cb_src,
                                  pb_dst.as<uint8_t*>(), out_bytes);
  if (result != MSPACK_ERR_OK) {
    XELOGE("LDIDecompress: lzx error {} (call {}, src={}, dst={})", result,
           context.calls, cb_src.value(), out_bytes);
    return kLdiError;
  }
  *pcb_decompressed = out_bytes;
  return kLdiSuccess;
}
DECLARE_XBOXKRNL_EXPORT1(LDIDecompress, kNone, kImplemented);

dword_result_t LDIResetDecompression_entry(dword_t h_decompression) {
  std::lock_guard<xe_mutex> lock(g_ldi_mutex);
  auto it = g_ldi_contexts.find(h_decompression);
  if (it == g_ldi_contexts.end() || !it->second->Reset()) {
    return kLdiError;
  }
  return kLdiSuccess;
}
DECLARE_XBOXKRNL_EXPORT1(LDIResetDecompression, kNone, kImplemented);

dword_result_t LDIDestroyDecompression_entry(dword_t h_decompression) {
  std::lock_guard<xe_mutex> lock(g_ldi_mutex);
  return g_ldi_contexts.erase(h_decompression) ? kLdiSuccess : kLdiError;
}
DECLARE_XBOXKRNL_EXPORT1(LDIDestroyDecompression, kNone, kImplemented);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Ldi);
