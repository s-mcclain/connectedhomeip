/*
 *
 *    Copyright (c) 2026 Project CHIP Authors
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

#include "../Esp32BleRssiRangingAdapter.h"

#include <ble/Ble.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/PlatformManager.h>

#include "host/ble_gap.h"
#include "host/ble_hs.h"

using namespace chip;
using namespace chip::app::Clusters::ProximityRanging;

// --- Session helpers ---

Esp32BleRssiRangingAdapter::SessionState * Esp32BleRssiRangingAdapter::FindFreeSlot()
{
    for (auto & slot : mSessions)
    {
        if (!slot.active)
        {
            return &slot;
        }
    }
    return nullptr;
}

Esp32BleRssiRangingAdapter::SessionState * Esp32BleRssiRangingAdapter::FindSession(uint8_t sessionId)
{
    for (auto & slot : mSessions)
    {
        if (slot.active && slot.sessionId == sessionId)
        {
            return &slot;
        }
    }
    return nullptr;
}

bool Esp32BleRssiRangingAdapter::HasActiveSessions() const
{
    for (const auto & slot : mSessions)
    {
        if (slot.active)
        {
            return true;
        }
    }
    return false;
}

bool Esp32BleRssiRangingAdapter::HasActiveSessionsWithRole(SessionRole role) const
{
    for (const auto & slot : mSessions)
    {
        if (slot.active && slot.role == role)
        {
            return true;
        }
    }
    return false;
}

// --- Session management ---

ResultCodeEnum Esp32BleRssiRangingAdapter::StartSession(
    uint8_t sessionId, const Commands::StartRangingRequest::DecodableType & request)
{
    VerifyOrReturnValue(FindSession(sessionId) == nullptr, ResultCodeEnum::kBusyTryAgainLater);

    SessionState * slot = FindFreeSlot();
    VerifyOrReturnValue(slot != nullptr, ResultCodeEnum::kBusySessionCapacityReached);

    VerifyOrReturnValue(request.BLERangingDeviceRoleConfig.HasValue(), ResultCodeEnum::kRejectedInfeasibleRanging);

    const auto & bleConfig = request.BLERangingDeviceRoleConfig.Value();

    slot->active               = true;
    slot->sessionId            = sessionId;
    slot->peerBleDeviceId      = bleConfig.peerBLEDeviceID;
    slot->trackedMsgCounter    = 0;
    slot->msgCounterInitialized = false;

    if (bleConfig.role == RangingRoleEnum::kBLEBeaconRole)
    {
        slot->role = SessionRole::kAdvertising;

        if (!mAdvertising)
        {
            CHIP_ERROR err = StartAdvertising();
            if (err != CHIP_NO_ERROR)
            {
                ChipLogError(AppServer, "Esp32BleRssiRangingAdapter: Failed to start advertising: %" CHIP_ERROR_FORMAT,
                             err.Format());
                slot->active = false;
                return ResultCodeEnum::kBusyTryAgainLater;
            }
        }
    }
    else
    {
        slot->role = SessionRole::kScanning;

        if (!mScanning)
        {
            CHIP_ERROR err = StartScanning();
            if (err != CHIP_NO_ERROR)
            {
                ChipLogError(AppServer, "Esp32BleRssiRangingAdapter: Failed to start scanning: %" CHIP_ERROR_FORMAT, err.Format());
                slot->active = false;
                return ResultCodeEnum::kBusyTryAgainLater;
            }
        }
    }

    ChipLogProgress(AppServer, "Esp32BleRssiRangingAdapter: Started session %u (role=%s)", sessionId,
                    slot->role == SessionRole::kAdvertising ? "beacon" : "scanner");
    return ResultCodeEnum::kAccepted;
}

CHIP_ERROR Esp32BleRssiRangingAdapter::StopSession(uint8_t sessionId)
{
    SessionState * slot = FindSession(sessionId);
    VerifyOrReturnError(slot != nullptr, CHIP_ERROR_NOT_FOUND);

    SessionRole role = slot->role;
    slot->active     = false;

    ChipLogProgress(AppServer, "Esp32BleRssiRangingAdapter: Stopped session %u", sessionId);

    if (role == SessionRole::kScanning && !HasActiveSessionsWithRole(SessionRole::kScanning) && mScanning)
    {
        ReturnErrorOnFailure(StopScanning());
    }
    else if (role == SessionRole::kAdvertising && !HasActiveSessionsWithRole(SessionRole::kAdvertising) && mAdvertising)
    {
        ReturnErrorOnFailure(StopAdvertising());
    }

    return CHIP_NO_ERROR;
}

void Esp32BleRssiRangingAdapter::StopAllSessions()
{
    for (auto & slot : mSessions)
    {
        slot.active = false;
    }

    if (mScanning)
    {
        (void) StopScanning();
    }
    if (mAdvertising)
    {
        (void) StopAdvertising();
    }

    ChipLogProgress(AppServer, "Esp32BleRssiRangingAdapter: Stopped all sessions");
}

CHIP_ERROR Esp32BleRssiRangingAdapter::GetActiveSessionIds(std::vector<uint8_t> & sessionIds)
{
    sessionIds.clear();
    for (const auto & slot : mSessions)
    {
        if (slot.active)
        {
            sessionIds.push_back(slot.sessionId);
        }
    }
    return CHIP_NO_ERROR;
}

// --- Scanning implementation ---

void Esp32BleRssiRangingAdapter::HandleAdvertisement(const uint8_t * advData, uint8_t advDataLen, int8_t rssi)
{
    // Parse AD structures to find the proximity ranging service data field
    const struct ble_hs_adv_field * field = reinterpret_cast<const struct ble_hs_adv_field *>(advData);
    size_t remaining                      = advDataLen;

    while (field && field->length > 0 && field->length + 1 <= remaining)
    {
        // Check for CHIPoBLE Service Data UUID16 (0xFFF6) with OpCode 0x02 (proximity ranging)
        if (field->type == BLE_HS_ADV_TYPE_SVC_DATA_UUID16 && field->length >= 2 + sizeof(chip::Ble::ChipBLEProximityRangingIdentificationInfo) &&
            field->value[0] == 0xF6 && field->value[1] == 0xFF && field->value[2] == 0x02)
        {
            // Skip the 2-byte UUID prefix, payload starts at value[2]
            chip::Ble::ChipBLEProximityRangingIdentificationInfo payload;
            memcpy(&payload, &field->value[2], sizeof(payload));

            ProcessRangingBeacon(payload, rssi);
            return;
        }

        const uint8_t * next = reinterpret_cast<const uint8_t *>(field) + 1 + field->length;
        remaining -= (1 + field->length);
        field = remaining >= 2 ? reinterpret_cast<const struct ble_hs_adv_field *>(next) : nullptr;
    }
}

void Esp32BleRssiRangingAdapter::ProcessRangingBeacon(
    const chip::Ble::ChipBLEProximityRangingIdentificationInfo & payload, int8_t rssi)
{
    for (auto & slot : mSessions)
    {
        if (!slot.active || slot.role != SessionRole::kScanning)
        {
            continue;
        }

        CHIP_ERROR err = DecodeBeaconPayload(payload, slot.peerBleDeviceId, ByteSpan());
        if (err != CHIP_NO_ERROR)
        {
            // DecodeBeaconPayload logs mismatch details internally
            continue;
        }

        // Message counter replay protection
        uint16_t rxMsgCounter = payload.GetMsgCounter();
        if (slot.msgCounterInitialized && rxMsgCounter < slot.trackedMsgCounter)
        {
            ChipLogDetail(AppServer, "Esp32BleRssiRangingAdapter: Session %u discarding stale msg counter %u (tracked=%u)",
                          slot.sessionId, rxMsgCounter, slot.trackedMsgCounter);
            continue;
        }

        // Update tracked counter
        if (!slot.msgCounterInitialized || rxMsgCounter > slot.trackedMsgCounter)
        {
            slot.trackedMsgCounter    = rxMsgCounter;
            slot.msgCounterInitialized = true;
        }

        // Report RSSI measurement
        Structs::RangingMeasurementDataStruct::Type measurement = {};
        measurement.rssi.SetValue(chip::app::DataModel::Nullable<int8_t>(rssi));

        DeviceLayer::PlatformMgr().LockChipStack();
        if (mCallback)
        {
            mCallback->OnMeasurementData(slot.sessionId, measurement);
        }
        DeviceLayer::PlatformMgr().UnlockChipStack();

        ChipLogDetail(AppServer, "Esp32BleRssiRangingAdapter: Session %u RSSI=%d msgCounter=%u",
                      slot.sessionId, rssi, rxMsgCounter);
        break;
    }
}

int Esp32BleRssiRangingAdapter::NimbleGapEventCallback(struct ble_gap_event * event, void * arg)
{
    auto * adapter = static_cast<Esp32BleRssiRangingAdapter *>(arg);

    switch (event->type)
    {
    case BLE_GAP_EVENT_DISC:
        adapter->HandleAdvertisement(event->disc.data, event->disc.length_data, event->disc.rssi);
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        ChipLogProgress(AppServer, "Esp32BleRssiRangingAdapter: NimBLE scan complete");
        adapter->mScanning = false;
        break;
    default:
        break;
    }

    return 0;
}

CHIP_ERROR Esp32BleRssiRangingAdapter::StartScanning()
{
    VerifyOrReturnError(ble_hs_is_enabled(), CHIP_ERROR_INCORRECT_STATE);

    uint8_t ownAddrType;
    int rc = ble_hs_id_infer_auto(0, &ownAddrType);
    VerifyOrReturnError(rc == 0, CHIP_ERROR_INTERNAL);

    struct ble_gap_disc_params discParams;
    memset(&discParams, 0, sizeof(discParams));
    discParams.filter_duplicates = 0; // Allow repeated readings for RSSI tracking
    discParams.passive           = 1; // Passive scan
    discParams.itvl              = 0; // Use defaults
    discParams.window            = 0;
    discParams.filter_policy     = 0;
    discParams.limited           = 0;

    rc = ble_gap_disc(ownAddrType, BLE_HS_FOREVER, &discParams, NimbleGapEventCallback, this);
    VerifyOrReturnError(rc == 0, CHIP_ERROR_INTERNAL);

    mScanning = true;
    ChipLogProgress(AppServer, "Esp32BleRssiRangingAdapter: NimBLE scanning started");
    return CHIP_NO_ERROR;
}

CHIP_ERROR Esp32BleRssiRangingAdapter::StopScanning()
{
    int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY)
    {
        ChipLogError(AppServer, "Esp32BleRssiRangingAdapter: NimBLE stop scan failed: %d", rc);
        return CHIP_ERROR_INTERNAL;
    }

    mScanning = false;
    ChipLogProgress(AppServer, "Esp32BleRssiRangingAdapter: NimBLE scanning stopped");
    return CHIP_NO_ERROR;
}

// --- Advertising implementation ---

void Esp32BleRssiRangingAdapter::AdvUpdateTimerHandler(chip::System::Layer * layer, void * appState)
{
    auto * adapter = static_cast<Esp32BleRssiRangingAdapter *>(appState);
    if (!adapter->mAdvertising)
    {
        return;
    }

    // Increment message counter and update payload
    adapter->mAdvMsgCounter++;
    CHIP_ERROR err = adapter->UpdateAdvertisingPayload();
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(AppServer, "Esp32BleRssiRangingAdapter: Failed to update adv payload: %" CHIP_ERROR_FORMAT, err.Format());
    }

    // Reschedule
    adapter->ScheduleAdvUpdate();
}

void Esp32BleRssiRangingAdapter::ScheduleAdvUpdate()
{
    DeviceLayer::SystemLayer().StartTimer(System::Clock::Milliseconds32(kAdvUpdateIntervalMs), AdvUpdateTimerHandler, this);
}

void Esp32BleRssiRangingAdapter::CancelAdvUpdate()
{
    DeviceLayer::SystemLayer().CancelTimer(AdvUpdateTimerHandler, this);
}

CHIP_ERROR Esp32BleRssiRangingAdapter::UpdateAdvertisingPayload()
{
    chip::Ble::ChipBLEProximityRangingIdentificationInfo payload;
    constexpr int8_t kDefaultTxPower = 0;

    ReturnErrorOnFailure(EncodeBeaconPayload(GetBleDeviceId(), mAdvMsgCounter, kDefaultTxPower, ByteSpan(), payload));

    constexpr uint8_t kChipAdvDataTypeFlags       = 0x01;
    constexpr uint8_t kChipAdvDataFlags           = 0x06;
    constexpr uint8_t kChipAdvDataTypeServiceData = 0x16;
    constexpr uint8_t kServiceDataTypeSize        = 1; // AD type byte
    constexpr uint16_t kShortUUID_CHIPoBLEService = 0xFFF6;

    uint8_t advData[31] = {};
    uint8_t index        = 0;

    memset(advData, 0, sizeof(advData));
    advData[index++] = 0x02;                                                              // length
    advData[index++] = kChipAdvDataTypeFlags;                                             // AD type : flags
    advData[index++] = kChipAdvDataFlags;                                                 // AD value
    advData[index++] = kServiceDataTypeSize + sizeof(payload) + 2;                        // length (type + UUID + payload)
    advData[index++] = kChipAdvDataTypeServiceData;                                       // AD type: Service Data - 16-bit UUID
    advData[index++] = static_cast<uint8_t>(kShortUUID_CHIPoBLEService & 0xFF);           // UUID low byte
    advData[index++] = static_cast<uint8_t>((kShortUUID_CHIPoBLEService >> 8) & 0xFF);    // UUID high byte
    memcpy(&advData[index], &payload, sizeof(payload));
    index += sizeof(payload);

    int rc = ble_gap_adv_set_data(advData, index);
    VerifyOrReturnError(rc == 0, CHIP_ERROR_INTERNAL);

    ChipLogDetail(AppServer, "Esp32BleRssiRangingAdapter: Updated adv payload, msgCounter=%u", mAdvMsgCounter);
    return CHIP_NO_ERROR;
}

CHIP_ERROR Esp32BleRssiRangingAdapter::StartAdvertising()
{
    VerifyOrReturnError(ble_hs_is_enabled(), CHIP_ERROR_INCORRECT_STATE);

    // Set initial advertising payload
    ReturnErrorOnFailure(UpdateAdvertisingPayload());

    uint8_t ownAddrType;
    int rc = ble_hs_id_infer_auto(0, &ownAddrType);
    VerifyOrReturnError(rc == 0, CHIP_ERROR_INTERNAL);

    struct ble_gap_adv_params advParams;
    memset(&advParams, 0, sizeof(advParams));
    advParams.conn_mode = BLE_GAP_CONN_MODE_NON; // Non-connectable beacon
    advParams.disc_mode = BLE_GAP_DISC_MODE_GEN; // General discoverable
    advParams.itvl_min  = kAdvIntervalMin;       // 300ms
    advParams.itvl_max  = kAdvIntervalMax;       // 500ms

    rc = ble_gap_adv_start(ownAddrType, nullptr, BLE_HS_FOREVER, &advParams, nullptr, nullptr);
    VerifyOrReturnError(rc == 0, CHIP_ERROR_INTERNAL);

    mAdvertising = true;

    // Schedule periodic payload updates (every ~10 advertisements)
    ScheduleAdvUpdate();

    ChipLogProgress(AppServer, "Esp32BleRssiRangingAdapter: NimBLE advertising started (interval %u-%u)",
                    kAdvIntervalMin, kAdvIntervalMax);
    return CHIP_NO_ERROR;
}

CHIP_ERROR Esp32BleRssiRangingAdapter::StopAdvertising()
{
    CancelAdvUpdate();

    int rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY)
    {
        ChipLogError(AppServer, "Esp32BleRssiRangingAdapter: NimBLE stop adv failed: %d", rc);
        return CHIP_ERROR_INTERNAL;
    }

    mAdvertising = false;
    ChipLogProgress(AppServer, "Esp32BleRssiRangingAdapter: NimBLE advertising stopped");
    return CHIP_NO_ERROR;
}
