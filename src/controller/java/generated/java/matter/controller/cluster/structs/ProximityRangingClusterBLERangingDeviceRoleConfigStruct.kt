/*
 *
 *    Copyright (c) 2023 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */
package matter.controller.cluster.structs

import java.util.Optional
import matter.controller.cluster.*
import matter.tlv.ContextSpecificTag
import matter.tlv.Tag
import matter.tlv.TlvReader
import matter.tlv.TlvWriter

class ProximityRangingClusterBLERangingDeviceRoleConfigStruct(
  val role: UByte,
  val peerBLEDeviceID: ULong,
  val pmk: Optional<ByteArray>,
) {
  override fun toString(): String = buildString {
    append("ProximityRangingClusterBLERangingDeviceRoleConfigStruct {\n")
    append("\trole : $role\n")
    append("\tpeerBLEDeviceID : $peerBLEDeviceID\n")
    append("\tpmk : $pmk\n")
    append("}\n")
  }

  fun toTlv(tlvTag: Tag, tlvWriter: TlvWriter) {
    tlvWriter.apply {
      startStructure(tlvTag)
      put(ContextSpecificTag(TAG_ROLE), role)
      put(ContextSpecificTag(TAG_PEER_BLE_DEVICE_ID), peerBLEDeviceID)
      if (pmk.isPresent) {
        val optpmk = pmk.get()
        put(ContextSpecificTag(TAG_PMK), optpmk)
      }
      endStructure()
    }
  }

  companion object {
    private const val TAG_ROLE = 0
    private const val TAG_PEER_BLE_DEVICE_ID = 1
    private const val TAG_PMK = 2

    fun fromTlv(
      tlvTag: Tag,
      tlvReader: TlvReader,
    ): ProximityRangingClusterBLERangingDeviceRoleConfigStruct {
      tlvReader.enterStructure(tlvTag)
      val role = tlvReader.getUByte(ContextSpecificTag(TAG_ROLE))
      val peerBLEDeviceID = tlvReader.getULong(ContextSpecificTag(TAG_PEER_BLE_DEVICE_ID))
      val pmk =
        if (tlvReader.isNextTag(ContextSpecificTag(TAG_PMK))) {
          Optional.of(tlvReader.getByteArray(ContextSpecificTag(TAG_PMK)))
        } else {
          Optional.empty()
        }

      tlvReader.exitContainer()

      return ProximityRangingClusterBLERangingDeviceRoleConfigStruct(role, peerBLEDeviceID, pmk)
    }
  }
}
