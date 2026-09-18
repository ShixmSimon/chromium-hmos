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

import deviceInfo from '@ohos.deviceInfo';
import display from '@ohos.display';

export enum DeviceType {
  PHONE = 'phone',
  TABLET = 'tablet',
  PC = 'pc'
}

export enum UIProfile {
  PHONE = 'phone',
  TABLET = 'tablet',
  PC = 'pc'
}

export enum UIFamily {
  AURA_PC = 'aura_pc',
  AURA_TABLET = 'aura_tablet',
  MOBILE_PHONE = 'mobile_phone',
  MOBILE_LARGE = 'mobile_large'
}

export enum WindowPolicy {
  FULLSCREEN = 'fullscreen',
  WINDOWED = 'windowed'
}

export enum InputProfile {
  TOUCH = 'touch',
  MIXED = 'mixed',
  REMOTE = 'remote'
}

export enum DeviceClass {
  PHONE = 'phone',
  FOLDABLE = 'foldable',
  TABLET = 'tablet',
  TWO_IN_ONE = '2in1',
  TV = 'tv',
  CAR = 'car'
}

export interface DeviceIdentity {
  deviceType: string;
  marketName: string;
  productModel: string;
  isFoldable: boolean;
  foldStatus: number;
}

export interface WindowStateSnapshot {
  inFreeWindowMode: boolean;
  windowStatus: number;
}

export interface DeviceRoutingContext {
  identity: DeviceIdentity;
  windowState?: WindowStateSnapshot;
}

export interface DeviceRouting {
  uiProfile: UIProfile;
  uiFamily: UIFamily;
  windowPolicy: WindowPolicy;
  inputProfile: InputProfile;
  deviceClass: DeviceClass;
  fullscreenRequested: boolean;
  identity: DeviceIdentity;
}

export interface AuraStartupConfig {
  startUrl: string;
  uiProfile: 'phone' | 'tablet' | 'pc';
  uiFamily: 'aura_pc' | 'aura_tablet' | 'mobile_phone' | 'mobile_large';
  windowMode: 'fullscreen' | 'windowed';
  deviceClass: 'phone' | 'foldable' | 'tablet' | '2in1' | 'tv' | 'car';
  marketName: string;
  productModel: string;
  isFoldable: boolean;
  foldStatus: number;
  displayWidth?: number;
  displayHeight?: number;
  displayDensity?: number;
  userDataDir?: string;
  printOutputDir?: string;
  resourcesDir?: string;
  applicationLocale?: string;
  colorScheme: 'light' | 'dark';
  icuDataFd?: number;
  icuDataOffset?: number;
  icuDataLength?: number;
  jitless: boolean;
  fullscreenRequested: boolean;
  windowRole?: 'browser' | 'pwa';
  targetWidget?: number;
  pwaAppId?: string;
}

export const CHROMIUM_HOME_URL: string = 'chrome://newtab/';

const TV_MARKET_KEYWORDS: string[] = [
  'MATETV',
  '\u667a\u6167\u5c4f',
  'SMART SCREEN'
];
const MATEPAD_KEYWORDS: string[] = [ 'MATEPAD' ];
const MATEBOOK_KEYWORDS: string[] = [ 'MATEBOOK' ];

function normalizeText(value: string): string {
  return (value ?? '').trim().toUpperCase();
}

export class DeviceRoutingPolicy {
  static readIdentity(foldStatusOverride?: number): DeviceIdentity {
    const foldStatus = foldStatusOverride === undefined ?
      DeviceRoutingPolicy.getFoldStatus() : foldStatusOverride;
    return {
      deviceType: deviceInfo.deviceType ?? 'phone',
      marketName: deviceInfo.marketName ?? '',
      productModel: deviceInfo.productModel ?? '',
      isFoldable: DeviceRoutingPolicy.isFoldable(),
      foldStatus
    };
  }

  static resolve(windowState?: WindowStateSnapshot, foldStatusOverride?: number): DeviceRouting {
    return DeviceRoutingPolicy.resolveFromIdentity(
      DeviceRoutingPolicy.readIdentity(foldStatusOverride), windowState);
  }

  static resolveFromContext(context: DeviceRoutingContext): DeviceRouting {
    return DeviceRoutingPolicy.resolveFromIdentity(context.identity, context.windowState);
  }

  static resolveFromIdentity(identity: DeviceIdentity, windowState?: WindowStateSnapshot): DeviceRouting {
    const deviceClass = DeviceRoutingPolicy.resolveDeviceClass(identity);
    const uiProfile = DeviceRoutingPolicy.resolveUiProfile(identity);
    const uiFamily = DeviceRoutingPolicy.resolveUiFamily(identity, deviceClass, uiProfile);
    const inputProfile = DeviceRoutingPolicy.resolveInputProfile(deviceClass, uiProfile);
    const windowPolicy = DeviceRoutingPolicy.resolveWindowPolicy(identity, windowState);

    return {
      uiProfile,
      uiFamily,
      windowPolicy,
      inputProfile,
      deviceClass,
      fullscreenRequested: windowPolicy === WindowPolicy.FULLSCREEN,
      identity
    };
  }

  static createAuraStartupConfig(
    startUrl: string,
    windowState?: WindowStateSnapshot,
    foldStatusOverride?: number,
    colorScheme: 'light' | 'dark' = 'light'
  ): AuraStartupConfig {
    const routing = DeviceRoutingPolicy.resolve(windowState, foldStatusOverride);
    return {
      startUrl: startUrl && startUrl.trim().length > 0 ? startUrl : CHROMIUM_HOME_URL,
      uiProfile: routing.uiProfile,
      uiFamily: routing.uiFamily,
      windowMode: routing.windowPolicy,
      deviceClass: routing.deviceClass,
      marketName: routing.identity.marketName,
      productModel: routing.identity.productModel,
      isFoldable: routing.identity.isFoldable,
      foldStatus: routing.identity.foldStatus,
      colorScheme,
      jitless: false,
      fullscreenRequested: routing.fullscreenRequested
    };
  }

  static buildRoutingSignature(routing: DeviceRouting): string {
    return [
      routing.uiProfile,
      routing.uiFamily,
      routing.windowPolicy,
      routing.inputProfile,
      routing.deviceClass,
      routing.identity.deviceType,
      routing.identity.marketName,
      routing.identity.productModel,
      routing.identity.foldStatus.toString()
    ].join('|');
  }

  static isFoldable(): boolean {
    try {
      return display.isFoldable();
    } catch (error) {
      return false;
    }
  }

  static getFoldStatus(): number {
    if (!DeviceRoutingPolicy.isFoldable()) {
      return -1;
    }

    try {
      return Number(display.getFoldStatus());
    } catch (error) {
      return -1;
    }
  }

  static isExpandedFold(identity?: DeviceIdentity): boolean {
    const source = identity ?? DeviceRoutingPolicy.readIdentity();
    if (!source.isFoldable) {
      return false;
    }

    const folded = display.FoldStatus.FOLD_STATUS_FOLDED;
    const unknown = display.FoldStatus.FOLD_STATUS_UNKNOWN;

    if (source.foldStatus === -1 || source.foldStatus === unknown) {
      return false;
    }

    return source.foldStatus !== folded;
  }

  private static resolveUiProfile(identity: DeviceIdentity): UIProfile {
    if (DeviceRoutingPolicy.isMateBook(identity) || identity.deviceType === '2in1' || identity.deviceType === 'desktop') {
      return UIProfile.PC;
    }

    if (DeviceRoutingPolicy.isMatePad(identity)) {
      return UIProfile.TABLET;
    }

    if (DeviceRoutingPolicy.isTv(identity) || DeviceRoutingPolicy.isCar(identity)) {
      return UIProfile.TABLET;
    }

    if (DeviceRoutingPolicy.isExpandedFold(identity)) {
      return UIProfile.TABLET;
    }

    if (identity.deviceType === 'phone') {
      return UIProfile.PHONE;
    }

    if (identity.deviceType === 'tablet') {
      return UIProfile.TABLET;
    }

    return UIProfile.PHONE;
  }

  private static resolveUiFamily(identity: DeviceIdentity, deviceClass: DeviceClass, uiProfile: UIProfile): UIFamily {
    if (uiProfile === UIProfile.PC || deviceClass === DeviceClass.TWO_IN_ONE) {
      return UIFamily.AURA_PC;
    }

    if (deviceClass === DeviceClass.TV || deviceClass === DeviceClass.CAR || DeviceRoutingPolicy.isExpandedFold(identity)) {
      return UIFamily.MOBILE_LARGE;
    }

    if (DeviceRoutingPolicy.isMatePad(identity) || identity.deviceType === 'tablet') {
      return UIFamily.AURA_TABLET;
    }

    if (deviceClass === DeviceClass.FOLDABLE && !DeviceRoutingPolicy.isExpandedFold(identity)) {
      return UIFamily.MOBILE_PHONE;
    }

    return identity.deviceType === 'phone' ? UIFamily.MOBILE_PHONE : UIFamily.AURA_TABLET;
  }

  private static resolveWindowPolicy(identity: DeviceIdentity, windowState?: WindowStateSnapshot): WindowPolicy {
    const inFreeWindowMode = windowState?.inFreeWindowMode ?? false;

    if (identity.deviceType === 'phone' && !DeviceRoutingPolicy.isExpandedFold(identity)) {
      return WindowPolicy.FULLSCREEN;
    }

    if (DeviceRoutingPolicy.isTv(identity)) {
      return WindowPolicy.FULLSCREEN;
    }

    if (DeviceRoutingPolicy.isMateBook(identity) || identity.deviceType === '2in1' || identity.deviceType === 'desktop') {
      return WindowPolicy.WINDOWED;
    }

    if (DeviceRoutingPolicy.isCar(identity)) {
      return WindowPolicy.FULLSCREEN;
    }

    if (DeviceRoutingPolicy.isMatePad(identity)) {
      return inFreeWindowMode ? WindowPolicy.WINDOWED : WindowPolicy.FULLSCREEN;
    }

    if (identity.deviceType === 'tablet' || DeviceRoutingPolicy.isExpandedFold(identity)) {
      return inFreeWindowMode ? WindowPolicy.WINDOWED : WindowPolicy.FULLSCREEN;
    }

    return WindowPolicy.FULLSCREEN;
  }

  private static resolveInputProfile(deviceClass: DeviceClass, uiProfile: UIProfile): InputProfile {
    if (deviceClass === DeviceClass.TV || deviceClass === DeviceClass.CAR) {
      return InputProfile.REMOTE;
    }

    if (uiProfile === UIProfile.PC || deviceClass === DeviceClass.TWO_IN_ONE) {
      return InputProfile.MIXED;
    }

    return InputProfile.TOUCH;
  }

  private static resolveDeviceClass(identity: DeviceIdentity): DeviceClass {
    if (DeviceRoutingPolicy.isTv(identity)) {
      return DeviceClass.TV;
    }

    if (DeviceRoutingPolicy.isCar(identity)) {
      return DeviceClass.CAR;
    }

    if (identity.deviceType === '2in1' || identity.deviceType === 'desktop' || DeviceRoutingPolicy.isMateBook(identity)) {
      return DeviceClass.TWO_IN_ONE;
    }

    if (identity.isFoldable) {
      return DeviceClass.FOLDABLE;
    }

    if (identity.deviceType === 'tablet') {
      return DeviceClass.TABLET;
    }

    return DeviceClass.PHONE;
  }

  private static isTv(identity: DeviceIdentity): boolean {
    if (identity.deviceType === 'tv') {
      return true;
    }

    const marketName = normalizeText(identity.marketName);
    return TV_MARKET_KEYWORDS.some((keyword: string) => marketName.includes(keyword));
  }

  private static isCar(identity: DeviceIdentity): boolean {
    return identity.deviceType === 'car';
  }

  private static isMatePad(identity: DeviceIdentity): boolean {
    const name = `${normalizeText(identity.marketName)} ${normalizeText(identity.productModel)}`;
    return MATEPAD_KEYWORDS.some((keyword: string) => name.includes(keyword));
  }

  private static isMateBook(identity: DeviceIdentity): boolean {
    const name = `${normalizeText(identity.marketName)} ${normalizeText(identity.productModel)}`;
    return MATEBOOK_KEYWORDS.some((keyword: string) => name.includes(keyword));
  }

}

export class DeviceTypeDetector {
  static getDeviceType(): DeviceType {
    const routing = DeviceRoutingPolicy.resolve();
    switch (routing.uiProfile) {
      case UIProfile.PC:
        return DeviceType.PC;
      case UIProfile.TABLET:
        return DeviceType.TABLET;
      case UIProfile.PHONE:
      default:
        return DeviceType.PHONE;
    }
  }

  static getUIProfile(): UIProfile {
    return DeviceRoutingPolicy.resolve().uiProfile;
  }

  static isPhone(): boolean {
    return DeviceTypeDetector.getDeviceType() === DeviceType.PHONE;
  }

  static isTablet(): boolean {
    return DeviceTypeDetector.getDeviceType() === DeviceType.TABLET;
  }

  static isPC(): boolean {
    return DeviceTypeDetector.getDeviceType() === DeviceType.PC;
  }

  static isTV(): boolean {
    return DeviceRoutingPolicy.resolve().deviceClass === DeviceClass.TV;
  }

  static isAuto(): boolean {
    return DeviceRoutingPolicy.resolve().deviceClass === DeviceClass.CAR;
  }

  static isFoldable(): boolean {
    return DeviceRoutingPolicy.isFoldable();
  }
}
