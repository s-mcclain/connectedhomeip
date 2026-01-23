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

class ProximityRangingClusterRangingTriggerConditionStruct(
  val startTimeDelay: UInt,
  val endTimeDelay: Optional<UInt>,
  val rangingInstanceInterval: Optional<UInt>,
) {
  override fun toString(): String = buildString {
    append("ProximityRangingClusterRangingTriggerConditionStruct {\n")
    append("\tstartTimeDelay : $startTimeDelay\n")
    append("\tendTimeDelay : $endTimeDelay\n")
    append("\trangingInstanceInterval : $rangingInstanceInterval\n")
    append("}\n")
  }

  fun toTlv(tlvTag: Tag, tlvWriter: TlvWriter) {
    tlvWriter.apply {
      startStructure(tlvTag)
      put(ContextSpecificTag(TAG_START_TIME_DELAY), startTimeDelay)
      if (endTimeDelay.isPresent) {
        val optendTimeDelay = endTimeDelay.get()
        put(ContextSpecificTag(TAG_END_TIME_DELAY), optendTimeDelay)
      }
      if (rangingInstanceInterval.isPresent) {
        val optrangingInstanceInterval = rangingInstanceInterval.get()
        put(ContextSpecificTag(TAG_RANGING_INSTANCE_INTERVAL), optrangingInstanceInterval)
      }
      endStructure()
    }
  }

  companion object {
    private const val TAG_START_TIME_DELAY = 0
    private const val TAG_END_TIME_DELAY = 1
    private const val TAG_RANGING_INSTANCE_INTERVAL = 2

    fun fromTlv(
      tlvTag: Tag,
      tlvReader: TlvReader,
    ): ProximityRangingClusterRangingTriggerConditionStruct {
      tlvReader.enterStructure(tlvTag)
      val startTimeDelay = tlvReader.getUInt(ContextSpecificTag(TAG_START_TIME_DELAY))
      val endTimeDelay =
        if (tlvReader.isNextTag(ContextSpecificTag(TAG_END_TIME_DELAY))) {
          Optional.of(tlvReader.getUInt(ContextSpecificTag(TAG_END_TIME_DELAY)))
        } else {
          Optional.empty()
        }
      val rangingInstanceInterval =
        if (tlvReader.isNextTag(ContextSpecificTag(TAG_RANGING_INSTANCE_INTERVAL))) {
          Optional.of(tlvReader.getUInt(ContextSpecificTag(TAG_RANGING_INSTANCE_INTERVAL)))
        } else {
          Optional.empty()
        }

      tlvReader.exitContainer()

      return ProximityRangingClusterRangingTriggerConditionStruct(
        startTimeDelay,
        endTimeDelay,
        rangingInstanceInterval,
      )
    }
  }
}
