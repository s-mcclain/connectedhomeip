/*
 *
 *    Copyright (c) 2021 Project CHIP Authors
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

/**
 *    @file
 *          Contains the functions for compatibility with ember ZCL inner state
 *          when calling ember callbacks.
 */

#pragma once

#include <app/Command.h>
#include <app/util/af-types.h>
#include <lib/core/CHIPCore.h>

namespace chip {
namespace app {
namespace Compatibility {

void SetupEmberAfObjects(Command * command, ClusterId clusterId, CommandId commandId, EndpointId endpointId);
bool IMEmberAfSendDefaultResponseWithCallback(EmberAfStatus status);
void ResetEmberAfObjects();

} // namespace Compatibility
} // namespace app
} // namespace chip
