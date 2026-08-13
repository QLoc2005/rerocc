package rerocc.client

import chisel3._
import chisel3.util._
import freechips.rocketchip.tile.{CustomCSR}


object ReRoCCCSRs {
  val MAX_CFGS = 16
  val DefaultBase = 0x800
  val BankStride = 0x40

  def customCSRs(nCfgs: Int): Seq[CustomCSR] = customCSRs(nCfgs, DefaultBase)

  def customCSRs(nCfgs: Int, base: Int): Seq[CustomCSR] = {
    require(base >= 0 && base + 0x1f < 0x1000,
      f"ReRoCC CSR bank 0x$base%03x does not fit in the 12-bit CSR space")
    val fixed = Seq(
      (base + 0x00, log2Ceil(MAX_CFGS)),
      (base + 0x01, log2Ceil(MAX_CFGS)),
      (base + 0x02, log2Ceil(MAX_CFGS)),
      (base + 0x03, log2Ceil(MAX_CFGS)),
      (base + 0x04, log2Ceil(MAX_CFGS)),
      (base + 0x05, 1),
      (base + 0x06, 64),
      (base + 0x07, 5),
      (base + 0x08, 64)
    )
    val configs = (0 until nCfgs).map { i => (base + 0x10 + i, 9) }
    (fixed ++ configs).map { case (csr, sz) =>
      CustomCSR(csr, (BigInt(1) << sz) - 1, Some(0))
    }
  }
}

class ReRoCCCfg extends Bundle {
  val acq = Bool()
  val mgr = UInt(8.W)
}
