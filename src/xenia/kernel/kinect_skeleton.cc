/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/kinect_skeleton.h"

#include <chrono>
#include <map>
#include <mutex>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <thread>

#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/kernel/kernel_state.h"
#if !XE_PLATFORM_WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "xenia/kernel/util/object_table.h"
#include "xenia/kernel/xthread.h"
#include "xenia/cpu/processor.h"

DEFINE_int32(kinect_fake_user_index, -1,
             "dwUserIndex/dwEnrollmentIndex given to the fake player (-1 = "
             "unidentified).",
             "Kinect");

DEFINE_bool(kinect_fake_hands_up, false,
            "Fake player holds both hands above the head and waves the right "
            "one (engagement pose).",
            "Kinect");

DEFINE_bool(kinect_live_skeleton, false,
            "Inject joints received from the external pose tracker (UDP "
            "127.0.0.1:--kinect_skeleton_port) into the title's skeleton frames.",
            "Kinect");

DEFINE_int32(kinect_skeleton_port, 5760,
             "UDP port the pose tracker sends skeleton packets to.", "Kinect");

DEFINE_double(kinect_cursor_gain_x, 1.5,
              "Hand cursor sensitivity, horizontal (pointer units per metre).",
              "Kinect");
DEFINE_double(kinect_cursor_gain_y, 2.6,
              "Hand cursor sensitivity, vertical (pointer units per metre).",
              "Kinect");
DEFINE_double(kinect_cursor_center_y, -0.10,
              "Hand height relative to the shoulders (m) that puts the cursor "
              "at mid-screen.",
              "Kinect");

DEFINE_bool(kinect_fake_skeleton, false,
            "Feed the title a synthetic standing player instead of its own "
            "(unreliable) skeleton tracker. Kinect Sports only.",
            "Kinect");

namespace xe {
namespace kernel {

namespace {

// NUI_SKELETON_FRAME from the Kinect SDK headers (360 and Windows share it):
//   LARGE_INTEGER liTimeStamp; DWORD dwFrameNumber; DWORD dwFlags;
//   Vector4 vFloorClipPlane; Vector4 vNormalToGravity;
//   NUI_SKELETON_DATA SkeletonData[6];       // 0x1C0 bytes each
// NUI_SKELETON_DATA:
//   DWORD eTrackingState (0 none, 1 position only, 2 tracked); dwTrackingID;
//   dwEnrollmentIndex; dwUserIndex; Vector4 Position;
//   Vector4 SkeletonPositions[20]; DWORD eSkeletonPositionTrackingState[20]
//   (0 none, 1 inferred, 2 tracked); DWORD dwQualityFlags;
constexpr uint32_t kFrameSize = 0xAB0;
constexpr uint32_t kSkeletonSize = 0x1C0;
constexpr uint32_t kSkeletonCount = 6;
constexpr uint32_t kJointCount = 20;
constexpr uint32_t kNoData = 0x8007048F;  // E_NUI_FRAME_NO_DATA
constexpr uint32_t kFramePeriodMs = 33;

// Kinect Sports (4D5308C9), NuiSkeletonGetNextFrame(timeout_ms, frame*).
constexpr uint32_t kKinectSportsTitleId = 0x4D5308C9;
constexpr uint32_t kKinectSportsGetNextFrame = 0x82A0B4B0;

struct Vec4 {
  float x, y, z, w;
};

struct Skeleton {
  uint32_t tracking_state = 0;
  uint32_t tracking_id = 0;
  Vec4 position = {};
  Vec4 joints[kJointCount] = {};
  uint32_t joint_states[kJointCount] = {};
};

// ponytail: one fixed standing player, camera 0.74 m above the floor, 2.2 m
// away. A live source (Mac-side tracker over a socket) replaces this.
void MakeFakeStandingPlayer(Skeleton& s, float sway) {
  constexpr float kCameraHeight = 0.74f;
  constexpr float kDistance = 2.2f;
  struct J {
    float x, height, z;  // x as the player's left (+); sensor space flips it
  };
  const J joints[kJointCount] = {
      {0.00f, 0.95f, 0},      // 0 hip center
      {0.00f, 1.15f, 0},      // 1 spine
      {0.00f, 1.42f, 0},      // 2 shoulder center
      {0.00f, 1.65f, 0},      // 3 head
      {0.18f, 1.40f, 0},      // 4 shoulder left
      {0.22f, 1.12f, 0},      // 5 elbow left
      {0.24f, 0.88f, 0},      // 6 wrist left
      {0.25f, 0.82f, 0},      // 7 hand left
      {-0.18f, 1.40f, 0},     // 8 shoulder right
      {-0.22f, 1.12f, 0},     // 9 elbow right
      {-0.24f, 0.88f, 0},     // 10 wrist right
      {-0.25f, 0.82f, 0},     // 11 hand right
      {0.10f, 0.93f, 0},      // 12 hip left
      {0.11f, 0.50f, 0},      // 13 knee left
      {0.11f, 0.10f, 0},      // 14 ankle left
      {0.11f, 0.03f, -0.08f}, // 15 foot left
      {-0.10f, 0.93f, 0},     // 16 hip right
      {-0.11f, 0.50f, 0},     // 17 knee right
      {-0.11f, 0.10f, 0},     // 18 ankle right
      {-0.11f, 0.03f, -0.08f} // 19 foot right
  };
  s.tracking_state = 2;
  s.tracking_id = 1;
  for (uint32_t i = 0; i < kJointCount; ++i) {
    s.joints[i] = {-joints[i].x + sway, joints[i].height - kCameraHeight,
                   kDistance + joints[i].z, 1.0f};
    s.joint_states[i] = 2;
  }
  if (cvars::kinect_fake_hands_up) {
    // Elbows out at shoulder height, hands above the head; the right hand
    // (-x) waves from side to side.
    const float wave = 0.12f * std::sin(sway * 40.0f);
    const struct {
      uint32_t joint;
      float x, height;
    } raised[] = {
        {5, 0.35f, 1.40f},          // elbow left
        {6, 0.38f, 1.70f},          // wrist left
        {7, 0.38f, 1.85f},          // hand left
        {9, -0.35f, 1.40f},         // elbow right
        {10, -0.38f + wave, 1.70f}, // wrist right
        {11, -0.38f + wave, 1.85f}, // hand right
    };
    for (const auto& r : raised) {
      s.joints[r.joint].x = -r.x + sway;
      s.joints[r.joint].y = r.height - kCameraHeight;
    }
  }
  s.position = s.joints[0];
}

void Put32(uint8_t*& p, uint32_t value) {
  xe::store_and_swap<uint32_t>(p, value);
  p += 4;
}

void PutFloat(uint8_t*& p, float value) {
  xe::store_and_swap<float>(p, value);
  p += 4;
}

void PutVec4(uint8_t*& p, const Vec4& v) {
  PutFloat(p, v.x);
  PutFloat(p, v.y);
  PutFloat(p, v.z);
  PutFloat(p, v.w);
}

void WriteFrame(uint8_t* out, uint32_t frame_number, uint64_t timestamp_100ns,
                const Skeleton* tracked) {
  std::memset(out, 0, kFrameSize);
  uint8_t* p = out;
  Put32(p, uint32_t(timestamp_100ns >> 32));
  Put32(p, uint32_t(timestamp_100ns));
  Put32(p, frame_number);
  Put32(p, 0);                       // dwFlags
  PutVec4(p, {0.0f, 1.0f, 0.0f, 0.74f});  // vFloorClipPlane: y = -height
  PutVec4(p, {0.0f, 1.0f, 0.0f, 0.0f});   // vNormalToGravity
  for (uint32_t i = 0; i < kSkeletonCount; ++i) {
    uint8_t* q = out + 0x30 + i * kSkeletonSize;
    if (!i && tracked) {
      Put32(q, tracked->tracking_state);
      Put32(q, tracked->tracking_id);
      Put32(q, uint32_t(cvars::kinect_fake_user_index));  // dwEnrollmentIndex
      Put32(q, uint32_t(cvars::kinect_fake_user_index));  // dwUserIndex
      PutVec4(q, tracked->position);
      for (uint32_t j = 0; j < kJointCount; ++j) {
        PutVec4(q, tracked->joints[j]);
      }
      for (uint32_t j = 0; j < kJointCount; ++j) {
        Put32(q, tracked->joint_states[j]);
      }
      Put32(q, 0);  // dwQualityFlags
    } else {
      // Untracked slot: state 0, ids 0xFFFFFFFF like the SDK leaves them.
      Put32(q, 0);
      Put32(q, 0);
      Put32(q, 0xFFFFFFFF);
      Put32(q, 0xFFFFFFFF);
    }
  }
}

// Debug aid (XENIA_DUMP_STACKS=1): best-effort PPC stack walk of every guest
// thread, using the saved-LR-at(-8 from caller SP) convention of the 360 ABI.
void DumpGuestThreadStacks(KernelState* kernel_state) {
  Memory* memory = kernel_state->memory();
  auto threads =
      kernel_state->object_table()->GetObjectsByType<XThread>();
  for (auto& thread : threads) {
    if (!thread->thread_state()) continue;
    auto* context = thread->thread_state()->context();
    uint32_t sp = uint32_t(context->r[1]);
    std::string text = fmt::format("  lr {:08X} |", uint32_t(context->lr));
    // Guest stacks have unmapped holes; only touch pages that are mapped.
    auto readable = [&](uint32_t address) {
      xe::BaseHeap* heap = memory->LookupHeap(address);
      uint32_t protect = 0;
      return heap && heap->QueryProtect(address & ~0xFFFu, &protect) &&
             (protect & kMemoryProtectRead);
    };
    for (int depth = 0; depth < 14; ++depth) {
      if (sp < 0x70000000 || sp >= 0x80000000 || (sp & 7) || !readable(sp)) {
        break;
      }
      uint32_t next_sp =
          xe::load_and_swap<uint32_t>(memory->TranslateVirtual(sp));
      if (next_sp <= sp || next_sp >= 0x80000000 || !readable(next_sp - 8)) {
        break;
      }
      uint32_t ret =
          xe::load_and_swap<uint32_t>(memory->TranslateVirtual(next_sp - 8));
      text += fmt::format(" {:08X}", ret);
      sp = next_sp;
    }
    XELOGI("Guest thread {:08X} '{}': {}", thread->thread_id(), thread->name(),
           text);
  }
}

// Live skeleton from the external pose tracker, one UDP datagram per frame
// (little endian): u32 'KSKL', u32 sequence, u32 tracked (0/1),
// float joints[20][3] in metres (sensor space, +y up, +z forward, +x to the
// sensor's LEFT), u8 joint_state[20] (0 none, 1 inferred, 2 tracked). 272 bytes.
struct LiveSkeleton {
  std::mutex mutex;
  Skeleton skeleton;
  bool tracked = false;
  std::chrono::steady_clock::time_point received;
};
LiveSkeleton g_live;

#if !XE_PLATFORM_WIN32
void LiveSkeletonThread() {
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(uint16_t(cvars::kinect_skeleton_port));
  if (sock < 0 || bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))) {
    XELOGW("Kinect live skeleton: cannot bind UDP port {}",
           cvars::kinect_skeleton_port);
    return;
  }
  XELOGI("Kinect live skeleton: listening on 127.0.0.1:{}",
         cvars::kinect_skeleton_port);
  uint8_t packet[512];
  uint32_t received = 0;
  for (;;) {
    ssize_t n = recv(sock, packet, sizeof(packet), 0);
    if (n < 272 || std::memcmp(packet, "KSKL", 4)) continue;
    LiveSkeleton& live = g_live;
    std::lock_guard<std::mutex> lock(live.mutex);
    uint32_t tracked;
    std::memcpy(&tracked, packet + 8, 4);
    live.tracked = tracked != 0;
    const float* joints = reinterpret_cast<const float*>(packet + 12);
    for (uint32_t j = 0; j < kJointCount; ++j) {
      live.skeleton.joints[j] = {joints[j * 3], joints[j * 3 + 1],
                                 joints[j * 3 + 2], 1.0f};
      live.skeleton.joint_states[j] = packet[12 + 240 + j];
    }
    live.skeleton.position = live.skeleton.joints[0];
    live.skeleton.tracking_state = 2;
    live.skeleton.tracking_id = 1;
    live.received = std::chrono::steady_clock::now();
    if (++received == 1 || received % 300 == 0) {
      XELOGI("Kinect live skeleton: {} packets, tracked={}, hip ({}, {}, {})",
             received, live.tracked, joints[0], joints[1], joints[2]);
    }
  }
}
#else
void LiveSkeletonThread() {}  // ponytail: Windows not wired up
#endif

// Latest live skeleton if the tracker reported a player in the last 300 ms.
bool GetLiveSkeleton(Skeleton& out) {
  std::lock_guard<std::mutex> lock(g_live.mutex);
  if (!g_live.tracked ||
      std::chrono::steady_clock::now() - g_live.received >
          std::chrono::milliseconds(300)) {
    return false;
  }
  out = g_live.skeleton;
  return true;
}

// Debug: caller histogram for the wrapper's frame accessors (guest LR -> count).
std::map<std::pair<uint32_t, uint32_t>, uint32_t> g_accessor_callers;
std::mutex g_accessor_mutex;

void NoteAccessorCall(uint32_t accessor, uint32_t lr) {
  std::lock_guard<std::mutex> lock(g_accessor_mutex);
  uint32_t& count = g_accessor_callers[{accessor, lr}];
  if (++count == 1) {
    XELOGI("Kinect accessor {:08X} first called from {:08X}", accessor, lr);
  }
}

constexpr uint32_t kGlobalFrame = 0x832A1250;  // wrapper's current NUI_SKELETON_FRAME

// 0x82F8DBE0: skeleton object method, GetSkeletonData(this, dest): copies the
// object's slot (this[0x2C]) of the global frame, 0x1C0 bytes, to dest.
void GetSkeletonDataHook(cpu::ppc::PPCContext* ppc_context,
                         KernelState* kernel_state) {
  NoteAccessorCall(0x82F8DBE0, uint32_t(ppc_context->lr));
  Memory* memory = kernel_state->memory();
  const uint32_t self = uint32_t(ppc_context->r[3]);
  const uint32_t dest = uint32_t(ppc_context->r[4]);
  if (!dest) return;
  const uint32_t slot =
      xe::load_and_swap<uint32_t>(memory->TranslateVirtual(self + 0x2C));
  if (slot >= kSkeletonCount) return;
  std::memcpy(memory->TranslateVirtual(dest),
              memory->TranslateVirtual(kGlobalFrame + 0x30 + slot * kSkeletonSize),
              kSkeletonSize);
}

// 0x82F8DC18: GetFrameHeader(this, dest): copies the first 0x30 bytes
// (timestamp, frame number, flags, floor clip plane, normal to gravity).
void GetFrameHeaderHook(cpu::ppc::PPCContext* ppc_context,
                        KernelState* kernel_state) {
  NoteAccessorCall(0x82F8DC18, uint32_t(ppc_context->lr));
  Memory* memory = kernel_state->memory();
  const uint32_t dest = uint32_t(ppc_context->r[4]);
  if (!dest) return;
  std::memcpy(memory->TranslateVirtual(dest),
              memory->TranslateVirtual(kGlobalFrame), 0x30);
}

// 0x8251CD88: GetHandPosition(manager, out_xy, player, hand) -> bool. The title
// asks the SDK's interaction library for a hand pointer; no SDK skeleton exists
// here, so compute it from the injected player: the hand's offset from the
// shoulder centre in metres, in the SDK's pointer space where +-0.75 spans the
// screen (x right, y up). Only valid while the hand is above the hips.
void GetHandPositionHook(cpu::ppc::PPCContext* ppc_context,
                         KernelState* kernel_state) {
  Memory* memory = kernel_state->memory();
  const uint32_t out = uint32_t(ppc_context->r[4]);
  const uint32_t player = uint32_t(ppc_context->r[5]);
  const uint32_t hand = uint32_t(ppc_context->r[6]) & 0xFF;
  ppc_context->r[3] = 0;
  if (player != 0 || !out) return;

  Skeleton skeleton;
  if (cvars::kinect_live_skeleton) {
    if (!GetLiveSkeleton(skeleton)) return;
  } else {
    MakeFakeStandingPlayer(skeleton, 0.0f);
  }
  // ponytail: hand index 1 = right hand, 0 = left; swap here if reversed.
  const Vec4& h = skeleton.joints[hand ? 11 : 7];
  const Vec4& shoulders = skeleton.joints[2];
  const Vec4& hips = skeleton.joints[0];
  if (h.y < hips.y) return;  // hand below the hips: no cursor
  float x = std::clamp(float((h.x - shoulders.x) * cvars::kinect_cursor_gain_x),
                       -0.75f, 0.75f);
  float y = std::clamp(
      float((h.y - (shoulders.y + cvars::kinect_cursor_center_y)) *
            cvars::kinect_cursor_gain_y),
      -0.75f, 0.75f);

  // The title's own mapping from pointer space to screen pixels.
  const float width = float(int32_t(
      xe::load_and_swap<uint32_t>(memory->TranslateVirtual(0x832AA920))));
  const float height = float(int32_t(
      xe::load_and_swap<uint32_t>(memory->TranslateVirtual(0x832AA924))));
  uint8_t* dest = memory->TranslateVirtual(out);
  xe::store_and_swap<float>(dest, width * 0.5f * (x * (1.0f / 0.75f) + 1.0f));
  xe::store_and_swap<float>(dest + 4,
                            height * (1.0f - 0.5f * (y * (1.0f / 0.75f) + 1.0f)));
  static uint32_t calls = 0;
  if (++calls == 1 || calls % 600 == 0) {
    XELOGI("Kinect hand cursor: hand {} pointer ({:.2f}, {:.2f}) -> screen "
           "({:.0f}, {:.0f}) of {}x{}",
           hand, x, y, width * 0.5f * (x / 0.75f + 1.0f),
           height * (1.0f - 0.5f * (y / 0.75f + 1.0f)), width, height);
  }
  ppc_context->r[3] = 1;
}

// Call-site hook: runs the title's real NuiSkeletonGetNextFrame (so the SDK's
// skeleton engine keeps draining and keeps estimating the floor), then writes
// the fake player into skeleton slot 0 of whatever frame it returned.
constexpr uint32_t kRealGetNextFrame = 0x82A0B4B0;

void NuiSkeletonGetNextFrameInjectHook(cpu::ppc::PPCContext* ppc_context,
                                       KernelState* kernel_state) {
  const uint32_t frame_address = uint32_t(ppc_context->r[4]);
  uint64_t args[] = {ppc_context->r[3], ppc_context->r[4]};
  auto* thread = XThread::GetCurrentThread();
  const uint64_t result = kernel_state->processor()->Execute(
      thread->thread_state(), kRealGetNextFrame, args, xe::countof(args));
  ppc_context->r[3] = result;
  if (uint32_t(result) != 0) {
    return;  // no frame (E_NUI_FRAME_NO_DATA etc.): pass it through
  }
  static const auto start = std::chrono::steady_clock::now();
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  uint8_t* frame = kernel_state->memory()->TranslateVirtual(frame_address);

  static uint32_t delivered = 0;
  if (++delivered == 1 || delivered % 300 == 0) {
    // Real header written by the SDK: frame number, flags, floor, gravity.
    auto f = [&](uint32_t off) {
      return xe::load_and_swap<float>(frame + off);
    };
    XELOGI(
        "Kinect SDK frame #{}: num {} flags {:X} floor ({}, {}, {}, {}) "
        "gravity ({}, {}, {}) slot0 state {}",
        delivered, xe::load_and_swap<uint32_t>(frame + 8),
        xe::load_and_swap<uint32_t>(frame + 12), f(0x10), f(0x14), f(0x18),
        f(0x1C), f(0x20), f(0x24), f(0x28),
        xe::load_and_swap<uint32_t>(frame + 0x30));
  }

  Skeleton skeleton;
  if (cvars::kinect_live_skeleton) {
    if (!GetLiveSkeleton(skeleton)) return;  // no player: leave the SDK's slot
  } else {
    MakeFakeStandingPlayer(skeleton, 0.03f * float(std::sin(seconds * 1.5)));
  }
  uint8_t* slot = frame + 0x30;
  std::memset(slot, 0, kSkeletonSize);
  uint8_t* q = slot;
  Put32(q, skeleton.tracking_state);
  Put32(q, skeleton.tracking_id);
  Put32(q, uint32_t(cvars::kinect_fake_user_index));
  Put32(q, uint32_t(cvars::kinect_fake_user_index));
  PutVec4(q, skeleton.position);
  for (uint32_t j = 0; j < kJointCount; ++j) PutVec4(q, skeleton.joints[j]);
  for (uint32_t j = 0; j < kJointCount; ++j) Put32(q, skeleton.joint_states[j]);
  Put32(q, 0);  // dwQualityFlags
}

// HRESULT NuiSkeletonGetNextFrame(DWORD timeout_ms, NUI_SKELETON_FRAME* frame)
void NuiSkeletonGetNextFrameHook(cpu::ppc::PPCContext* ppc_context,
                                 KernelState* kernel_state) {
  static auto next_frame_time = std::chrono::steady_clock::now();
  static uint32_t frame_number = 0;
  static const auto start = std::chrono::steady_clock::now();

  const uint32_t timeout_ms = uint32_t(ppc_context->r[3]);
  const uint32_t frame_address = uint32_t(ppc_context->r[4]);

  auto now = std::chrono::steady_clock::now();
  if (now < next_frame_time) {
    // Wait for the next frame like the SDK does; INFINITE is capped so the
    // title's thread can still shut down.
    auto wait = next_frame_time - now;
    auto limit = std::chrono::milliseconds(timeout_ms == 0xFFFFFFFF
                                               ? 1000
                                               : std::min(timeout_ms, 1000u));
    if (wait > limit) {
      std::this_thread::sleep_for(limit);
      ppc_context->r[3] = kNoData;
      return;
    }
    std::this_thread::sleep_for(wait);
  }
  next_frame_time = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(kFramePeriodMs);

  double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  Skeleton skeleton;
  MakeFakeStandingPlayer(skeleton, 0.03f * float(std::sin(seconds * 1.5)));

  uint8_t* frame = kernel_state->memory()->TranslateVirtual(frame_address);
  WriteFrame(frame, ++frame_number, uint64_t(seconds * 1e7), &skeleton);
  static const bool dump_stacks = std::getenv("XENIA_DUMP_STACKS") != nullptr;
  if (dump_stacks && (frame_number == 120 || frame_number == 300)) {
    DumpGuestThreadStacks(kernel_state);
  }
  if (frame_number % 150 == 20 || (frame_number < 90 && frame_number % 10 == 0)) {
    // Debug: how far did the title's NuiSensor wrapper (*0x832A124C) get?
    Memory* memory = kernel_state->memory();
    uint32_t object =
        xe::load_and_swap<uint32_t>(memory->TranslateVirtual(0x832A124C));
    auto word = [&](uint32_t offset) {
      return xe::load_and_swap<uint32_t>(memory->TranslateVirtual(object + offset));
    };
    auto global_word = [&](uint32_t address) {
      return xe::load_and_swap<uint32_t>(memory->TranslateVirtual(address));
    };
    XELOGI("Kinect NUI update gate: 10d0={} 10d4={} 10d8={} 10dc={} 10e0={}",
           global_word(0x832A10D0), global_word(0x832A10D4),
           global_word(0x832A10D8), global_word(0x832A10DC),
           global_word(0x832A10E0));
    XELOGI(
        "Kinect game-level: tracked ids {} {} {} {} {} {} | global frame# {} "
        "state[0] {} id[0] {} enroll[0] {} user[0] {}",
        global_word(0x832A1234), global_word(0x832A1238),
        global_word(0x832A123C), global_word(0x832A1240),
        global_word(0x832A1244), global_word(0x832A1248),
        global_word(0x832A1258), global_word(0x832A1280),
        global_word(0x832A1284), global_word(0x832A1288),
        global_word(0x832A128C));
    XELOGI(
        "Kinect wrapper {:08X}: frame# 2068={} 15b8={} public 58={} "
        "state[0] 15e0={} 15e8={} flags 2b38={:X} 2dd8={} 4={} timeout={:X} "
        "0={} 2b10={} 2b14={} 2b18={} 2b60={} 2b1c={:08X} {:08X} {:08X}",
        object, word(0x2068), word(0x15b8), word(0x58), word(0x15e0),
        word(0x15e8), word(0x2b38), word(0x2dd8), word(4), timeout_ms,
        word(0), word(0x2b10), word(0x2b14), word(0x2b18), word(0x2b60),
        word(0x2b1c), word(0x2b20), word(0x2b24));
  }
  if (frame_number == 1 || frame_number % 300 == 0) {
    XELOGI("Kinect skeleton hook: frame {} delivered to {:08X}", frame_number,
           frame_address);
  }
  ppc_context->r[3] = 0;  // S_OK
}

}  // namespace

static bool InstallGuestHook(KernelState* kernel_state, cpu::Module* module,
                      uint32_t address, const char* name,
                      cpu::GuestFunction::ExternHandler handler) {
  uint8_t* code = kernel_state->memory()->TranslateVirtual(address);
  // Only patch the disc revision this was reverse engineered from: the
  // function must start with the usual `mflr r12` prologue, or (for leaf
  // helpers) simply be code; accept any word for the leaf accessors.
  xe::BaseHeap* heap = kernel_state->memory()->LookupHeap(address);
  uint32_t old_protect = 0;
  if (!heap || !heap->Protect(address, 8,
                              kMemoryProtectRead | kMemoryProtectWrite,
                              &old_protect)) {
    XELOGW("Kinect hook {}: could not unprotect {:08X}", name, address);
    return false;
  }
  // Same recipe as kernel import thunks: `sc 2; blr`, handled by the
  // function's extern handler.
  xe::store_and_swap<uint32_t>(code + 0, 0x44000042);
  xe::store_and_swap<uint32_t>(code + 4, 0x4E800020);
  heap->Protect(address, 8, old_protect);

  cpu::Function* function = nullptr;
  module->DeclareFunction(address, &function);
  function->set_end_address(address + 4);
  function->set_name(std::string("__Hook_") + name);
  static_cast<cpu::GuestFunction*>(function)->SetupExtern(handler, nullptr);
  function->set_status(cpu::Symbol::Status::kDeclared);
  XELOGI("Kinect hook {} installed at {:08X}", name, address);
  return true;
}

void InstallKinectSkeletonHooks(KernelState* kernel_state,
                                cpu::Module* module, uint32_t title_id) {
  if (!(cvars::kinect_fake_skeleton || cvars::kinect_live_skeleton) ||
      title_id != kKinectSportsTitleId) {
    return;
  }
  if (cvars::kinect_live_skeleton) {
    std::thread(LiveSkeletonThread).detach();
  }
  Memory* memory = kernel_state->memory();
  // The skeleton thread calls the SDK as `bl 0x82A0B4B0` at 0x82F8E058.
  constexpr uint32_t kCallSite = 0x82F8E058;
  constexpr uint32_t kExpectedCall = 0x4BA7D459;
  // An import thunk the title never calls (XAudioGetDuckerHoldTime, already
  // `sc 2; blr`) becomes the host stub, in `bl` range of the call site.
  constexpr uint32_t kStubThunk = 0x8317A244;
  uint8_t* site = memory->TranslateVirtual(kCallSite);
  if (xe::load_and_swap<uint32_t>(site) != kExpectedCall) {
    XELOGW("Kinect skeleton hook: unexpected call site at {:08X}, not installing",
           kCallSite);
    return;
  }
  cpu::Function* thunk = nullptr;
  module->DeclareFunction(kStubThunk, &thunk);  // returns the import's function
  if (!thunk) {
    XELOGW("Kinect skeleton hook: stub thunk {:08X} not found", kStubThunk);
    return;
  }
  static_cast<cpu::GuestFunction*>(thunk)->SetupExtern(
      &NuiSkeletonGetNextFrameInjectHook, nullptr);

  xe::BaseHeap* heap = memory->LookupHeap(kCallSite);
  uint32_t old_protect = 0;
  if (!heap || !heap->Protect(kCallSite, 4,
                              kMemoryProtectRead | kMemoryProtectWrite,
                              &old_protect)) {
    XELOGW("Kinect skeleton hook: could not unprotect {:08X}", kCallSite);
    return;
  }
  const int32_t offset = int32_t(kStubThunk - kCallSite);
  xe::store_and_swap<uint32_t>(site, 0x48000001u | (uint32_t(offset) & 0x03FFFFFCu));
  heap->Protect(kCallSite, 4, old_protect);
  XELOGI("Kinect skeleton call-site hook installed at {:08X} -> stub {:08X}",
         kCallSite, kStubThunk);

  // The title asks the (empty) SDK interaction library for the hand pointer.
  InstallGuestHook(kernel_state, module, 0x8251CD88, "GetHandPosition",
                   &GetHandPositionHook);
  // Debug accessors that also log their callers.
  InstallGuestHook(kernel_state, module, 0x82F8DBE0, "GetSkeletonData",
                   &GetSkeletonDataHook);
  InstallGuestHook(kernel_state, module, 0x82F8DC18, "GetFrameHeader",
                   &GetFrameHeaderHook);
}

}  // namespace kernel
}  // namespace xe
