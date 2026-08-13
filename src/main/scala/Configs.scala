package rerocc

import chisel3._
import org.chipsalliance.cde.config._
import freechips.rocketchip.tile.{BuildRoCC, OpcodeSet}
import freechips.rocketchip.diplomacy.{LazyModule}

import rerocc.client._
import rerocc.manager._
import rerocc.bus._

class WithReRoCC(clientParams: ReRoCCClientParams = ReRoCCClientParams(), reRoCCManagerParams: ReRoCCTileParams = ReRoCCTileParams()) extends Config((site, here, up) => {
  case BuildRoCC => Seq((p: Parameters) => {
    val rerocc_client = LazyModule(new ReRoCCClient(clientParams)(p))
    rerocc_client
  })
  case ReRoCCTileKey => up(BuildRoCC).map(gen => reRoCCManagerParams.copy(genRoCC=Some(gen)))
})

/**
  * Phase 5 side experiment: three logical ReRoCC clients on one Rocket, all
  * routed to one manager wrapping one lower-priority accelerator.
  */
class WithReRoCC1R3C(
  clientParams: Seq[ReRoCCClientParams] = Seq(
    ReRoCCClientParams(clientId = 0, csrBase = 0x800, opcodes = OpcodeSet.custom3),
    ReRoCCClientParams(clientId = 1, csrBase = 0x840, opcodes = OpcodeSet.custom1),
    ReRoCCClientParams(clientId = 2, csrBase = 0x880, opcodes = OpcodeSet.custom2)
  ),
  reRoCCManagerParams: ReRoCCTileParams = ReRoCCTileParams()
) extends Config((site, here, up) => {
  case BuildRoCC => {
    require(clientParams.size == 3, "Phase 5 requires exactly three ReRoCC clients")
    clientParams.map { params => (p: Parameters) => {
      println(f"ReRoCC 1R3C Client ${params.clientId}: CSR base 0x${params.csrBase}%03x, opcodes ${params.opcodes.opcodes}")
      LazyModule(new ReRoCCClient(params)(p))
    }}
  }
  case ReRoCCTileKey => {
    val wrapped = up(BuildRoCC)
    require(wrapped.size == 1,
      s"Phase 5 requires exactly one wrapped accelerator, found ${wrapped.size}")
    wrapped.map(gen => reRoCCManagerParams.copy(genRoCC = Some(gen)))
  }
})

class WithReRoCCNoC(nocParams: ReRoCCNoCParams) extends Config((site, here, up) => {
  case ReRoCCNoCKey => Some(nocParams)
})
