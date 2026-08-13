#ifndef REROCC_MULTICLIENT_H
#define REROCC_MULTICLIENT_H

#include "rerocc.h"

/* The hardware allocates one complete ReRoCC CSR bank per logical client.
 * These IDs intentionally match ReRoCCCSRs.BankStride and the Phase 5
 * configuration: client 0 = 0x800, client 1 = 0x840, client 2 = 0x880. */
#define RR_MC_CLIENTS 3
#define RR_MC_BANK_STRIDE 0x40
#define RR_MC_OPC0 0x00
#define RR_MC_OPC1 0x01
#define RR_MC_OPC2 0x02
#define RR_MC_OPC3 0x03
#define RR_MC_BAR 0x04
#define RR_MC_IRQ 0x05
#define RR_MC_ASYNC_END 0x06
#define RR_MC_COMPLETION_COUNT 0x07
#define RR_MC_COMPLETION_DATA 0x08
#define RR_MC_CFG0 0x10
#define RR_MC_CFGS 16

#define RR_MC_READ_CASES_0 \
  case 0x00: return read_csr(0x800); \
  case 0x01: return read_csr(0x801); \
  case 0x02: return read_csr(0x802); \
  case 0x03: return read_csr(0x803); \
  case 0x04: return read_csr(0x804); \
  case 0x05: return read_csr(0x805); \
  case 0x06: return read_csr(0x806); \
  case 0x07: return read_csr(0x807); \
  case 0x08: return read_csr(0x808); \
  case 0x10: return read_csr(0x810); \
  case 0x11: return read_csr(0x811); \
  case 0x12: return read_csr(0x812); \
  case 0x13: return read_csr(0x813); \
  case 0x14: return read_csr(0x814); \
  case 0x15: return read_csr(0x815); \
  case 0x16: return read_csr(0x816); \
  case 0x17: return read_csr(0x817); \
  case 0x18: return read_csr(0x818); \
  case 0x19: return read_csr(0x819); \
  case 0x1a: return read_csr(0x81a); \
  case 0x1b: return read_csr(0x81b); \
  case 0x1c: return read_csr(0x81c); \
  case 0x1d: return read_csr(0x81d); \
  case 0x1e: return read_csr(0x81e); \
  case 0x1f: return read_csr(0x81f);

#define RR_MC_READ_CASES_1 \
  case 0x00: return read_csr(0x840); \
  case 0x01: return read_csr(0x841); \
  case 0x02: return read_csr(0x842); \
  case 0x03: return read_csr(0x843); \
  case 0x04: return read_csr(0x844); \
  case 0x05: return read_csr(0x845); \
  case 0x06: return read_csr(0x846); \
  case 0x07: return read_csr(0x847); \
  case 0x08: return read_csr(0x848); \
  case 0x10: return read_csr(0x850); \
  case 0x11: return read_csr(0x851); \
  case 0x12: return read_csr(0x852); \
  case 0x13: return read_csr(0x853); \
  case 0x14: return read_csr(0x854); \
  case 0x15: return read_csr(0x855); \
  case 0x16: return read_csr(0x856); \
  case 0x17: return read_csr(0x857); \
  case 0x18: return read_csr(0x858); \
  case 0x19: return read_csr(0x859); \
  case 0x1a: return read_csr(0x85a); \
  case 0x1b: return read_csr(0x85b); \
  case 0x1c: return read_csr(0x85c); \
  case 0x1d: return read_csr(0x85d); \
  case 0x1e: return read_csr(0x85e); \
  case 0x1f: return read_csr(0x85f);

#define RR_MC_READ_CASES_2 \
  case 0x00: return read_csr(0x880); \
  case 0x01: return read_csr(0x881); \
  case 0x02: return read_csr(0x882); \
  case 0x03: return read_csr(0x883); \
  case 0x04: return read_csr(0x884); \
  case 0x05: return read_csr(0x885); \
  case 0x06: return read_csr(0x886); \
  case 0x07: return read_csr(0x887); \
  case 0x08: return read_csr(0x888); \
  case 0x10: return read_csr(0x890); \
  case 0x11: return read_csr(0x891); \
  case 0x12: return read_csr(0x892); \
  case 0x13: return read_csr(0x893); \
  case 0x14: return read_csr(0x894); \
  case 0x15: return read_csr(0x895); \
  case 0x16: return read_csr(0x896); \
  case 0x17: return read_csr(0x897); \
  case 0x18: return read_csr(0x898); \
  case 0x19: return read_csr(0x899); \
  case 0x1a: return read_csr(0x89a); \
  case 0x1b: return read_csr(0x89b); \
  case 0x1c: return read_csr(0x89c); \
  case 0x1d: return read_csr(0x89d); \
  case 0x1e: return read_csr(0x89e); \
  case 0x1f: return read_csr(0x89f);

#define RR_MC_SWAP_CASES_0 \
  case 0x00: return swap_csr(0x800, wdata); \
  case 0x01: return swap_csr(0x801, wdata); \
  case 0x02: return swap_csr(0x802, wdata); \
  case 0x03: return swap_csr(0x803, wdata); \
  case 0x04: return swap_csr(0x804, wdata); \
  case 0x05: return swap_csr(0x805, wdata); \
  case 0x06: return swap_csr(0x806, wdata); \
  case 0x07: return swap_csr(0x807, wdata); \
  case 0x08: return swap_csr(0x808, wdata); \
  case 0x10: return swap_csr(0x810, wdata); \
  case 0x11: return swap_csr(0x811, wdata); \
  case 0x12: return swap_csr(0x812, wdata); \
  case 0x13: return swap_csr(0x813, wdata); \
  case 0x14: return swap_csr(0x814, wdata); \
  case 0x15: return swap_csr(0x815, wdata); \
  case 0x16: return swap_csr(0x816, wdata); \
  case 0x17: return swap_csr(0x817, wdata); \
  case 0x18: return swap_csr(0x818, wdata); \
  case 0x19: return swap_csr(0x819, wdata); \
  case 0x1a: return swap_csr(0x81a, wdata); \
  case 0x1b: return swap_csr(0x81b, wdata); \
  case 0x1c: return swap_csr(0x81c, wdata); \
  case 0x1d: return swap_csr(0x81d, wdata); \
  case 0x1e: return swap_csr(0x81e, wdata); \
  case 0x1f: return swap_csr(0x81f, wdata);

#define RR_MC_SWAP_CASES_1 \
  case 0x00: return swap_csr(0x840, wdata); \
  case 0x01: return swap_csr(0x841, wdata); \
  case 0x02: return swap_csr(0x842, wdata); \
  case 0x03: return swap_csr(0x843, wdata); \
  case 0x04: return swap_csr(0x844, wdata); \
  case 0x05: return swap_csr(0x845, wdata); \
  case 0x06: return swap_csr(0x846, wdata); \
  case 0x07: return swap_csr(0x847, wdata); \
  case 0x08: return swap_csr(0x848, wdata); \
  case 0x10: return swap_csr(0x850, wdata); \
  case 0x11: return swap_csr(0x851, wdata); \
  case 0x12: return swap_csr(0x852, wdata); \
  case 0x13: return swap_csr(0x853, wdata); \
  case 0x14: return swap_csr(0x854, wdata); \
  case 0x15: return swap_csr(0x855, wdata); \
  case 0x16: return swap_csr(0x856, wdata); \
  case 0x17: return swap_csr(0x857, wdata); \
  case 0x18: return swap_csr(0x858, wdata); \
  case 0x19: return swap_csr(0x859, wdata); \
  case 0x1a: return swap_csr(0x85a, wdata); \
  case 0x1b: return swap_csr(0x85b, wdata); \
  case 0x1c: return swap_csr(0x85c, wdata); \
  case 0x1d: return swap_csr(0x85d, wdata); \
  case 0x1e: return swap_csr(0x85e, wdata); \
  case 0x1f: return swap_csr(0x85f, wdata);

#define RR_MC_SWAP_CASES_2 \
  case 0x00: return swap_csr(0x880, wdata); \
  case 0x01: return swap_csr(0x881, wdata); \
  case 0x02: return swap_csr(0x882, wdata); \
  case 0x03: return swap_csr(0x883, wdata); \
  case 0x04: return swap_csr(0x884, wdata); \
  case 0x05: return swap_csr(0x885, wdata); \
  case 0x06: return swap_csr(0x886, wdata); \
  case 0x07: return swap_csr(0x887, wdata); \
  case 0x08: return swap_csr(0x888, wdata); \
  case 0x10: return swap_csr(0x890, wdata); \
  case 0x11: return swap_csr(0x891, wdata); \
  case 0x12: return swap_csr(0x892, wdata); \
  case 0x13: return swap_csr(0x893, wdata); \
  case 0x14: return swap_csr(0x894, wdata); \
  case 0x15: return swap_csr(0x895, wdata); \
  case 0x16: return swap_csr(0x896, wdata); \
  case 0x17: return swap_csr(0x897, wdata); \
  case 0x18: return swap_csr(0x898, wdata); \
  case 0x19: return swap_csr(0x899, wdata); \
  case 0x1a: return swap_csr(0x89a, wdata); \
  case 0x1b: return swap_csr(0x89b, wdata); \
  case 0x1c: return swap_csr(0x89c, wdata); \
  case 0x1d: return swap_csr(0x89d, wdata); \
  case 0x1e: return swap_csr(0x89e, wdata); \
  case 0x1f: return swap_csr(0x89f, wdata);

static inline uint64_t rr_mc_read_csr(uint32_t client, uint32_t offset) {
  switch (client) {
    case 0: switch (offset) { RR_MC_READ_CASES_0 default: break; } break;
    case 1: switch (offset) { RR_MC_READ_CASES_1 default: break; } break;
    case 2: switch (offset) { RR_MC_READ_CASES_2 default: break; } break;
    default: break;
  }
  printf("rr_mc_read_csr illegal client=%lu offset=0x%lx\n",
         (unsigned long)client, (unsigned long)offset);
  abort();
}

static inline uint64_t rr_mc_swap_csr(uint32_t client, uint32_t offset,
                                      uint64_t wdata) {
  switch (client) {
    case 0: switch (offset) { RR_MC_SWAP_CASES_0 default: break; } break;
    case 1: switch (offset) { RR_MC_SWAP_CASES_1 default: break; } break;
    case 2: switch (offset) { RR_MC_SWAP_CASES_2 default: break; } break;
    default: break;
  }
  printf("rr_mc_swap_csr illegal client=%lu offset=0x%lx\n",
         (unsigned long)client, (unsigned long)offset);
  abort();
}

static inline uint64_t rr_mc_read_cfg(uint32_t client, uint32_t cfg_id) {
  if (cfg_id >= RR_MC_CFGS) abort();
  return rr_mc_read_csr(client, RR_MC_CFG0 + cfg_id);
}

static inline bool rr_mc_acquire(uint32_t client, uint32_t cfg_id,
                                 uint32_t manager_id) {
  if (cfg_id >= RR_MC_CFGS || client >= RR_MC_CLIENTS) abort();
  rr_mc_swap_csr(client, RR_MC_CFG0 + cfg_id,
                 RR_CFG_ACQ_MASK | (manager_id & RR_CFG_MGR_MASK));
  return (rr_mc_read_cfg(client, cfg_id) & RR_CFG_ACQ_MASK) != 0;
}

static inline void rr_mc_release(uint32_t client, uint32_t cfg_id) {
  if (cfg_id >= RR_MC_CFGS || client >= RR_MC_CLIENTS) abort();
  rr_mc_swap_csr(client, RR_MC_CFG0 + cfg_id, 0);
}

static inline void rr_mc_set_opc(uint32_t client, uint32_t opc,
                                 uint32_t cfg_id) {
  if (client >= RR_MC_CLIENTS || opc >= 4 || cfg_id >= RR_MC_CFGS) abort();
  rr_mc_swap_csr(client, opc, cfg_id);
}

static inline void rr_mc_fence(uint32_t client, uint32_t cfg_id) {
  if (client >= RR_MC_CLIENTS || cfg_id >= RR_MC_CFGS) abort();
  rr_mc_swap_csr(client, RR_MC_BAR, cfg_id);
  asm volatile("fence" ::: "memory");
}

static inline void rr_mc_async_end(uint32_t client, uint32_t cfg_id,
                                   uint32_t token) {
  if (client >= RR_MC_CLIENTS || cfg_id >= RR_MC_CFGS) abort();
  uint64_t data = ((uint64_t)(cfg_id & 0x00ffffffU) << 40) |
                  ((uint64_t)token << 8);
  rr_mc_swap_csr(client, RR_MC_ASYNC_END, data);
  asm volatile("fence" ::: "memory");
}

static inline uint64_t rr_mc_completion_count(uint32_t client) {
  if (client >= RR_MC_CLIENTS) abort();
  return rr_mc_read_csr(client, RR_MC_COMPLETION_COUNT);
}

static inline bool rr_mc_completion_available(uint32_t client) {
  return rr_mc_completion_count(client) != 0;
}

static inline void rr_mc_completion_pop(uint32_t client,
                                        struct rr_completion *completion) {
  if (client >= RR_MC_CLIENTS) abort();
  rr_completion_decode(rr_mc_read_csr(client, RR_MC_COMPLETION_DATA),
                       completion);
}

static inline bool rr_mc_irq_pending(uint32_t client) {
  if (client >= RR_MC_CLIENTS) abort();
  return (rr_mc_read_csr(client, RR_MC_IRQ) & 1) != 0;
}

static inline void rr_mc_irq_ack(uint32_t client) {
  if (client >= RR_MC_CLIENTS) abort();
  rr_mc_swap_csr(client, RR_MC_IRQ, 1);
}

#endif
