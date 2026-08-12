package rerocc.bus

import chisel3._
import chisel3.util._
import org.chipsalliance.cde.config._
import freechips.rocketchip.tile.{CustomCSR}

import rerocc.client._
import rerocc.manager._
import rerocc.bus._

case class ReRoCCClientPortParams(
  clients: Seq[ReRoCCClientParams]
)

case class ReRoCCManagerPortParams(
  managers: Seq[ReRoCCManagerParams]
)

case class ReRoCCEdgeParams(
  mParams: ReRoCCManagerPortParams,
  cParams: ReRoCCClientPortParams
) {
  require(cParams.clients.size >= 1 && mParams.managers.size >= 1)
  val bundle = ReRoCCBundleParams(
    log2Ceil(cParams.clients.map(_.nCfgs).sum),
    8
  )
  require(mParams.managers.forall(_.managerId < 256))
}

case class ReRoCCBundleParams(
  clientIdBits: Int,
  managerIdBits: Int
) {
  def union(x: ReRoCCBundleParams) = ReRoCCBundleParams(
    clientIdBits = clientIdBits.max(x.clientIdBits),
    managerIdBits = managerIdBits.max(x.managerIdBits))
}
