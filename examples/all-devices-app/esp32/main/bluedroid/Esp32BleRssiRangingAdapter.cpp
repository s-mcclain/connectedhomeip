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

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"

using namespace chip;
using namespace chip::app::Clusters::ProximityRanging;

Esp32BleRssiRangingAdapter * Esp32BleRssiRangingAdapter::sInstance = nullptr;

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
    uint8_t offset = 0;

    while (offset < advDataLen)
    {
        uint8_t fieldLen = advData[offset];
        if (fieldLen == 0 || offset + 1 + fieldLen > advDataLen)
        {
            break;
        }

        uint8_t fieldType       = advData[offset + 1];
        const uint8_t * fieldValue = &advData[offset + 2];
        uint8_t fieldValueLen   = fieldLen - 1;

        // Check for CHIPoBLE Service Data UUID16 (0xFFF6) with OpCode 0x02 (proximity ranging)
        if (fieldType == 0x16 && fieldValueLen >= 2 + sizeof(chip::Ble::ChipBLEProximityRangingIdentificationInfo) &&
            fieldValue[0] == 0xF6 && fieldValue[1] == 0xFF && fieldValue[2] == 0x02)
        {
            chip::Ble::ChipBLEProximityRangingIdentificationInfo payload;
            memcpy(&payload, &fieldValue[2], sizeof(payload));

            ProcessRangingBeacon(payload, rssi);
            return;
        }

        offset += 1 + fieldLen;
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

        if (!slot.msgCounterInitialized || rxMsgCounter > slot.trackedMsgCounter)
        {
            slot.trackedMsgCounter    = rxMsgCounter;
            slot.msgCounterInitialized = true;
        }

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

void Esp32BleRssiRangingAdapter::BluedroidGapEventCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t * param)
{
    if (sInstance == nullptr)
    {
        return;
    }

    switch (event)
    {
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT)
        {
            sInstance->HandleAdvertisement(param->scan_rst.ble_adv, param->scan_rst.adv_data_len,
                                           static_cast<int8_t>(param->scan_rst.rssi));
        }
        else if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT)
        {
            ChipLogProgress(AppServer, "Esp32BleRssiRangingAdapter: Bluedroid scan complete");
            sInstance->mScanning = false;
        }
        break;
    }
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: {
        esp_ble_gap_start_scanning(UINT32_MAX);
        break;
    }
    default:
        break;
    }
}

CHIP_ERROR Esp32BleRssiRangingAdapter::StartScanning()
{
    sInstance = this;

    esp_ble_scan_params_t scanParams;
    memset(&scanParams, 0, sizeof(scanParams));
    scanParams.scan_type          = BLE_SCAN_TYPE_PASSIVE;
    scanParams.own_addr_type      = BLE_ADDR_TYPE_PUBLIC;
    scanParams.scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
    scanParams.scan_interval      = 0x50;
    scanParams.scan_window        = 0x30;
    scanParams.scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE;

    esp_err_t err = esp_ble_gap_register_callback(BluedroidGapEventCallback);
    VerifyOrReturnError(err == ESP_OK, CHIP_ERROR_INTERNAL);

    err = esp_ble_gap_set_scan_params(&scanParams);
    VerifyOrReturnError(err == ESP_OK, CHIP_ERROR_INTERNAL);

    mScanning = true;
    ChipLogProgress(AppServer, "Esp32BleRssiRangingAdapter: Bluedroid scanning started");
    return CHIP_NO_ERROR;
}

CHIP_ERROR Esp32BleRssiRangingAdapter::StopScanning()
{
    esp_err_t err = esp_ble_gap_stop_scanning();
    if (err != ESP_OK)
    {
        ChipLogError(AppServer, "Esp32BleRssiRangingAdapter: Bluedroid stop scan failed: %d", err);
        return CHIP_ERROR_INTERNAL;
    }

    mScanning = false;
    ChipLogProgress(AppServer, "Esp32BleRssiRangingAdapter: Bluedroid scanning stopped");
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

    adapter->mAdvMsgCounter++;
    CHIP_ERROR err = adapter->UpdateAdvertisingPayload();
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(AppServer, "Esp32BleRssiRangingAdapter: Failed to update adv payload: %" CHIP_ERROR_FORMAT, err.Format());
    }

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

    esp_err_t err = esp_ble_gap_config_adv_data_raw(advData, index);
    VerifyOrReturnError(err == ESP_OK, CHIP_ERROR_INTERNAL);

    ChipLogDetail(AppServer, "Esp32BleRssiRangingAdapter: Updated adv payload, msgCounter=%u", mAdvMsgCounter);
    return CHIP_NO_ERROR;
}

CHIP_ERROR Esp32BleRssiRangingAdapter::StartAdvertising()
{
    sInstance = this;

    ReturnErrorOnFailure(UpdateAdvertisingPayload());

    esp_ble_adv_params_t advParams;
    memset(&advParams, 0, sizeof(advParams));
    advParams.adv_int_min       = kAdvIntervalMin;
    advParams.adv_int_max       = kAdvIntervalMax;
    advParams.adv_type          = ADV_TYPE_NONCONN_IND;
    advParams.own_addr_type     = BLE_ADDR_TYPE_PUBLIC;
    advParams.channel_map       = ADV_CHNL_ALL;
    advParams.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;

    esp_err_t err = esp_ble_gap_start_advertising(&advParams);
    VerifyOrReturnError(err == ESP_OK, CHIP_ERROR_INTERNAL);

    mAdvertising = true;
    ScheduleAdvUpdate();

    ChipLogProgress(AppServer, "Esp32BleRssiRangingAdapter: Bluedroid advertising started (interval %u-%u)",
                    kAdvIntervalMin, kAdvIntervalMax);
    return CHIP_NO_ERROR;
}

CHIP_ERROR Esp32BleRssiRangingAdapter::StopAdvertising()
{
    CancelAdvUpdate();

    esp_err_t err = esp_ble_gap_stop_advertising();
    if (err != ESP_OK)
    {
        ChipLogError(AppServer, "Esp32BleRssiRangingAdapter: Bluedroid stop adv failed: %d", err);
        return CHIP_ERROR_INTERNAL;
    }

    mAdvertising = false;
    ChipLogProgress(AppServer, "Esp32BleRssiRangingAdapter: Bluedroid advertising stopped");
    return CHIP_NO_ERROR;
}
