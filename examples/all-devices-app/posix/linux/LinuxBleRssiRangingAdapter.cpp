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

#include "LinuxBleRssiRangingAdapter.h"

#include <ble/BleUUID.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/PlatformManager.h>

using namespace chip;
using namespace chip::app::Clusters::ProximityRanging;
using namespace chip::DeviceLayer;
using namespace chip::DeviceLayer::Internal;

// --- Session helpers ---

LinuxBleRssiRangingAdapter::SessionState * LinuxBleRssiRangingAdapter::FindFreeSlot()
{
    for (auto & slot : mSessions)
    {
        if (!slot.active)
            return &slot;
    }
    return nullptr;
}

LinuxBleRssiRangingAdapter::SessionState * LinuxBleRssiRangingAdapter::FindSession(uint8_t sessionId)
{
    for (auto & slot : mSessions)
    {
        if (slot.active && slot.sessionId == sessionId)
            return &slot;
    }
    return nullptr;
}

bool LinuxBleRssiRangingAdapter::HasActiveSessionsWithRole(SessionRole role) const
{
    for (const auto & slot : mSessions)
    {
        if (slot.active && slot.role == role)
            return true;
    }
    return false;
}

// --- BLE lifecycle ---

CHIP_ERROR LinuxBleRssiRangingAdapter::EnsureBleInitialized()
{
    VerifyOrReturnError(!mBleInitialized, CHIP_NO_ERROR);

    ReturnErrorOnFailure(mObjectManager.Init());

    BluezAdapter1 * adapter = mObjectManager.GetAdapter(0u);
    VerifyOrReturnError(adapter != nullptr, CHIP_ERROR_INTERNAL,
                        ChipLogError(AppServer, "LinuxBleRssiRangingAdapter: No BLE adapter found"));

    // Initialize endpoint (needed by BluezAdvertisement constructor reference).
    // Use isCentral=true since we don't need GATT services for broadcast beacons.
    ReturnErrorOnFailure(mEndpoint.Init(adapter, true /* isCentral */));

    // Initialize advertisement once (broadcast, non-connectable)
    ReturnErrorOnFailure(mAdvertisement.Init(adapter, Ble::CHIP_BLE_SERVICE_LONG_UUID_STR, "CHIPProxRng", true /* broadcast */));
    mAdvertisement.SetDelegate(this);
    // BlueZ with --experimental rejects MinInterval/MaxInterval of 0. Set valid values.
    (void) mAdvertisement.SetIntervals({ kAdvIntervalMin, kAdvIntervalMax });

    mBleInitialized = true;
    return CHIP_NO_ERROR;
}

// --- Session management ---

ResultCodeEnum LinuxBleRssiRangingAdapter::StartSession(
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
                ChipLogError(AppServer, "LinuxBleRssiRangingAdapter: Failed to start advertising: %" CHIP_ERROR_FORMAT,
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
                ChipLogError(AppServer, "LinuxBleRssiRangingAdapter: Failed to start scanning: %" CHIP_ERROR_FORMAT, err.Format());
                slot->active = false;
                return ResultCodeEnum::kBusyTryAgainLater;
            }
        }
    }

    ChipLogProgress(AppServer, "LinuxBleRssiRangingAdapter: Started session %u (role=%s)", sessionId,
                    slot->role == SessionRole::kAdvertising ? "beacon" : "scanner");
    return ResultCodeEnum::kAccepted;
}

CHIP_ERROR LinuxBleRssiRangingAdapter::StopSession(uint8_t sessionId)
{
    SessionState * slot = FindSession(sessionId);
    VerifyOrReturnError(slot != nullptr, CHIP_ERROR_NOT_FOUND);

    SessionRole role = slot->role;
    slot->active     = false;

    ChipLogProgress(AppServer, "LinuxBleRssiRangingAdapter: Stopped session %u", sessionId);

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

void LinuxBleRssiRangingAdapter::StopAllSessions()
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
    ChipLogProgress(AppServer, "LinuxBleRssiRangingAdapter: Stopped all sessions");
}

CHIP_ERROR LinuxBleRssiRangingAdapter::GetActiveSessionIds(std::vector<uint8_t> & sessionIds)
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

// --- Scanning ---

CHIP_ERROR LinuxBleRssiRangingAdapter::StartScanning()
{
    ReturnErrorOnFailure(EnsureBleInitialized());

    BluezAdapter1 * adapter = mObjectManager.GetAdapter(0u);
    VerifyOrReturnError(adapter != nullptr, CHIP_ERROR_INTERNAL);

    ReturnErrorOnFailure(mScanner.Init(adapter, this));
    ReturnErrorOnFailure(mScanner.StartScan());

    mScanning = true;
    ChipLogProgress(AppServer, "LinuxBleRssiRangingAdapter: Scanning started");
    return CHIP_NO_ERROR;
}

CHIP_ERROR LinuxBleRssiRangingAdapter::StopScanning()
{
    CHIP_ERROR err = mScanner.StopScan();
    mScanning = false;
    ChipLogProgress(AppServer, "LinuxBleRssiRangingAdapter: Scanning stopped");
    return err;
}

void LinuxBleRssiRangingAdapter::OnDeviceScanned(BluezDevice1 & device,
                                                  const Ble::ChipBLEProximityRangingIdentificationInfo & info, int8_t rssi)
{
    ProcessRangingBeacon(info, rssi);
}

void LinuxBleRssiRangingAdapter::OnScanError(CHIP_ERROR err)
{
    ChipLogError(AppServer, "LinuxBleRssiRangingAdapter: Scan error: %" CHIP_ERROR_FORMAT, err.Format());
    mScanning = false;
}

void LinuxBleRssiRangingAdapter::ProcessRangingBeacon(
    const Ble::ChipBLEProximityRangingIdentificationInfo & payload, int8_t rssi)
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
            ChipLogDetail(AppServer, "LinuxBleRssiRangingAdapter: Session %u discarding stale msg counter %u (tracked=%u)",
                          slot.sessionId, rxMsgCounter, slot.trackedMsgCounter);
            continue;
        }

        if (!slot.msgCounterInitialized || rxMsgCounter > slot.trackedMsgCounter)
        {
            slot.trackedMsgCounter    = rxMsgCounter;
            slot.msgCounterInitialized = true;
        }

        // Report RSSI measurement
        Structs::RangingMeasurementDataStruct::Type measurement = {};
        measurement.rssi.SetValue(app::DataModel::Nullable<int8_t>(rssi));

        PlatformMgr().LockChipStack();
        if (mCallback)
        {
            mCallback->OnMeasurementData(slot.sessionId, measurement);
        }
        PlatformMgr().UnlockChipStack();

        ChipLogDetail(AppServer, "LinuxBleRssiRangingAdapter: Session %u RSSI=%d msgCounter=%u",
                      slot.sessionId, rssi, rxMsgCounter);
        break;
    }
}

// --- Advertising ---

CHIP_ERROR LinuxBleRssiRangingAdapter::StartAdvertising()
{
    ReturnErrorOnFailure(EnsureBleInitialized());

    // Set initial payload and start
    ReturnErrorOnFailure(UpdateAdvertisingPayload());
    ReturnErrorOnFailure(mAdvertisement.Start());

    mAdvertising = true;
    ScheduleAdvUpdate();

    ChipLogProgress(AppServer, "LinuxBleRssiRangingAdapter: Advertising started");
    return CHIP_NO_ERROR;
}

CHIP_ERROR LinuxBleRssiRangingAdapter::StopAdvertising()
{
    CancelAdvUpdate();
    CHIP_ERROR err = mAdvertisement.Stop();
    mAdvertising = false;
    ChipLogProgress(AppServer, "LinuxBleRssiRangingAdapter: Advertising stopped");
    return err;
}

CHIP_ERROR LinuxBleRssiRangingAdapter::UpdateAdvertisingPayload()
{
    Ble::ChipBLEProximityRangingIdentificationInfo payload;
    constexpr int8_t kDefaultTxPower = 0;
    ReturnErrorOnFailure(EncodeBeaconPayload(GetBleDeviceId(), mAdvMsgCounter, kDefaultTxPower, ByteSpan(), payload));

    return mAdvertisement.SetServiceDataRaw(ByteSpan(reinterpret_cast<const uint8_t *>(&payload), sizeof(payload)));
}

void LinuxBleRssiRangingAdapter::OnAdvertisementReleased()
{
    ChipLogProgress(AppServer, "LinuxBleRssiRangingAdapter: Advertisement released by BlueZ");
    mAdvertising = false;
}

void LinuxBleRssiRangingAdapter::AdvUpdateTimerHandler(System::Layer * layer, void * appState)
{
    auto * self = static_cast<LinuxBleRssiRangingAdapter *>(appState);
    if (!self->mAdvertising)
    {
        return;
    }

    self->mAdvMsgCounter++;
    CHIP_ERROR err = self->UpdateAdvertisingPayload();
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(AppServer, "LinuxBleRssiRangingAdapter: Failed to update adv payload: %" CHIP_ERROR_FORMAT, err.Format());
    }
    self->ScheduleAdvUpdate();
}

void LinuxBleRssiRangingAdapter::ScheduleAdvUpdate()
{
    SystemLayer().StartTimer(System::Clock::Milliseconds32(kAdvUpdateIntervalMs), AdvUpdateTimerHandler, this);
}

void LinuxBleRssiRangingAdapter::CancelAdvUpdate()
{
    SystemLayer().CancelTimer(AdvUpdateTimerHandler, this);
}
