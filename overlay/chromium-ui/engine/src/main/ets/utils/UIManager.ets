/*
 * Copyright (c) 2024 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import {
  AuraStartupConfig,
  DeviceRouting,
  DeviceRoutingPolicy,
  DeviceType,
  DeviceTypeDetector,
  WindowStateSnapshot
} from './DeviceType';

export class UIManager {
  static readonly AURA_SHELL_PAGE: string = 'pages/AuraShell';

  static loadUI(): string {
    return UIManager.AURA_SHELL_PAGE;
  }

  static getDeviceType(): DeviceType {
    return DeviceTypeDetector.getDeviceType();
  }

  static getRouting(windowState?: WindowStateSnapshot, foldStatusOverride?: number): DeviceRouting {
    return DeviceRoutingPolicy.resolve(windowState, foldStatusOverride);
  }

  static buildAuraStartupConfig(
    startUrl: string,
    windowState?: WindowStateSnapshot,
    foldStatusOverride?: number,
    colorScheme: 'light' | 'dark' = 'light'
  ): AuraStartupConfig {
    return DeviceRoutingPolicy.createAuraStartupConfig(
      startUrl, windowState, foldStatusOverride, colorScheme);
  }

  static serializeAuraStartupConfig(
    startUrl: string,
    windowState?: WindowStateSnapshot,
    foldStatusOverride?: number,
    colorScheme: 'light' | 'dark' = 'light'
  ): string {
    return JSON.stringify(UIManager.buildAuraStartupConfig(
      startUrl, windowState, foldStatusOverride, colorScheme));
  }
}
