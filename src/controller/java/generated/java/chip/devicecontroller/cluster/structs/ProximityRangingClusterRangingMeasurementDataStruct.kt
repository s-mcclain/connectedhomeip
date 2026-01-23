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
package chip.devicecontroller.cluster.structs

import chip.devicecontroller.cluster.*
import java.util.Optional
import matter.tlv.ContextSpecificTag
import matter.tlv.Tag
import matter.tlv.TlvReader
import matter.tlv.TlvWriter

class ProximityRangingClusterRangingMeasurementDataStruct(
  val wiFiDevIK: Optional<ByteArray>,
  val BLEDeviceId: Optional<ULong>,
  val timeOfMeasurement: ULong,
  val distance: Int,
  val accuracy: Optional<UInt>,
  val rdr: Optional<ProximityRangingClusterRDRStruct>,
  val rssi: Optional<Int>,
  val txPower: Optional<Int>,
) {
  override fun toString(): String = buildString {
    append("ProximityRangingClusterRangingMeasurementDataStruct {\n")
    append("\twiFiDevIK : $wiFiDevIK\n")
    append("\tBLEDeviceId : $BLEDeviceId\n")
    append("\ttimeOfMeasurement : $timeOfMeasurement\n")
    append("\tdistance : $distance\n")
    append("\taccuracy : $accuracy\n")
    append("\trdr : $rdr\n")
    append("\trssi : $rssi\n")
    append("\ttxPower : $txPower\n")
    append("}\n")
  }

  fun toTlv(tlvTag: Tag, tlvWriter: TlvWriter) {
    tlvWriter.apply {
      startStructure(tlvTag)
      if (wiFiDevIK.isPresent) {
        val optwiFiDevIK = wiFiDevIK.get()
        put(ContextSpecificTag(TAG_WI_FI_DEV_IK), optwiFiDevIK)
      }
      if (BLEDeviceId.isPresent) {
        val optBLEDeviceId = BLEDeviceId.get()
        put(ContextSpecificTag(TAG_BLE_DEVICE_ID), optBLEDeviceId)
      }
      put(ContextSpecificTag(TAG_TIME_OF_MEASUREMENT), timeOfMeasurement)
      put(ContextSpecificTag(TAG_DISTANCE), distance)
      if (accuracy.isPresent) {
        val optaccuracy = accuracy.get()
        put(ContextSpecificTag(TAG_ACCURACY), optaccuracy)
      }
      if (rdr.isPresent) {
        val optrdr = rdr.get()
        optrdr.toTlv(ContextSpecificTag(TAG_RDR), this)
      }
      if (rssi.isPresent) {
        val optrssi = rssi.get()
        put(ContextSpecificTag(TAG_RSSI), optrssi)
      }
      if (txPower.isPresent) {
        val opttxPower = txPower.get()
        put(ContextSpecificTag(TAG_TX_POWER), opttxPower)
      }
      endStructure()
    }
  }

  companion object {
    private const val TAG_WI_FI_DEV_IK = 0
    private const val TAG_BLE_DEVICE_ID = 1
    private const val TAG_TIME_OF_MEASUREMENT = 2
    private const val TAG_DISTANCE = 3
    private const val TAG_ACCURACY = 4
    private const val TAG_RDR = 5
    private const val TAG_RSSI = 6
    private const val TAG_TX_POWER = 7

    fun fromTlv(
      tlvTag: Tag,
      tlvReader: TlvReader,
    ): ProximityRangingClusterRangingMeasurementDataStruct {
      tlvReader.enterStructure(tlvTag)
      val wiFiDevIK =
        if (tlvReader.isNextTag(ContextSpecificTag(TAG_WI_FI_DEV_IK))) {
          Optional.of(tlvReader.getByteArray(ContextSpecificTag(TAG_WI_FI_DEV_IK)))
        } else {
          Optional.empty()
        }
      val BLEDeviceId =
        if (tlvReader.isNextTag(ContextSpecificTag(TAG_BLE_DEVICE_ID))) {
          Optional.of(tlvReader.getULong(ContextSpecificTag(TAG_BLE_DEVICE_ID)))
        } else {
          Optional.empty()
        }
      val timeOfMeasurement = tlvReader.getULong(ContextSpecificTag(TAG_TIME_OF_MEASUREMENT))
      val distance = tlvReader.getInt(ContextSpecificTag(TAG_DISTANCE))
      val accuracy =
        if (tlvReader.isNextTag(ContextSpecificTag(TAG_ACCURACY))) {
          Optional.of(tlvReader.getUInt(ContextSpecificTag(TAG_ACCURACY)))
        } else {
          Optional.empty()
        }
      val rdr =
        if (tlvReader.isNextTag(ContextSpecificTag(TAG_RDR))) {
          Optional.of(
            ProximityRangingClusterRDRStruct.fromTlv(ContextSpecificTag(TAG_RDR), tlvReader)
          )
        } else {
          Optional.empty()
        }
      val rssi =
        if (tlvReader.isNextTag(ContextSpecificTag(TAG_RSSI))) {
          Optional.of(tlvReader.getInt(ContextSpecificTag(TAG_RSSI)))
        } else {
          Optional.empty()
        }
      val txPower =
        if (tlvReader.isNextTag(ContextSpecificTag(TAG_TX_POWER))) {
          Optional.of(tlvReader.getInt(ContextSpecificTag(TAG_TX_POWER)))
        } else {
          Optional.empty()
        }

      tlvReader.exitContainer()

      return ProximityRangingClusterRangingMeasurementDataStruct(
        wiFiDevIK,
        BLEDeviceId,
        timeOfMeasurement,
        distance,
        accuracy,
        rdr,
        rssi,
        txPower,
      )
    }
  }
}
