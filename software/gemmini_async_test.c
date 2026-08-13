#include "rerocc.h"
#include "include/gemmini.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

enum {
  GEMMINI_CFG = 0,
  GEMMINI_MANAGER = 0,
  GEMMINI_OUTPUTS = 1,
  GEMMINI_MATRIX_DIM = 16,
  GEMMINI_COMPUTE_REPEATS = 1,
  GEMMINI_TOKEN = 0x4d4d0001U,
  SPAD_A = 0,
  SPAD_B = DIM,
  SPAD_C = 2 * DIM,
};

static volatile uint64_t gemmini_irq_count;
static volatile uint64_t gemmini_completion_irq_count;
static volatile uint64_t gemmini_completion_count;
static volatile uint64_t gemmini_wfi_entries;
static volatile uint64_t gemmini_wfi_wakes;
static volatile uint64_t gemmini_wfi_before;
static volatile uint64_t gemmini_wfi_after;
static volatile uint64_t gemmini_unexpected_traps;
static volatile uint64_t gemmini_pop_failures;
static struct rr_completion gemmini_completion;

static inline uint64_t gemmini_rdcycle(void) {
  uint64_t value;
  asm volatile("rdcycle %0" : "=r"(value));
  return value;
}

uintptr_t handle_trap(uintptr_t epc, uintptr_t cause, uintptr_t tval,
                      uintptr_t regs[32]) {
  (void)tval;
  (void)regs;

  if (cause == RR_IRQ_MCAUSE) {
    gemmini_irq_count++;
    const bool completion_irq = rr_completion_available();
    if (completion_irq) gemmini_completion_irq_count++;

    if (completion_irq) {
      struct rr_completion completion;
      rr_completion_pop_data(&completion);
      if (gemmini_completion_count == 0)
        gemmini_completion = completion;
      gemmini_completion_count++;
    }
    return epc;
  }

  gemmini_unexpected_traps++;
  return epc;
}

static inline void gemmini_wfi_wait(void) {
  gemmini_wfi_entries++;
  gemmini_wfi_before = gemmini_rdcycle();
  asm volatile("wfi" ::: "memory");
  gemmini_wfi_wakes++;
  /* Phase 3 established that WFI may wake with MIE clear.  Re-enabling MIE
   * takes the already-pending level interrupt into handle_trap(). */
  set_csr(mstatus, RR_IRQ_MSTATUS_MIE);
  gemmini_wfi_after = gemmini_rdcycle();
}

static void fill_operands(elem_t a[DIM][DIM], elem_t b[DIM][DIM]) {
  for (size_t row = 0; row < GEMMINI_MATRIX_DIM; row++) {
    for (size_t col = 0; col < GEMMINI_MATRIX_DIM; col++) {
      a[row][col] = (elem_t)((row + 2 * col) % 5) - 2;
      b[row][col] = (elem_t)((3 * row + col) % 5) - 2;
    }
  }
}

static bool check_output(const elem_t a[DIM][DIM],
                         const elem_t b[DIM][DIM],
                         elem_t output[GEMMINI_OUTPUTS][DIM][DIM]) {
  for (size_t matrix = 0; matrix < GEMMINI_OUTPUTS; matrix++) {
    for (size_t row = 0; row < GEMMINI_MATRIX_DIM; row++) {
      for (size_t col = 0; col < GEMMINI_MATRIX_DIM; col++) {
        int32_t expected = 0;
        for (size_t k = 0; k < GEMMINI_MATRIX_DIM; k++)
          expected += (int32_t)a[row][k] * (int32_t)b[k][col];
        if (output[matrix][row][col] != (elem_t)expected) {
          printf("Gemmini mismatch matrix=%lu row=%lu col=%lu expected=%ld observed=%d\n",
                 (unsigned long)matrix, (unsigned long)row,
                 (unsigned long)col, (long)expected,
                 (int)output[matrix][row][col]);
          return false;
        }
      }
    }
  }
  return true;
}

int main(void) {
  static elem_t input_a[DIM][DIM] row_align(1);
  static elem_t input_b[DIM][DIM] row_align(1);
  /* The sentinel deliberately brings output lines into Rocket's D$ before
   * Gemmini's uncached TileLink writer publishes the final data. */
  static elem_t output[GEMMINI_OUTPUTS][DIM][DIM] row_align(1);

  fill_operands(input_a, input_b);
  for (size_t matrix = 0; matrix < GEMMINI_OUTPUTS; matrix++)
    for (size_t row = 0; row < GEMMINI_MATRIX_DIM; row++)
      for (size_t col = 0; col < GEMMINI_MATRIX_DIM; col++)
        output[matrix][row][col] = (elem_t)-7;

  if (!rr_acquire_single(GEMMINI_CFG, GEMMINI_MANAGER)) {
    printf("Gemmini acquire FAIL\n");
    return 1;
  }

  /* Gemmini's upstream default config is OpcodeSet.custom3.  The only
   * Rocket RoCC visible in this configuration is the ReRoCC client, so these
   * custom3 instructions can reach Gemmini only through client -> manager. */
  rr_set_opc(XCUSTOM_ACC, GEMMINI_CFG);
  rr_irq_enable();

  /* Publish CPU-owned input/output buffers before forwarding Gemmini DMA. */
  asm volatile("fence rw, rw" ::: "memory");

  gemmini_config_ld(DIM * sizeof(elem_t));
  gemmini_config_st(DIM * sizeof(elem_t));
  gemmini_extended_mvin(input_a, SPAD_A, GEMMINI_MATRIX_DIM,
                        GEMMINI_MATRIX_DIM);
  gemmini_extended_mvin(input_b, SPAD_B, GEMMINI_MATRIX_DIM,
                        GEMMINI_MATRIX_DIM);
  gemmini_config_ex(OUTPUT_STATIONARY, NO_ACTIVATION, 0);

  for (size_t matrix = 0; matrix < GEMMINI_OUTPUTS; matrix++) {
    uint32_t c_spad = SPAD_C + (uint32_t)(matrix * DIM);
    for (size_t repeat = 0; repeat < GEMMINI_COMPUTE_REPEATS; repeat++) {
      /* Each preload resets the accumulator, so the final tile remains A*B
       * while the stream keeps Gemmini executing after rr_async_end(). */
      gemmini_extended_preload(GARBAGE_ADDR, c_spad, GEMMINI_MATRIX_DIM,
                               GEMMINI_MATRIX_DIM, GEMMINI_MATRIX_DIM,
                               GEMMINI_MATRIX_DIM);
      gemmini_extended_compute_preloaded(SPAD_A, SPAD_B, GEMMINI_MATRIX_DIM,
                                         GEMMINI_MATRIX_DIM,
                                         GEMMINI_MATRIX_DIM,
                                         GEMMINI_MATRIX_DIM);
    }
    gemmini_extended_mvout(output[matrix], c_spad, GEMMINI_MATRIX_DIM,
                           GEMMINI_MATRIX_DIM);
  }

  clear_csr(mstatus, RR_IRQ_MSTATUS_MIE);
  const uint64_t token = GEMMINI_TOKEN;
  const uint64_t async_start = gemmini_rdcycle();
  rr_async_end(GEMMINI_CFG, (uint32_t)token);
  const uint64_t async_return = gemmini_rdcycle();

  volatile uint64_t cpu_progress = 0;
  for (uint64_t i = 0; i < 1; i++)
    cpu_progress += (i * 5) + 1;

  const uint64_t completion_count_before_wfi =
      read_rr_csr(CSR_RRCOMPLETION_COUNT);
  if (cpu_progress == 0 || completion_count_before_wfi != 0) {
    printf("Gemmini did not provide WFI-in-flight evidence: progress=%lu fifo=%lu\n",
           (unsigned long)cpu_progress,
           (unsigned long)completion_count_before_wfi);
    return 1;
  }

  gemmini_wfi_wait();

  bool completion_retired = false;
  for (uint64_t spin = 0; spin < 1000000ULL; spin++) {
    if (gemmini_completion_count == 1) {
      completion_retired = true;
      break;
    }
  }
  if (!completion_retired || gemmini_pop_failures ||
      gemmini_unexpected_traps || gemmini_irq_count != 1 ||
      gemmini_completion_irq_count != 1 ||
      gemmini_completion_count != 1 ||
      rr_completion_available()) {
    printf("Gemmini completion delivery FAIL irq=%lu completion_irq=%lu count=%lu\n",
           (unsigned long)gemmini_irq_count,
           (unsigned long)gemmini_completion_irq_count,
           (unsigned long)gemmini_completion_count);
    return 1;
  }

  if (gemmini_completion.status != 0 ||
      gemmini_completion.token != (uint32_t)token ||
      gemmini_completion.cfg_id != GEMMINI_CFG ||
      gemmini_completion.manager_id != GEMMINI_MANAGER) {
    printf("Gemmini completion fields FAIL token=0x%08lx cfg=%lu manager=%lu status=%lu\n",
           (unsigned long)gemmini_completion.token,
           (unsigned long)gemmini_completion.cfg_id,
           (unsigned long)gemmini_completion.manager_id,
           (unsigned long)gemmini_completion.status);
    return 1;
  }

  /* Completion means Gemmini's DMA writer is idle; this fence is the explicit
   * consumer-side publication boundary before Rocket reads the output. */
  asm volatile("fence rw, rw" ::: "memory");
  if (!check_output(input_a, input_b, output)) {
    printf("Gemmini result FAIL\n");
    return 1;
  }

  printf("Gemmini async token = 0x%08lx\n", (unsigned long)token);
  printf("rr_async_end cycles = %lu\n",
         (unsigned long)(async_return - async_start));
  printf("CPU progress before completion = %lu\n",
         (unsigned long)cpu_progress);
  printf("WFI entry count = %lu\n", (unsigned long)gemmini_wfi_entries);
  printf("WFI wake count = %lu\n", (unsigned long)gemmini_wfi_wakes);
  printf("WFI cycles = %lu\n",
         (unsigned long)(gemmini_wfi_after - gemmini_wfi_before));
  printf("Gemmini completion IRQ = %lu\n",
         (unsigned long)gemmini_completion_irq_count);
  printf("Gemmini completion count = %lu\n",
         (unsigned long)gemmini_completion_count);
  printf("Gemmini completion token = 0x%08lx cfg=%lu manager=%lu status=%lu\n",
         (unsigned long)gemmini_completion.token,
         (unsigned long)gemmini_completion.cfg_id,
         (unsigned long)gemmini_completion.manager_id,
         (unsigned long)gemmini_completion.status);
  printf("Gemmini result PASS\n");

  /* Ownership remains with the manager until the response was retired and
   * the result was consumed. */
  rr_release(GEMMINI_CFG);
  printf("ReRoCC Gemmini async TEST PASSED\n");
  return 0;
}
