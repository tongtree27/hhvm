/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
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

#include "hphp/util/managed-arena.h"

#ifdef USE_JEMALLOC_EXTENT_HOOKS

#include <cinttypes>
#include <stdexcept>

namespace HPHP { namespace alloc {

void* g_arenas[MAX_MANAGED_ARENA_COUNT];
static_assert(alignof(HighArena) <= 64, "");
static_assert(alignof(LowHugeArena) <= 64, "");
alignas(64) uint8_t g_highArena[sizeof(HighArena)];
alignas(64) uint8_t g_lowHugeArena[sizeof(LowHugeArena)];

//////////////////////////////////////////////////////////////////////

namespace {
// jemalloc mibs that are frequently used, initialized early when there is only
// one thread.
size_t g_pactive_mib[4];                // "stats.arenas.<i>.pactive"
size_t g_epoch_mib[1];                  // "epoch"
bool g_mib_initialized = false;

void initializeMibs() {
  if (g_mib_initialized) return;
  size_t miblen = 1;
  mallctlnametomib("epoch", g_epoch_mib, &miblen);
  miblen = 4;
  mallctlnametomib("stats.arenas.0.pactive", g_pactive_mib, &miblen);
  g_mib_initialized = true;
}

void mallctl_epoch() {
  if (!g_mib_initialized) return;
  uint64_t epoch = 1;
  mallctlbymib(g_epoch_mib, 1, nullptr, nullptr, &epoch, sizeof(epoch));
}

size_t activeSize(unsigned arenaId) {
  if (!g_mib_initialized) return 0;
  size_t mib[4] =
    {g_pactive_mib[0], g_pactive_mib[1], arenaId, g_pactive_mib[3]};
  size_t pactive = 0;
  size_t sz = sizeof(pactive);
  if (mallctlbymib(mib, 4, &pactive, &sz, nullptr, 0)) return 0;
  auto const activeSize = pactive * s_pageSize;
  return activeSize;
}

}

template<typename ExtentAllocator>
std::string ManagedArena<ExtentAllocator>::reportStats() {
  mallctl_epoch();
  char buffer[128];
  std::snprintf(buffer, sizeof(buffer),
                "Arena %d: capacity %zd, max_capacity %zd, used %zd\n",
                id(),
                ExtentAllocator::mappedSize(),
                ExtentAllocator::maxCapacity(),
                activeSize(id()));
  return std::string{buffer};
}

template<typename ExtentAllocator>
size_t ManagedArena<ExtentAllocator>::unusedSize() {
  mallctl_epoch();
  auto const active = activeSize(id());
  return ExtentAllocator::mappedSize() - active;
}

template<typename ExtentAllocator>
void ManagedArena<ExtentAllocator>::init() {
  size_t idSize = sizeof(m_arenaId);
  if (mallctl("arenas.create", &m_arenaId, &idSize, nullptr, 0)) {
    throw std::runtime_error{"arenas.create"};
  }
  if (m_arenaId >= MAX_MANAGED_ARENA_COUNT) {
    throw std::out_of_range{"too many arenas, check MAX_HUGE_ARENA_COUNT"};
  }
  char command[32];
  std::snprintf(command, sizeof(command), "arena.%d.extent_hooks", m_arenaId);
  extent_hooks_t* hooks_ptr = &ExtentAllocator::s_hooks;
  if (mallctl(command, nullptr, nullptr, &hooks_ptr, sizeof(hooks_ptr))) {
    throw std::runtime_error{command};
  }
  // Turn off purging if we are unable to return memeory to the system.
  if (ExtentAllocator::IsPurgingSupported()) {
    ssize_t decay_time = -1;
    std::snprintf(command, sizeof(command),
                  "arena.%d.dirty_decay_ms", m_arenaId);
    if (mallctl(command, nullptr, nullptr, &decay_time, sizeof(decay_time))) {
      throw std::runtime_error{command};
    }
    std::snprintf(command, sizeof(command),
                  "arena.%d.muzzy_decay_ms", m_arenaId);
    if (mallctl(command, nullptr, nullptr, &decay_time, sizeof(decay_time))) {
      throw std::runtime_error{command};
    }
  }
  assert(m_arenaId < MAX_MANAGED_ARENA_COUNT);
  g_arenas[m_arenaId] = this;
  if (!g_mib_initialized) {
    initializeMibs();
  }
}

template void ManagedArena<BumpExtentAllocator>::init();
template size_t ManagedArena<BumpExtentAllocator>::unusedSize();
template std::string ManagedArena<BumpExtentAllocator>::reportStats();

}}

#endif
