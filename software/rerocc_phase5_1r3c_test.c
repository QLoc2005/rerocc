#include "rerocc_multiclient.h"
#include "include/gemmini.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

enum {
  RR5_MANAGER = 0,
  RR5_CFG = 0,
  RR5_DIM = 16,
  RR5_REQUESTS = 12,
  RR5_COMPLETION_STORAGE = 32,
  RR5_STANDALONE_BASE = 0,
  RR5_SEQUENTIAL_BASE = 3,
  RR5_INTERLEAVED_BASE = 6,
};

static volatile uint64_t rr5_irq_count;
static volatile uint64_t rr5_completion_irq_count;
static volatile uint64_t rr5_completion_count;
static volatile uint64_t rr5_wfi_entries;
static volatile uint64_t rr5_wfi_wakes;
static volatile uint64_t rr5_cpu_progress;
static volatile uint64_t rr5_unexpected_traps;
static volatile uint64_t rr5_completion_overflow;
static struct rr_completion rr5_completions[RR5_COMPLETION_STORAGE];

static inline uint64_t rr5_rdcycle(void) {
  uint64_t value;
  asm volatile("rdcycle %0" : "=r"(value));
  return value;
}

uintptr_t handle_trap(uintptr_t epc, uintptr_t cause, uintptr_t tval,
                      uintptr_t regs[32]) {
  (void)tval;
  (void)regs;

  if (cause == RR_IRQ_MCAUSE) {
    rr5_irq_count++;

    /* Every client owns a legacy RRIRQ bit.  Acknowledge all asserted bits,
     * then snapshot each completion count once.  The FIFO remains level
     * asserted if more records are waiting, so one trap is not confused with
     * one completion. */
    for (uint32_t client = 0; client < RR_MC_CLIENTS; client++) {
      if (rr_mc_irq_pending(client)) rr_mc_irq_ack(client);
    }
    for (uint32_t client = 0; client < RR_MC_CLIENTS; client++) {
      const uint64_t pending = rr_mc_completion_count(client);
      if (pending != 0) {
        struct rr_completion completion;
        rr_mc_completion_pop(client, &completion);
        rr5_completion_irq_count++;
        if (rr5_completion_count < RR5_COMPLETION_STORAGE)
          rr5_completions[rr5_completion_count] = completion;
        else
          rr5_completion_overflow++;
        rr5_completion_count++;
      }
    }
    return epc;
  }

  rr5_unexpected_traps++;
  return epc;
}

static void rr5_fail(const char *message) {
  printf("Phase 5 FAIL: %s\n", message);
  abort();
}

static bool rr5_wait_acquired(uint32_t client) {
  for (uint64_t spin = 0; spin < 1000000ULL; spin++)
    if ((rr_mc_read_cfg(client, RR5_CFG) & RR_CFG_ACQ_MASK) != 0) return true;
  return false;
}

static bool rr5_wait_released(uint32_t client) {
  for (uint64_t spin = 0; spin < 1000000ULL; spin++)
    if ((rr_mc_read_cfg(client, RR5_CFG) & RR_CFG_ACQ_MASK) == 0) return true;
  return false;
}

static bool rr5_wait_completion(uint64_t target) {
  for (uint64_t spin = 0; spin < 1000000ULL; spin++)
    if (rr5_completion_count >= target) return true;
  return false;
}

static void rr5_fill_operands(uint32_t slot,
                              elem_t a[RR5_DIM][RR5_DIM],
                              elem_t b[RR5_DIM][RR5_DIM],
                              elem_t output[RR5_DIM][RR5_DIM]) {
  for (size_t row = 0; row < RR5_DIM; row++) {
    for (size_t col = 0; col < RR5_DIM; col++) {
      a[row][col] = (elem_t)((row + 2 * col + slot) % 5) - 2;
      b[row][col] = (elem_t)((3 * row + col + 2 * slot) % 5) - 2;
      output[row][col] = (elem_t)-7;
    }
  }
}

static bool rr5_check_output(uint32_t slot,
                              const elem_t a[RR5_DIM][RR5_DIM],
                              const elem_t b[RR5_DIM][RR5_DIM],
                              const elem_t output[RR5_DIM][RR5_DIM]) {
  for (size_t row = 0; row < RR5_DIM; row++) {
    for (size_t col = 0; col < RR5_DIM; col++) {
      int32_t expected = 0;
      for (size_t k = 0; k < RR5_DIM; k++)
        expected += (int32_t)a[row][k] * (int32_t)b[k][col];
      if (output[row][col] != (elem_t)expected) {
        printf("Phase 5 mismatch slot=%lu row=%lu col=%lu expected=%ld observed=%d\n",
               (unsigned long)slot, (unsigned long)row, (unsigned long)col,
               (long)expected, (int)output[row][col]);
        return false;
      }
    }
  }
  return true;
}

/* The upstream Gemmini header provides the command funct/encoding constants.
 * This local wrapper changes only the Rocket custom opcode.  Client 0/1/2
 * therefore issue the same real Gemmini command stream through custom3/1/2. */
#define RR5_GEMMINI_CMD(OP, RS1, RS2, FUNCT) \
  ROCC_INSTRUCTION_0_R_R(OP, (uint64_t)(RS1), (uint64_t)(RS2), FUNCT)
#define RR5_GEMMINI_SPAD(ADDR, ROWS, COLS) \
  (((uint64_t)(ROWS) << (ADDR_LEN + 16)) | \
   ((uint64_t)(COLS) << ADDR_LEN) | (uint64_t)(ADDR))
#define RR5_GEMMINI_CONFIG_LD(OP, STRIDE) \
  RR5_GEMMINI_CMD(OP, \
                  ((uint64_t)scale_t_to_scale_t_bits((scale_t)MVIN_SCALE_IDENTITY) << 32) | \
                  ((uint64_t)(RR5_DIM) << 16) | (1ULL << 8) | CONFIG_LD, \
                  (STRIDE), k_CONFIG)
#define RR5_GEMMINI_CONFIG_ST(OP, STRIDE) \
  RR5_GEMMINI_CMD(OP, CONFIG_ST, \
                  ((uint64_t)acc_scale_t_to_acc_scale_t_bits((acc_scale_t)ACC_SCALE_IDENTITY) << 32) | \
                  (uint32_t)(STRIDE), k_CONFIG)
#define RR5_GEMMINI_CONFIG_EX(OP) \
  RR5_GEMMINI_CMD(OP, \
                  ((uint64_t)acc_scale_t_to_acc_scale_t_bits((acc_scale_t)ACC_SCALE_IDENTITY) << 32) | \
                  (1ULL << 16) | (OUTPUT_STATIONARY << 2), 1ULL << 48, k_CONFIG)
#define RR5_GEMMINI_MVIN(OP, DRAM, SPAD) \
  RR5_GEMMINI_CMD(OP, (DRAM), RR5_GEMMINI_SPAD((SPAD), RR5_DIM, RR5_DIM), k_MVIN)
#define RR5_GEMMINI_PRELOAD(OP, BD, C) \
  RR5_GEMMINI_CMD(OP, RR5_GEMMINI_SPAD((BD), RR5_DIM, RR5_DIM), \
                  RR5_GEMMINI_SPAD((C), RR5_DIM, RR5_DIM), k_PRELOAD)
#define RR5_GEMMINI_COMPUTE(OP, A, BD) \
  RR5_GEMMINI_CMD(OP, RR5_GEMMINI_SPAD((A), RR5_DIM, RR5_DIM), \
                  RR5_GEMMINI_SPAD((BD), RR5_DIM, RR5_DIM), k_COMPUTE_PRELOADED)
#define RR5_GEMMINI_MVOUT(OP, DRAM, SPAD) \
  RR5_GEMMINI_CMD(OP, (DRAM), RR5_GEMMINI_SPAD((SPAD), RR5_DIM, RR5_DIM), k_MVOUT)

#define RR5_GEMMINI_RUN(OP, A, B, OUT) do { \
  RR5_GEMMINI_CONFIG_LD(OP, RR5_DIM * sizeof(elem_t)); \
  RR5_GEMMINI_CONFIG_ST(OP, RR5_DIM * sizeof(elem_t)); \
  RR5_GEMMINI_MVIN(OP, (A), 0); \
  RR5_GEMMINI_MVIN(OP, (B), DIM); \
  RR5_GEMMINI_CONFIG_EX(OP); \
  RR5_GEMMINI_PRELOAD(OP, GARBAGE_ADDR, 2 * DIM); \
  RR5_GEMMINI_COMPUTE(OP, 0, DIM); \
  RR5_GEMMINI_MVOUT(OP, (OUT), 2 * DIM); \
} while (0)

static void rr5_issue_gemmini(uint32_t client,
                              elem_t a[RR5_DIM][RR5_DIM],
                              elem_t b[RR5_DIM][RR5_DIM],
                              elem_t output[RR5_DIM][RR5_DIM]) {
  switch (client) {
    case 0: RR5_GEMMINI_RUN(3, a, b, output); break;
    case 1: RR5_GEMMINI_RUN(1, a, b, output); break;
    case 2: RR5_GEMMINI_RUN(2, a, b, output); break;
    default: rr5_fail("invalid client opcode");
  }
}

static uint32_t rr5_expected_client(uint32_t slot) {
  static const uint32_t interleaved[] = {0, 2, 1, 0, 1, 2};
  if (slot < RR5_STANDALONE_BASE + 3) return slot;
  if (slot < RR5_INTERLEAVED_BASE) return slot - RR5_SEQUENTIAL_BASE;
  return interleaved[slot - RR5_INTERLEAVED_BASE];
}

static uint32_t rr5_opcode(uint32_t client) {
  /* custom3 matches Gemmini's upstream RoCC opcode for Client 0. */
  return client == 0 ? 3 : client;
}

static uint32_t rr5_expected_token(uint32_t slot) {
  return 0x50000000U | ((slot + 1) << 8) | rr5_expected_client(slot);
}

static bool rr5_validate_completion(uint32_t slot, uint64_t base_count) {
  if (!rr5_wait_completion(base_count + 1)) return false;
  if (rr5_completion_count != base_count + 1) return false;

  bool found = false;
  const uint32_t expected_client = rr5_expected_client(slot);
  const uint32_t expected_token = rr5_expected_token(slot);
  for (uint64_t i = 0; i < rr5_completion_count; i++) {
    const struct rr_completion *completion = &rr5_completions[i];
    if (completion->token == expected_token) {
      if (found || completion->client_id != expected_client ||
          completion->cfg_id != RR5_CFG ||
          completion->manager_id != RR5_MANAGER || completion->status != 0)
        return false;
      found = true;
    }
  }
  return found;
}

static bool rr5_release(uint32_t client) {
  rr_mc_release(client, RR5_CFG);
  return rr5_wait_released(client);
}

static bool rr5_run_request(uint32_t slot, uint32_t client,
                            elem_t a[RR5_REQUESTS][RR5_DIM][RR5_DIM],
                            elem_t b[RR5_REQUESTS][RR5_DIM][RR5_DIM],
                            elem_t output[RR5_REQUESTS][RR5_DIM][RR5_DIM]) {
  if (client != rr5_expected_client(slot)) return false;
  rr5_fill_operands(slot, a[slot], b[slot], output[slot]);
  if (!rr_mc_acquire(client, RR5_CFG, RR5_MANAGER) ||
      !rr5_wait_acquired(client)) return false;

  /* custom3/custom1/custom2 select the three distinct Rocket client routes. */
  rr_mc_set_opc(client, rr5_opcode(client), RR5_CFG);
  asm volatile("fence rw, rw" ::: "memory");
  rr5_issue_gemmini(client, a[slot], b[slot], output[slot]);

  clear_csr(mstatus, RR_IRQ_MSTATUS_MIE);
  const uint64_t completion_base = rr5_completion_count;
  if (rr_mc_completion_available(client)) return false;
  rr_mc_async_end(client, RR5_CFG, rr5_expected_token(slot));
  rr5_cpu_progress += (uint64_t)(slot + 1);
  const uint64_t wfi_before = rr5_rdcycle();
  rr5_wfi_entries++;
  asm volatile("wfi" ::: "memory");
  rr5_wfi_wakes++;
  const uint64_t wfi_after = rr5_rdcycle();
  set_csr(mstatus, RR_IRQ_MSTATUS_MIE);

  if (wfi_after < wfi_before || !rr5_validate_completion(slot, completion_base))
    return false;
  if (rr5_completion_count != completion_base + 1 ||
      rr_mc_completion_count(client) != 0) return false;

  asm volatile("fence rw, rw" ::: "memory");
  if (!rr5_check_output(slot, a[slot], b[slot], output[slot])) return false;
  if (!rr5_release(client)) return false;

  return rr_mc_completion_count(0) == 0 &&
         rr_mc_completion_count(1) == 0 &&
         rr_mc_completion_count(2) == 0;
}

static bool rr5_validate_all_completions(void) {
  bool retired[RR5_REQUESTS] = {false};
  if (rr5_completion_count != RR5_REQUESTS || rr5_completion_overflow ||
      rr5_unexpected_traps) return false;
  for (uint64_t i = 0; i < rr5_completion_count; i++) {
    bool found = false;
    for (uint32_t slot = 0; slot < RR5_REQUESTS; slot++) {
      if (!retired[slot] && rr5_completions[i].token == rr5_expected_token(slot)) {
        if (rr5_completions[i].client_id != rr5_expected_client(slot) ||
            rr5_completions[i].cfg_id != RR5_CFG ||
            rr5_completions[i].manager_id != RR5_MANAGER ||
            rr5_completions[i].status != 0) return false;
        retired[slot] = true;
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  for (uint32_t slot = 0; slot < RR5_REQUESTS; slot++)
    if (!retired[slot]) return false;
  return true;
}

int main(void) {
  static elem_t input_a[RR5_REQUESTS][RR5_DIM][RR5_DIM] row_align(1);
  static elem_t input_b[RR5_REQUESTS][RR5_DIM][RR5_DIM] row_align(1);
  static elem_t output[RR5_REQUESTS][RR5_DIM][RR5_DIM] row_align(1);

  rr_irq_enable();

  /* Concurrent outstanding requests are intentionally probed, not faked:
   * the one manager has scalar ownership/instruction/completion state. */
  if (!rr_mc_acquire(0, RR5_CFG, RR5_MANAGER)) rr5_fail("client 0 probe acquire");
  const bool concurrent_supported = rr_mc_acquire(1, RR5_CFG, RR5_MANAGER);
  if (concurrent_supported || !rr5_release(0))
    rr5_fail("single-manager serialization probe");
  if (rr_mc_completion_count(0) || rr_mc_completion_count(1) ||
      rr_mc_completion_count(2))
    rr5_fail("stale completion before tests");
  printf("Concurrent outstanding clients = NO (single manager ownership is scalar)\n");

  clear_csr(mstatus, RR_IRQ_MSTATUS_MIE);
  if (!rr5_run_request(RR5_STANDALONE_BASE + 0, 0, input_a, input_b, output))
    rr5_fail("Client 0 standalone");
  printf("Client 0 standalone PASS token=0x%08lx\n",
         (unsigned long)rr5_expected_token(RR5_STANDALONE_BASE + 0));

  clear_csr(mstatus, RR_IRQ_MSTATUS_MIE);
  if (!rr5_run_request(RR5_STANDALONE_BASE + 1, 1, input_a, input_b, output))
    rr5_fail("Client 1 standalone");
  printf("Client 1 standalone PASS token=0x%08lx\n",
         (unsigned long)rr5_expected_token(RR5_STANDALONE_BASE + 1));

  clear_csr(mstatus, RR_IRQ_MSTATUS_MIE);
  if (!rr5_run_request(RR5_STANDALONE_BASE + 2, 2, input_a, input_b, output))
    rr5_fail("Client 2 standalone");
  printf("Client 2 standalone PASS token=0x%08lx\n",
         (unsigned long)rr5_expected_token(RR5_STANDALONE_BASE + 2));

  const uint32_t sequential_clients[] = {0, 1, 2};
  for (uint32_t i = 0; i < 3; i++) {
    clear_csr(mstatus, RR_IRQ_MSTATUS_MIE);
    if (!rr5_run_request(RR5_SEQUENTIAL_BASE + i, sequential_clients[i],
                         input_a, input_b, output))
      rr5_fail("sequential cross-client isolation");
  }
  printf("Sequential cross-client isolation PASS\n");

  const uint32_t interleaved_clients[] = {0, 2, 1, 0, 1, 2};
  for (uint32_t i = 0; i < 6; i++) {
    clear_csr(mstatus, RR_IRQ_MSTATUS_MIE);
    if (!rr5_run_request(RR5_INTERLEAVED_BASE + i, interleaved_clients[i],
                         input_a, input_b, output))
      rr5_fail("interleaved cross-client routing");
  }
  if (!rr5_validate_all_completions()) rr5_fail("completion mapping/exactly-once");

  printf("Submission order = standalone(0,1,2), sequential(0,1,2), interleaved(0,2,1,0,1,2)\n");
  printf("Completion order =");
  for (uint64_t i = 0; i < rr5_completion_count; i++)
    printf(" 0x%08lx->%u", (unsigned long)rr5_completions[i].token,
           rr5_completions[i].client_id);
  printf("\n");
  printf("Completion count = %lu\n", (unsigned long)rr5_completion_count);
  printf("Completion IRQ drains = %lu\n", (unsigned long)rr5_completion_irq_count);
  printf("IRQ handler count = %lu\n", (unsigned long)rr5_irq_count);
  printf("WFI entries = %lu\n", (unsigned long)rr5_wfi_entries);
  printf("WFI wakes = %lu\n", (unsigned long)rr5_wfi_wakes);
  printf("CPU progress = %lu\n", (unsigned long)rr5_cpu_progress);
  printf("Full output golden comparisons = %u\n", RR5_REQUESTS);
  printf("Client/token mapping PASS (0x%08lx -> 0, 0x%08lx -> 1, 0x%08lx -> 2)\n",
         (unsigned long)rr5_expected_token(0),
         (unsigned long)rr5_expected_token(1),
         (unsigned long)rr5_expected_token(2));
  printf("Level IRQ FIFO isolation PASS\n");
  printf("ReRoCC Phase 5 1R3C TEST PASSED\n");
  return 0;
}
