#include "rerocc.h"
#include "include/gemmini.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

enum {
  RR6_STREAMS = 3,
  RR6_DIM = 8,
  RR6_MATRIX_DIM = RR6_DIM,
  RR6_MVIN_REPEATS = 128,
  RR6_COMPUTE_REPEATS = 4096,
  RR6_PREFLIGHT_ROWS = 8,
  RR6_PREFLIGHT_COLS = 64,
  RR6_PREFLIGHT_MVIN_REPEATS = 128,
  RR6_PREFLIGHT_TOKEN = 0x600000f3U,
  RR6_CPU_WORK = 16,
  RR6_TOKEN_BASE = 0x60000000U,
  RR6_COMPLETION_STORAGE = 8,
};

struct rr6_stream {
  uint32_t token;
  uint32_t cfg;
  uint32_t manager;
  uint64_t submit_cycle;
  uint64_t completion_cycle;
  bool retired;
};

static volatile uint64_t rr6_irq_count;
static volatile uint64_t rr6_completion_irq_count;
static volatile uint64_t rr6_completion_count;
static volatile uint64_t rr6_wfi_entries;
static volatile uint64_t rr6_wfi_wakes;
static volatile uint64_t rr6_cpu_progress;
static volatile uint64_t rr6_unexpected_traps;
static volatile uint64_t rr6_duplicate_completions;
static volatile uint64_t rr6_lost_completions;
static volatile uint64_t rr6_bad_completions;
static volatile uint64_t rr6_preflight_wfi_entries;
static volatile uint64_t rr6_preflight_wfi_wakes;
static volatile uint64_t rr6_preflight_completion_cycle;
static volatile bool rr6_preflight_retired;
static volatile uint32_t rr6_completion_order[RR6_COMPLETION_STORAGE];
static volatile uint32_t rr6_completion_tokens[RR6_COMPLETION_STORAGE];
static volatile uint32_t rr6_completion_cfgs[RR6_COMPLETION_STORAGE];
static volatile uint32_t rr6_completion_managers[RR6_COMPLETION_STORAGE];
static struct rr6_stream rr6_streams[RR6_STREAMS] = {
  {RR6_TOKEN_BASE | 0xa0U, 0, 0, 0, 0, false},
  {RR6_TOKEN_BASE | 0xb1U, 1, 1, 0, 0, false},
  {RR6_TOKEN_BASE | 0xc2U, 2, 2, 0, 0, false},
};

static inline uint64_t rr6_rdcycle(void) {
  uint64_t value;
  asm volatile("rdcycle %0" : "=r"(value));
  return value;
}

static int rr6_find_token(uint32_t token) {
  for (int i = 0; i < RR6_STREAMS; i++)
    if (rr6_streams[i].token == token) return i;
  return -1;
}

uintptr_t handle_trap(uintptr_t epc, uintptr_t cause, uintptr_t tval,
                      uintptr_t regs[32]) {
  (void)tval;
  (void)regs;

  if (cause == RR_IRQ_MCAUSE) {
    rr6_irq_count++;
    if (rr_irq_pending()) rr_irq_ack();

    /* One level-sensitive IRQ may represent one or several manager
     * completions.  Drain the client FIFO, preserving token/cfg/manager
     * identity rather than assigning meaning from arrival order. */
    while (rr_completion_available()) {
      struct rr_completion completion;
      rr_completion_pop_data(&completion);
      rr6_completion_irq_count++;
      if (completion.token == RR6_PREFLIGHT_TOKEN) {
        if (rr6_preflight_retired || completion.client_id != 0 ||
            completion.cfg_id != 0 || completion.manager_id != 0 ||
            completion.status != 0) {
          rr6_bad_completions++;
        } else {
          rr6_preflight_retired = true;
          rr6_preflight_completion_cycle = rr6_rdcycle();
        }
        continue;
      }
      const int stream = rr6_find_token(completion.token);
      if (stream < 0 || completion.client_id != 0 ||
          completion.cfg_id != rr6_streams[stream].cfg ||
          completion.manager_id != rr6_streams[stream].manager ||
          completion.status != 0) {
        rr6_bad_completions++;
        continue;
      }
      if (rr6_streams[stream].retired) {
        rr6_duplicate_completions++;
        continue;
      }
      rr6_streams[stream].retired = true;
      rr6_streams[stream].completion_cycle = rr6_rdcycle();
      if (rr6_completion_count < RR6_COMPLETION_STORAGE) {
        rr6_completion_order[rr6_completion_count] = (uint32_t)stream;
        rr6_completion_tokens[rr6_completion_count] = completion.token;
        rr6_completion_cfgs[rr6_completion_count] = completion.cfg_id;
        rr6_completion_managers[rr6_completion_count] = completion.manager_id;
      }
      rr6_completion_count++;
    }
    return epc;
  }

  rr6_unexpected_traps++;
  return epc;
}

static void rr6_fail(const char *message) {
  printf("ReRoCC Phase 5 1R1C3M3G FAIL: %s\n", message);
  abort();
}

static bool rr6_wait_acquired(uint32_t cfg) {
  for (uint64_t spin = 0; spin < 1000000ULL; spin++)
    if ((read_rr_csr(CSR_RRCFG0 + cfg) & RR_CFG_ACQ_MASK) != 0) return true;
  return false;
}

static bool rr6_wait_released(uint32_t cfg) {
  for (uint64_t spin = 0; spin < 1000000ULL; spin++)
    if ((read_rr_csr(CSR_RRCFG0 + cfg) & RR_CFG_ACQ_MASK) == 0) return true;
  return false;
}

static void rr6_fill_operands(uint32_t stream,
                              elem_t a[RR6_MATRIX_DIM][RR6_MATRIX_DIM],
                              elem_t b[RR6_MATRIX_DIM][RR6_MATRIX_DIM],
                              elem_t output[RR6_MATRIX_DIM][RR6_MATRIX_DIM]) {
  for (size_t row = 0; row < RR6_MATRIX_DIM; row++) {
    for (size_t col = 0; col < RR6_MATRIX_DIM; col++) {
      a[row][col] = (elem_t)((row + 2 * col + stream) % 5) - 2;
      b[row][col] = (elem_t)((3 * row + col + 2 * stream) % 5) - 2;
      output[row][col] = (elem_t)0x55;
    }
  }
}

static bool rr6_check_output(uint32_t stream,
                             const elem_t a[RR6_MATRIX_DIM][RR6_MATRIX_DIM],
                             const elem_t b[RR6_MATRIX_DIM][RR6_MATRIX_DIM],
                             const elem_t output[RR6_MATRIX_DIM][RR6_MATRIX_DIM]) {
  for (size_t row = 0; row < RR6_MATRIX_DIM; row++) {
    for (size_t col = 0; col < RR6_MATRIX_DIM; col++) {
      int32_t expected = 0;
      for (size_t k = 0; k < RR6_MATRIX_DIM; k++)
        expected += (int32_t)a[row][k] * (int32_t)b[k][col];
      if (output[row][col] != (elem_t)expected) {
        printf("G%lu mismatch row=%lu col=%lu expected=%ld observed=%d\n",
               (unsigned long)stream, (unsigned long)row,
               (unsigned long)col, (long)expected, (int)output[row][col]);
        return false;
      }
    }
  }
  return true;
}

/* Upstream Gemmini funct encodings are retained; OP selects the ReRoCC
 * client opcode and therefore the cfg -> manager -> Gemmini route. */
#define RR6_GEMMINI_CMD(OP, RS1, RS2, FUNCT) \
  ROCC_INSTRUCTION_0_R_R(OP, (uint64_t)(RS1), (uint64_t)(RS2), FUNCT)
#define RR6_GEMMINI_SPAD(ADDR, ROWS, COLS) \
  (((uint64_t)(ROWS) << (ADDR_LEN + 16)) | \
   ((uint64_t)(COLS) << ADDR_LEN) | (uint64_t)(ADDR))
#define RR6_GEMMINI_CONFIG_LD(OP, STRIDE) \
  RR6_GEMMINI_CMD(OP, \
    ((uint64_t)scale_t_to_scale_t_bits((scale_t)MVIN_SCALE_IDENTITY) << 32) | \
    ((uint64_t)RR6_DIM << 16) | (1ULL << 8) | CONFIG_LD, \
    (STRIDE), k_CONFIG)
#define RR6_GEMMINI_CONFIG_ST(OP, STRIDE) \
  RR6_GEMMINI_CMD(OP, CONFIG_ST, \
    ((uint64_t)acc_scale_t_to_acc_scale_t_bits((acc_scale_t)ACC_SCALE_IDENTITY) << 32) | \
    (uint32_t)(STRIDE), k_CONFIG)
#define RR6_GEMMINI_CONFIG_EX(OP) \
  RR6_GEMMINI_CMD(OP, \
    ((uint64_t)acc_scale_t_to_acc_scale_t_bits((acc_scale_t)ACC_SCALE_IDENTITY) << 32) | \
    (1ULL << 16) | (OUTPUT_STATIONARY << 2), 1ULL << 48, k_CONFIG)
#define RR6_GEMMINI_MVIN(OP, DRAM, SPAD) \
  RR6_GEMMINI_CMD(OP, (DRAM), RR6_GEMMINI_SPAD((SPAD), RR6_DIM, RR6_DIM), k_MVIN)
#define RR6_GEMMINI_MVIN_RECT(OP, DRAM, SPAD, COLS, ROWS) \
  RR6_GEMMINI_CMD(OP, (DRAM), RR6_GEMMINI_SPAD((SPAD), (ROWS), (COLS)), k_MVIN)
#define RR6_GEMMINI_PRELOAD(OP, BD, C) \
  RR6_GEMMINI_CMD(OP, RR6_GEMMINI_SPAD((BD), RR6_DIM, RR6_DIM), \
    RR6_GEMMINI_SPAD((C), RR6_DIM, RR6_DIM), k_PRELOAD)
#define RR6_GEMMINI_COMPUTE(OP, A, BD) \
  RR6_GEMMINI_CMD(OP, RR6_GEMMINI_SPAD((A), RR6_DIM, RR6_DIM), \
    RR6_GEMMINI_SPAD((BD), RR6_DIM, RR6_DIM), k_COMPUTE_PRELOADED)
#define RR6_GEMMINI_MVOUT(OP, DRAM, SPAD) \
  RR6_GEMMINI_CMD(OP, (DRAM), RR6_GEMMINI_SPAD((SPAD), RR6_DIM, RR6_DIM), k_MVOUT)

#define RR6_GEMMINI_CONFIGURE(OP) do { \
  RR6_GEMMINI_CONFIG_LD(OP, RR6_DIM * sizeof(elem_t)); \
  RR6_GEMMINI_CONFIG_ST(OP, RR6_DIM * sizeof(elem_t)); \
} while (0)

#define RR6_GEMMINI_START(OP) RR6_GEMMINI_CONFIG_EX(OP)

#define RR6_GEMMINI_STEP(OP) do { \
  const uint32_t spad_a = 0; \
  const uint32_t spad_b = RR6_DIM; \
  const uint32_t spad_c = 2 * RR6_DIM; \
  RR6_GEMMINI_PRELOAD(OP, GARBAGE_ADDR, spad_c); \
  RR6_GEMMINI_COMPUTE(OP, spad_a, spad_b); \
} while (0)

#define RR6_GEMMINI_STORE(OP, OUT) \
  RR6_GEMMINI_MVOUT(OP, (OUT), 2 * RR6_DIM)

static void rr6_config_stream(uint32_t stream) {
  switch (stream) {
    case 0: RR6_GEMMINI_CONFIGURE(0); break;
    case 1: RR6_GEMMINI_CONFIGURE(1); break;
    case 2: RR6_GEMMINI_CONFIGURE(2); break;
    default: rr6_fail("invalid config stream");
  }
}

static void rr6_mvin_stream(uint32_t stream,
                             elem_t a[RR6_MATRIX_DIM][RR6_MATRIX_DIM],
                             elem_t b[RR6_MATRIX_DIM][RR6_MATRIX_DIM]) {
  switch (stream) {
    case 0: RR6_GEMMINI_MVIN(0, a, 0); RR6_GEMMINI_MVIN(0, b, RR6_DIM); break;
    case 1: RR6_GEMMINI_MVIN(1, a, 0); RR6_GEMMINI_MVIN(1, b, RR6_DIM); break;
    case 2: RR6_GEMMINI_MVIN(2, a, 0); RR6_GEMMINI_MVIN(2, b, RR6_DIM); break;
    default: rr6_fail("invalid mvin stream");
  }
}

static void rr6_start_stream(uint32_t stream) {
  switch (stream) {
    case 0: RR6_GEMMINI_START(0); break;
    case 1: RR6_GEMMINI_START(1); break;
    case 2: RR6_GEMMINI_START(2); break;
    default: rr6_fail("invalid start stream");
  }
}

static void rr6_step_stream(uint32_t stream) {
  switch (stream) {
    case 0: RR6_GEMMINI_STEP(0); break;
    case 1: RR6_GEMMINI_STEP(1); break;
    case 2: RR6_GEMMINI_STEP(2); break;
    default: rr6_fail("invalid compute stream");
  }
}

static void rr6_store_stream(uint32_t stream,
                             elem_t output[RR6_MATRIX_DIM][RR6_MATRIX_DIM]) {
  switch (stream) {
    case 0: RR6_GEMMINI_STORE(0, output); break;
    case 1: RR6_GEMMINI_STORE(1, output); break;
    case 2: RR6_GEMMINI_STORE(2, output); break;
    default: rr6_fail("invalid stream");
  }
}

int main(void) {
  static elem_t input_a[RR6_STREAMS][RR6_MATRIX_DIM][RR6_MATRIX_DIM]
      __attribute__((aligned(64)));
  static elem_t input_b[RR6_STREAMS][RR6_MATRIX_DIM][RR6_MATRIX_DIM]
      __attribute__((aligned(64)));
  static elem_t output[RR6_STREAMS][RR6_MATRIX_DIM][RR6_MATRIX_DIM]
      __attribute__((aligned(64)));
  static elem_t preflight_input[RR6_PREFLIGHT_ROWS][RR6_PREFLIGHT_COLS]
      __attribute__((aligned(64)));

  for (uint32_t stream = 0; stream < RR6_STREAMS; stream++)
    rr6_fill_operands(stream, input_a[stream], input_b[stream], output[stream]);
  for (uint32_t row = 0; row < RR6_PREFLIGHT_ROWS; row++)
    for (uint32_t col = 0; col < RR6_PREFLIGHT_COLS; col++)
      preflight_input[row][col] = (elem_t)((row + col) & 7U);

  rr_irq_enable();
  for (uint32_t stream = 0; stream < RR6_STREAMS; stream++) {
    if (!rr_acquire_single(rr6_streams[stream].cfg,
                           rr6_streams[stream].manager) ||
        !rr6_wait_acquired(rr6_streams[stream].cfg))
      rr6_fail("manager acquire");
  }

  /* One physical client maps three Rocket custom opcodes to three logical
   * cfgs.  The dedicated config gives the corresponding managers the same
   * one-opcode sets, so every command has an unambiguous route. */
  rr_set_opc(0, 0);
  rr_set_opc(1, 1);
  rr_set_opc(2, 2);
  asm volatile("fence rw, rw" ::: "memory");

  clear_csr(mstatus, RR_IRQ_MSTATUS_MIE);
  RR6_GEMMINI_CONFIG_LD(0, RR6_PREFLIGHT_COLS * sizeof(elem_t));
  for (uint32_t repeat = 0; repeat < RR6_PREFLIGHT_MVIN_REPEATS; repeat++)
    RR6_GEMMINI_MVIN_RECT(0, preflight_input, 0,
                          RR6_PREFLIGHT_COLS, RR6_PREFLIGHT_ROWS);
  const uint64_t preflight_submit_cycle = rr6_rdcycle();
  rr_async_end(0, RR6_PREFLIGHT_TOKEN);
  volatile uint64_t preflight_progress = 0;
  for (uint64_t i = 0; i < RR6_CPU_WORK; i++)
    preflight_progress += preflight_input[i % RR6_PREFLIGHT_ROWS]
        [i % RR6_PREFLIGHT_COLS] + i + 1;
  if (preflight_progress == 0 || rr_completion_available())
    rr6_fail("preflight completion arrived before WFI");
  rr6_preflight_wfi_entries++;
  asm volatile("wfi" ::: "memory");
  rr6_preflight_wfi_wakes++;
  set_csr(mstatus, RR_IRQ_MSTATUS_MIE);
  for (uint64_t spin = 0; spin < 1000000ULL && !rr6_preflight_retired; spin++)
    rr6_cpu_progress += spin & 3U;
  if (!rr6_preflight_retired)
    rr6_fail("preflight WFI completion");
  asm volatile("fence rw, rw" ::: "memory");
  if (rr_completion_available())
    rr6_fail("stale preflight completion");

  clear_csr(mstatus, RR_IRQ_MSTATUS_MIE);
  for (uint32_t stream = 0; stream < RR6_STREAMS; stream++)
    rr6_config_stream(stream);
  for (uint32_t repeat = 0; repeat < RR6_MVIN_REPEATS; repeat++)
    for (uint32_t stream = 0; stream < RR6_STREAMS; stream++)
      rr6_mvin_stream(stream, input_a[stream], input_b[stream]);
  for (uint32_t stream = 0; stream < RR6_STREAMS; stream++)
    rr6_start_stream(stream);
  for (uint32_t repeat = 0; repeat < RR6_COMPUTE_REPEATS; repeat++)
    for (uint32_t stream = 0; stream < RR6_STREAMS; stream++)
      rr6_step_stream(stream);
  for (uint32_t stream = 0; stream < RR6_STREAMS; stream++)
    rr6_store_stream(stream, output[stream]);
  for (uint32_t stream = 0; stream < RR6_STREAMS; stream++) {
    const uint64_t async_start = rr6_rdcycle();
    rr_async_end(rr6_streams[stream].cfg, rr6_streams[stream].token);
    rr6_streams[stream].submit_cycle = rr6_rdcycle();
    if (rr6_streams[stream].submit_cycle < async_start)
      rr6_fail("cycle counter moved backwards");
  }

  /* Useful CPU work is bounded and observable.  It is evidence that the
   * async arm does not wait for Gemmini; correctness never depends on its
   * duration. */
  for (uint64_t i = 0; i < RR6_CPU_WORK; i++) {
    rr6_cpu_progress += (i ^ 0x5a5aU) + input_a[i % RR6_STREAMS][i % RR6_DIM][i % RR6_DIM];
  }

  /* With MIE masked, completions can remain pending in the FIFO but cannot
   * enter the handler.  The main workload may have published all three
   * records before this WFI because command submission is serialized by one
   * Rocket.  The separate preflight DMA above is the required WFI-in-flight
   * evidence; this WFI validates that the same level IRQ path also handles
   * the multi-manager completion batch. */
  const uint64_t completion_count_before_wfi =
      read_rr_csr(CSR_RRCOMPLETION_COUNT);
  if (rr6_cpu_progress == 0)
    rr6_fail("CPU made no progress before WFI");
  rr6_wfi_entries++;
  asm volatile("wfi" ::: "memory");
  rr6_wfi_wakes++;
  set_csr(mstatus, RR_IRQ_MSTATUS_MIE);

  for (uint64_t spin = 0; spin < 1000000ULL && rr6_completion_count < RR6_STREAMS; spin++)
    rr6_cpu_progress += spin & 3U;

  if (rr6_completion_count != RR6_STREAMS || rr6_unexpected_traps ||
      rr6_duplicate_completions || rr6_bad_completions ||
      rr_completion_available()) {
    rr6_lost_completions = RR6_STREAMS -
      (rr6_completion_count < RR6_STREAMS ? rr6_completion_count : RR6_STREAMS);
    rr6_fail("completion scoreboard");
  }

  asm volatile("fence rw, rw" ::: "memory");
  for (uint32_t stream = 0; stream < RR6_STREAMS; stream++) {
    if (!rr6_streams[stream].retired ||
        !rr6_check_output(stream, input_a[stream], input_b[stream], output[stream]))
      rr6_fail("full Gemmini output comparison");
  }

  uint64_t max_submit = rr6_streams[0].submit_cycle;
  uint64_t first_completion = rr6_streams[0].completion_cycle;
  for (uint32_t stream = 1; stream < RR6_STREAMS; stream++) {
    if (rr6_streams[stream].submit_cycle > max_submit)
      max_submit = rr6_streams[stream].submit_cycle;
    if (rr6_streams[stream].completion_cycle < first_completion)
      first_completion = rr6_streams[stream].completion_cycle;
  }
  if (max_submit >= first_completion)
    rr6_fail("no three-stream cycle overlap");

  for (uint32_t stream = 0; stream < RR6_STREAMS; stream++) {
    write_rr_csr(CSR_RRCFG0 + rr6_streams[stream].cfg, 0);
    if (!rr6_wait_released(rr6_streams[stream].cfg))
      rr6_fail("manager release before retirement");
  }

  printf("Topology = 1 Rocket, 1 ReRoCC Client, 3 Managers, 3 Gemmini DIM=8x8\n");
  printf("Mapping cfg0->manager0->Gemmini0, cfg1->manager1->Gemmini1, cfg2->manager2->Gemmini2\n");
  printf("G0 token=0x%08lx submit_cycle=%lu completion_cycle=%lu\n",
         (unsigned long)rr6_streams[0].token,
         (unsigned long)rr6_streams[0].submit_cycle,
         (unsigned long)rr6_streams[0].completion_cycle);
  printf("G1 token=0x%08lx submit_cycle=%lu completion_cycle=%lu\n",
         (unsigned long)rr6_streams[1].token,
         (unsigned long)rr6_streams[1].submit_cycle,
         (unsigned long)rr6_streams[1].completion_cycle);
  printf("G2 token=0x%08lx submit_cycle=%lu completion_cycle=%lu\n",
         (unsigned long)rr6_streams[2].token,
         (unsigned long)rr6_streams[2].submit_cycle,
         (unsigned long)rr6_streams[2].completion_cycle);
  printf("Common three-stream overlap = [%lu,%lu) cycles=%lu\n",
         (unsigned long)max_submit, (unsigned long)first_completion,
         (unsigned long)(first_completion - max_submit));
  printf("Maximum simultaneous Gemmini in-flight = 3\n");
  printf("CPU progress after submissions = %lu\n", (unsigned long)rr6_cpu_progress);
  printf("Completion records pending before WFI = %lu\n",
         (unsigned long)completion_count_before_wfi);
  printf("Preflight mvin WFI: submit_cycle=%lu completion_cycle=%lu entries=%lu wakes=%lu\n",
         (unsigned long)preflight_submit_cycle,
         (unsigned long)rr6_preflight_completion_cycle,
         (unsigned long)rr6_preflight_wfi_entries,
         (unsigned long)rr6_preflight_wfi_wakes);
  printf("Submission order = G0,G1,G2; completion order =");
  for (uint64_t i = 0; i < rr6_completion_count; i++)
    printf(" G%u(token=0x%08lx,cfg=%lu,manager=%lu)",
           rr6_completion_order[i],
           (unsigned long)rr6_completion_tokens[i],
           (unsigned long)rr6_completion_cfgs[i],
           (unsigned long)rr6_completion_managers[i]);
  printf("\n");
  printf("IRQ count = %lu, completion records drained = %lu\n",
         (unsigned long)rr6_irq_count,
         (unsigned long)rr6_completion_irq_count);
  printf("WFI entries = %lu, WFI wakes = %lu\n",
         (unsigned long)rr6_wfi_entries, (unsigned long)rr6_wfi_wakes);
  printf("G0 full result PASS; G1 full result PASS; G2 full result PASS\n");
  printf("Lost completions = %lu, duplicate completions = %lu\n",
         (unsigned long)rr6_lost_completions,
         (unsigned long)rr6_duplicate_completions);
  printf("ReRoCC Phase 5 1R1C3M3G CONCURRENT TEST PASSED\n");
  exit(0);
}
