#include "rerocc.h"
#include <stdio.h>

enum {
  RR_PHASE2_COMPLETIONS = 5,
  RR_WFI_SCENARIOS = 4,
  RR_EXPECTED_COMPLETIONS = 12,
  RR_COMPLETION_STORAGE = 32,
};

static volatile uint64_t rr_irq_count;
static volatile uint64_t rr_legacy_irq_count;
static volatile uint64_t rr_wake_count;
static volatile uint64_t rr_wfi_wake_count;
static volatile uint64_t rr_async_completion_count;
static volatile uint64_t rr_completion_irq_count;
static volatile uint64_t rr_completion_overflow;
static volatile uint64_t rr_completion_pop_failures;
static volatile uint64_t rr_unexpected_trap;
static volatile uint64_t rr_wfi_entries;
static volatile uint64_t rr_wfi_cycle_before[RR_WFI_SCENARIOS];
static volatile uint64_t rr_wfi_cycle_after[RR_WFI_SCENARIOS];
static volatile uint64_t rr_wfi_cycles;
static struct rr_completion rr_completions[RR_COMPLETION_STORAGE];

static inline uint64_t rr_rdcycle(void) {
  uint64_t value;
  asm volatile("rdcycle %0" : "=r"(value));
  return value;
}

uintptr_t handle_trap(uintptr_t epc, uintptr_t cause, uintptr_t tval,
                      uintptr_t regs[32]) {
  (void)regs;
  if (cause == RR_IRQ_MCAUSE) {
    rr_irq_count++;
    rr_wake_count++;
    if (rr_irq_pending()) {
      rr_legacy_irq_count++;
      rr_irq_ack();
    }
    if (rr_completion_available()) rr_completion_irq_count++;
    for (uint32_t n = 0; n < 16 && rr_completion_available(); n++) {
      struct rr_completion completion;
      if (!rr_completion_pop(&completion)) {
        rr_completion_pop_failures++;
        break;
      }
      if (rr_async_completion_count < RR_COMPLETION_STORAGE)
        rr_completions[rr_async_completion_count] = completion;
      else
        rr_completion_overflow++;
      rr_async_completion_count++;
    }
    return epc;
  }

  (void)epc;
  (void)tval;
  rr_unexpected_trap++;
  return epc;
}

static inline bool rr_wait_for_completion_count(uint32_t target,
                                                uint32_t limit) {
  for (uint32_t spin = 0; spin < limit; spin++) {
    if (read_rr_csr(CSR_RRCOMPLETION_COUNT) >= target) return true;
  }
  return false;
}

static inline bool rr_wait_for_completions(uint64_t target) {
  for (uint64_t spin = 0; spin < 1000000ULL; spin++)
    if (rr_async_completion_count >= target) return true;
  return false;
}

static inline bool rr_wait_for_irqs(uint64_t target) {
  for (uint64_t spin = 0; spin < 1000000ULL; spin++)
    if (rr_irq_count >= target) return true;
  return false;
}

/* WFI is entered with global MIE disabled.  The RoCC bit in mie remains
 * enabled, so Rocket's pending_interrupts clears reg_wfi as soon as the
 * level-triggered FIFO IRQ appears.  Re-enabling MIE immediately after WFI
 * takes the already-pending interrupt into handle_trap(). */
static inline void rr_wfi_wait(uint32_t scenario) {
  if (scenario >= RR_WFI_SCENARIOS) abort();
  rr_wfi_entries++;
  rr_wfi_cycle_before[scenario] = rr_rdcycle();
  asm volatile("wfi" ::: "memory");
  rr_wfi_wake_count++;
  set_csr(mstatus, RR_IRQ_MSTATUS_MIE);
  rr_wfi_cycle_after[scenario] = rr_rdcycle();
  rr_wfi_cycles += rr_wfi_cycle_after[scenario] - rr_wfi_cycle_before[scenario];
}

static bool rr_validate_completions(void) {
  static const uint32_t expected_tokens[RR_EXPECTED_COMPLETIONS] = {
    0xabc00000U, 0xabc00001U, 0xabc00002U, 0xabc00003U, 0xabc00004U,
    0xd0000000U, 0xd0000001U,
    0xd0000010U, 0xd0000011U, 0xd0000012U, 0xd0000013U, 0xd0000014U,
  };
  static const uint32_t expected_cfgs[RR_EXPECTED_COMPLETIONS] = {
    0, 1, 2, 3, 4, 0, 1, 0, 1, 2, 3, 4,
  };
  static const uint32_t expected_managers[RR_EXPECTED_COMPLETIONS] = {
    0, 1, 2, 3, 4, 0, 1, 0, 1, 2, 3, 4,
  };
  bool retired[RR_EXPECTED_COMPLETIONS] = {false};

  if (rr_unexpected_trap || rr_completion_overflow ||
      rr_completion_pop_failures ||
      rr_async_completion_count != RR_EXPECTED_COMPLETIONS)
    return false;
  for (uint64_t i = 0; i < rr_async_completion_count; i++) {
    struct rr_completion *completion = &rr_completions[i];
    bool found = false;
    for (uint32_t j = 0; j < RR_EXPECTED_COMPLETIONS; j++) {
      if (!retired[j] && completion->token == expected_tokens[j]) {
        if (completion->cfg_id != expected_cfgs[j] ||
            completion->manager_id != expected_managers[j] ||
            completion->status != 0) return false;
        retired[j] = true;
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  for (uint32_t i = 0; i < RR_EXPECTED_COMPLETIONS; i++)
    if (!retired[i]) return false;
  return !rr_completion_available();
}

static inline void accum_write(int idx, unsigned long data) {
  ROCC_INSTRUCTION_SS(1, data, idx, 0);
}

static inline unsigned long accum_read(int idx) {
  unsigned long value;
  ROCC_INSTRUCTION_DSS(1, value, 0, idx, 1);
  return value;
}

static inline void accum_load(int idx, void *ptr) {
  asm volatile ("fence");
  ROCC_INSTRUCTION_SS(1, (uintptr_t) ptr, idx, 2);
}

static inline void accum_add(int idx, unsigned long addend) {
  ROCC_INSTRUCTION_SS(1, addend, idx, 3);
}

char test_string[64] __attribute__ ((aligned (64))) = "The quick brown fox jumped over the lazy dog";

static inline unsigned long count_chars(char *start, char needle) {
  unsigned long count;
  ROCC_INSTRUCTION_DSS(2, count, start, needle, 0);
  return count;
}

int accum_test() {
  unsigned long data = 0x3421;
  unsigned long result;

  accum_load(0, &data);
  accum_add(0, 2);
  result = accum_read(0);

  if (result != data + 2)
    return 1;

  accum_write(0, 3);
  accum_add(0, 1);
  result = accum_read(0);

  if (result != 4)
    return 2;

  return 0;
}

int charcount_test() {
  unsigned long count = count_chars(test_string + 14, 'o');
  if (count != 3) return count + 1;
  return 0;
}

int main(void) {
  printf("ReRoCC test\n");

  static uint64_t accum_accels[] = {0, 1, 2, 3};
  static uint64_t charcount_accels[] = {4};

  for (int i = 0; i < 4; i++)
    if (!rr_acquire_multi(i, accum_accels, 4))
      printf("Failed to acquire to cfg %d\n", i);
    else
      printf("Acquired accelerator %ld to cfg %d\n", read_rr_csr(CSR_RRCFG0 + i) & RR_CFG_MGR_MASK, i);

  for (int i = 0; i < 4; i++) rr_release(i);

  for (int i = 0; i < 4; i++)
    if (!rr_acquire_multi(i, accum_accels, 4))
      printf("Failed to acquire to cfg %d\n", i);
    else
      printf("Acquired accelerator %ld to cfg %d\n", read_rr_csr(CSR_RRCFG0 + i) & RR_CFG_MGR_MASK, i);

  int r = 0;

  rr_irq_enable();

  // For each tracker, assign opcode 1 to it, then perform the operation
  // The tracker will automatically forward instructions from the assigned opcode
  // the the accelerator allocated to that tracker
  for (int i = 0; i < 4; i++) {
    rr_set_opc(0x1, i);
    r += accum_test();
    rr_fence(i);
  }

  rr_acquire_multi(4, charcount_accels, 1);
  rr_set_opc(0x2, 4);
  r += charcount_test();
  rr_fence(4);

  // Phase 2 regression: submit completions without waiting for accelerator
  // idle. The bounded CPU work proves that rr_async_end returns immediately.
  volatile uint64_t cpu_progress = 0;
  for (int i = 0; i < 4; i++) {
    rr_set_opc(0x1, i);
    r += accum_test();
    rr_async_end(i, 0xabc00000U + i);
    cpu_progress += (uint64_t)(i + 1);
  }
  rr_set_opc(0x2, 4);
  r += charcount_test();
  rr_async_end(4, 0xabc00004U);
  cpu_progress += 5;

  for (uint32_t spin = 0;
       spin < 1000000 && rr_async_completion_count < RR_PHASE2_COMPLETIONS;
       spin++)
    cpu_progress += spin & 1;

  if (cpu_progress == 0 || rr_async_completion_count != RR_PHASE2_COMPLETIONS)
    return 1;
  if (r != 0 || rr_legacy_irq_count != 5) return 1;
  const uint64_t phase2_irq_count = rr_irq_count;
  const uint64_t completion_irq_base = rr_completion_irq_count;

  /* Scenario A: completion is already in the FIFO before WFI.  Keep global
   * MIE clear while the manager completes so the level remains pending. */
  clear_csr(mstatus, RR_IRQ_MSTATUS_MIE);
  rr_set_opc(0x1, 0);
  accum_add(0, 1);
  rr_async_end(0, 0xd0000000U);
  if (!rr_wait_for_completion_count(1, 1000000)) return 1;
  rr_wfi_wait(0);
  if (!rr_wait_for_completions(6)) return 1;

  /* Scenario B: submit asynchronous accelerator work, do useful CPU work,
   * then enter WFI. The completion wakes the level-triggered RoCC IRQ. */
  clear_csr(mstatus, RR_IRQ_MSTATUS_MIE);
  rr_set_opc(0x1, 1);
  for (uint32_t i = 0; i < 32; i++) accum_add(1, i + 1);
  rr_async_end(1, 0xd0000001U);
  volatile uint64_t useful_cpu_progress = 0;
  for (uint64_t i = 0; i < 4096; i++)
    useful_cpu_progress += (i * 3) + 1;
  if (useful_cpu_progress == 0) return 1;
  rr_wfi_wait(1);
  if (!rr_wait_for_completions(7)) return 1;

  /* Scenario C: five completions are allowed to accumulate while MIE is
   * clear, then one WFI wake drains all pending records in one bounded ISR
   * entry. */
  clear_csr(mstatus, RR_IRQ_MSTATUS_MIE);
  if (rr_completion_available()) return 1;
  for (int i = 0; i < 4; i++) {
    rr_set_opc(0x1, i);
    accum_add(i, 1);
    rr_async_end(i, 0xd0000010U + i);
  }
  rr_set_opc(0x2, 4);
  count_chars(test_string + 14, 'o');
  rr_async_end(4, 0xd0000014U);
  if (!rr_wait_for_completion_count(5, 1000000)) return 1;
  const uint64_t completion_irqs_before_multi = rr_completion_irq_count;
  uint64_t irq_before_multi = rr_irq_count;
  rr_wfi_wait(2);
  if (!rr_wait_for_completions(RR_EXPECTED_COMPLETIONS) ||
      rr_irq_count != irq_before_multi + 1 ||
      rr_completion_irq_count != completion_irqs_before_multi + 1)
    return 1;

  /* Scenario D: create a pending legacy RRIRQ while interrupts are masked,
   * then bound the WFI wake and verify that the wake is not lost. */
  clear_csr(mstatus, RR_IRQ_MSTATUS_MIE);
  if (rr_irq_pending() || rr_completion_available()) return 1;
  const uint64_t irq_before_lost_wakeup = rr_irq_count;
  rr_fence(0);
  bool pending = false;
  for (uint64_t spin = 0; spin < 1000000ULL; spin++) {
    if (rr_irq_pending()) {
      pending = true;
      break;
    }
  }
  if (!pending) return 1;
  rr_wfi_wait(3);
  if (!rr_wait_for_irqs(irq_before_lost_wakeup + 1) ||
      rr_irq_count != irq_before_lost_wakeup + 1 ||
      rr_irq_pending() || rr_completion_available())
    return 1;

  if (rr_wfi_entries != RR_WFI_SCENARIOS ||
      rr_wfi_wake_count != RR_WFI_SCENARIOS ||
      rr_completion_irq_count != completion_irq_base + 3 ||
      rr_irq_count != phase2_irq_count + 4)
    return 1;
  if (!rr_validate_completions()) return 1;

  printf("ReRoCC async completion count = %lu\n",
         (unsigned long)rr_async_completion_count);
  printf("CPU progress after async end = %lu\n", (unsigned long)cpu_progress);
  printf("Useful CPU progress before WFI = %lu\n",
         (unsigned long)useful_cpu_progress);
  printf("WFI entries = %lu\n", (unsigned long)rr_wfi_entries);
  printf("WFI wake count = %lu\n", (unsigned long)rr_wfi_wake_count);
  printf("IRQ handler count = %lu\n", (unsigned long)rr_irq_count);
  printf("IRQ wake count = %lu\n", (unsigned long)rr_wake_count);
  printf("Completion IRQ drains = %lu\n",
         (unsigned long)rr_completion_irq_count);
  printf("WFI cycles: [%lu,%lu] [%lu,%lu] [%lu,%lu] [%lu,%lu] total=%lu\n",
         (unsigned long)rr_wfi_cycle_before[0],
         (unsigned long)rr_wfi_cycle_after[0],
         (unsigned long)rr_wfi_cycle_before[1],
         (unsigned long)rr_wfi_cycle_after[1],
         (unsigned long)rr_wfi_cycle_before[2],
         (unsigned long)rr_wfi_cycle_after[2],
         (unsigned long)rr_wfi_cycle_before[3],
         (unsigned long)rr_wfi_cycle_after[3],
         (unsigned long)rr_wfi_cycles);

  for (int i = 0; i < 16; i++) rr_release(i);

  printf("ReRoCC IRQ count = %lu\n", (unsigned long)rr_irq_count);
  if (r != 0 || rr_legacy_irq_count != 6 || rr_async_completion_count != 12)
    return 1;

  printf("IRQ TEST PASSED\n");
  printf("WFI TEST PASSED\n");
  return 0;
}
