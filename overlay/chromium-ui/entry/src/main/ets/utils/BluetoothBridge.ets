/*
 * Copyright 2026 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

import { abilityAccessCtrl, common, Permissions } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import access from '@ohos.bluetooth.access';
import ble from '@ohos.bluetooth.ble';
import constant from '@ohos.bluetooth.constant';

export interface BluetoothNativePort {
  SetBluetoothEventCallback(componentId: string, callback: (commandJson: string) => void): void;
  CompleteBluetoothMessage(componentId: string, messageJson: string): void;
}

interface BluetoothCommand {
  id: number;
  method: string;
  address?: string;
  characteristicId?: string;
  descriptorId?: string;
  enabled?: boolean;
  withResponse?: boolean;
  value?: number[];
}

interface DescriptorInfo {
  identifier: string;
  uuid: string;
}

interface CharacteristicInfo {
  identifier: string;
  uuid: string;
  properties: number;
  descriptors: DescriptorInfo[];
}

interface ServiceInfo {
  identifier: string;
  uuid: string;
  primary: boolean;
  characteristics: CharacteristicInfo[];
}

interface BluetoothReply {
  id: number;
  ok: boolean;
  error?: string;
  errorCode?: number;
  powered?: boolean;
  services?: ServiceInfo[];
  value?: number[];
}

interface ServiceDataInfo {
  uuid: string;
  value: number[];
}

interface ManufacturerDataInfo {
  id: number;
  value: number[];
}

interface ScanAdvertisement {
  flags?: number;
  txPower?: number;
  uuids: string[];
  serviceData: ServiceDataInfo[];
  manufacturerData: ManufacturerDataInfo[];
}

interface BluetoothEvent {
  event: string;
  address?: string;
  connected?: boolean;
  powered?: boolean;
  name?: string;
  rssi?: number;
  connectable?: boolean;
  flags?: number;
  txPower?: number;
  uuids?: string[];
  serviceData?: ServiceDataInfo[];
  manufacturerData?: ManufacturerDataInfo[];
  characteristicId?: string;
  value?: number[];
}

type ScanCallback = (results: ble.ScanResult[]) => void;
type ConnectionCallback = (state: ble.BLEConnectionChangeState) => void;
type CharacteristicCallback = (characteristic: ble.BLECharacteristic) => void;

const ACCESS_BLUETOOTH = 'ohos.permission.ACCESS_BLUETOOTH' as Permissions;

/** Executes BLE operations on the ArkTS Ability thread for Chromium's backend. */
export class BluetoothBridge {
  private readonly devices: Map<string, ble.GattClientDevice> = new Map();
  private readonly characteristics: Map<string, ble.BLECharacteristic> = new Map();
  private readonly descriptors: Map<string, ble.BLEDescriptor> = new Map();
  private readonly connectionCallbacks: Map<string, ConnectionCallback> = new Map();
  private readonly characteristicCallbacks: Map<string, CharacteristicCallback> = new Map();
  private readonly notifying: Set<string> = new Set();
  private readonly context: common.UIAbilityContext;
  private readonly componentId: string;
  private readonly nativePort: BluetoothNativePort;
  private scanCallback: ScanCallback | undefined = undefined;
  private scanning: boolean = false;

  private readonly stateCallback = (state: access.BluetoothState): void => {
    this.sendEvent({
      event: 'adapterState',
      powered: state === access.BluetoothState.STATE_ON ||
        state === access.BluetoothState.STATE_BLE_ON
    });
  };

  constructor(
    context: common.UIAbilityContext,
    componentId: string,
    nativePort: BluetoothNativePort
  ) {
    this.context = context;
    this.componentId = componentId;
    this.nativePort = nativePort;
    this.nativePort.SetBluetoothEventCallback(
      this.componentId,
      (commandJson: string): void => this.onCommand(commandJson)
    );
    try {
      access.on('stateChange', this.stateCallback);
    } catch (error) {
      console.error(`Bluetooth state monitor failed: ${String(error)}`);
    }
  }

  destroy(): void {
    if (this.scanning) {
      try {
        ble.stopBLEScan();
      } catch (_error) {
      }
    }
    if (this.scanCallback) {
      try {
        ble.off('BLEDeviceFind', this.scanCallback);
      } catch (_error) {
      }
    }
    try {
      access.off('stateChange', this.stateCallback);
    } catch (_error) {
    }
    this.devices.forEach((device: ble.GattClientDevice): void => {
      try {
        device.disconnect();
        device.close();
      } catch (_error) {
      }
    });
    this.devices.clear();
    this.characteristics.clear();
    this.descriptors.clear();
    this.connectionCallbacks.clear();
    this.characteristicCallbacks.clear();
    this.notifying.clear();
  }

  private onCommand(commandJson: string): void {
    let command: BluetoothCommand;
    try {
      command = JSON.parse(commandJson) as BluetoothCommand;
    } catch (error) {
      console.error(`Invalid Bluetooth command: ${String(error)}`);
      return;
    }
    this.execute(command).catch((error: Error): void => {
      this.sendError(command.id, error);
    });
  }

  private async execute(command: BluetoothCommand): Promise<void> {
    switch (command.method) {
      case 'getAdapterState':
        this.getAdapterState(command.id);
        return;
      case 'startScan':
        await this.startScan(command.id);
        return;
      case 'stopScan':
        this.stopScan(command.id);
        return;
      case 'connect':
        this.connect(command.id, command.address ?? '');
        return;
      case 'disconnect':
        this.disconnect(command.id, command.address ?? '');
        return;
      case 'getServices':
        await this.getServices(command.id, command.address ?? '');
        return;
      case 'readCharacteristic':
        await this.readCharacteristic(command.id, command.characteristicId ?? '');
        return;
      case 'writeCharacteristic':
        await this.writeCharacteristic(command);
        return;
      case 'readDescriptor':
        await this.readDescriptor(command.id, command.descriptorId ?? '');
        return;
      case 'writeDescriptor':
        await this.writeDescriptor(command);
        return;
      case 'setNotify':
        await this.setNotify(command);
        return;
      default:
        throw new Error(`Unknown Bluetooth method: ${command.method}`);
    }
  }

  private getAdapterState(id: number): void {
    const state = access.getState();
    this.sendReply({
      id: id,
      ok: true,
      powered: state === access.BluetoothState.STATE_ON ||
        state === access.BluetoothState.STATE_BLE_ON
    });
  }

  private async startScan(id: number): Promise<void> {
    if (!await this.ensurePermission()) {
      this.sendReply({ id: id, ok: false, error: 'Bluetooth permission denied', errorCode: 201 });
      return;
    }
    if (!this.scanCallback) {
      this.scanCallback = (results: ble.ScanResult[]): void => {
        results.forEach((result: ble.ScanResult): void => this.onScanResult(result));
      };
      ble.on('BLEDeviceFind', this.scanCallback);
    }
    if (!this.scanning) {
      const filter: ble.ScanFilter = {};
      const options: ble.ScanOptions = {
        interval: 500,
        dutyMode: ble.ScanDuty.SCAN_MODE_LOW_POWER,
        matchMode: ble.MatchMode.MATCH_MODE_AGGRESSIVE
      };
      ble.startBLEScan([filter], options);
      this.scanning = true;
    }
    this.sendReply({ id: id, ok: true });
  }

  private stopScan(id: number): void {
    if (this.scanning) {
      ble.stopBLEScan();
      this.scanning = false;
    }
    this.sendReply({ id: id, ok: true });
  }

  private connect(id: number, address: string): void {
    if (!address) {
      throw new Error('Missing Bluetooth address');
    }
    let device = this.devices.get(address);
    if (!device) {
      device = ble.createGattClientDevice(address);
      const callback: ConnectionCallback =
        (state: ble.BLEConnectionChangeState): void => {
          const connected = state.state === constant.ProfileConnectionState.STATE_CONNECTED;
          this.sendEvent({ event: 'connectionState', address: address, connected: connected });
          if (state.state === constant.ProfileConnectionState.STATE_DISCONNECTED) {
            this.clearDeviceAttributes(address);
          }
        };
      device.on('BLEConnectionStateChange', callback);
      this.connectionCallbacks.set(address, callback);
      this.devices.set(address, device);
    }
    device.connect();
    this.sendReply({ id: id, ok: true });
  }

  private disconnect(id: number, address: string): void {
    const device = this.requireDevice(address);
    device.disconnect();
    this.sendReply({ id: id, ok: true });
  }

  private async getServices(id: number, address: string): Promise<void> {
    const device = this.requireDevice(address);
    const services = await device.getServices();
    this.clearDeviceAttributes(address);
    const serviceInfos: ServiceInfo[] = [];
    services.forEach((service: ble.GattService, serviceIndex: number): void => {
      const serviceId = `${address}/service/${serviceIndex}/${service.serviceUuid}`;
      const characteristicInfos: CharacteristicInfo[] = [];
      service.characteristics.forEach(
        (characteristic: ble.BLECharacteristic, characteristicIndex: number): void => {
          const characteristicId =
            `${serviceId}/characteristic/${characteristicIndex}/${characteristic.characteristicUuid}`;
          this.characteristics.set(characteristicId, characteristic);
          const descriptorInfos: DescriptorInfo[] = [];
          characteristic.descriptors.forEach(
            (descriptor: ble.BLEDescriptor, descriptorIndex: number): void => {
              const descriptorId =
                `${characteristicId}/descriptor/${descriptorIndex}/${descriptor.descriptorUuid}`;
              this.descriptors.set(descriptorId, descriptor);
              descriptorInfos.push({ identifier: descriptorId, uuid: descriptor.descriptorUuid });
            });
          characteristicInfos.push({
            identifier: characteristicId,
            uuid: characteristic.characteristicUuid,
            properties: this.properties(characteristic.properties),
            descriptors: descriptorInfos
          });
        });
      serviceInfos.push({
        identifier: serviceId,
        uuid: service.serviceUuid,
        primary: service.isPrimary,
        characteristics: characteristicInfos
      });
    });
    this.sendReply({ id: id, ok: true, services: serviceInfos });
  }

  private async readCharacteristic(id: number, characteristicId: string): Promise<void> {
    const characteristic = this.requireCharacteristic(characteristicId);
    const device = this.requireDeviceForIdentifier(characteristicId);
    const result = await device.readCharacteristicValue(characteristic);
    this.characteristics.set(characteristicId, result);
    this.sendReply({ id: id, ok: true, value: this.bufferToNumbers(result.characteristicValue) });
  }

  private async writeCharacteristic(command: BluetoothCommand): Promise<void> {
    const characteristicId = command.characteristicId ?? '';
    const characteristic = this.requireCharacteristic(characteristicId);
    const device = this.requireDeviceForIdentifier(characteristicId);
    characteristic.characteristicValue = this.numbersToBuffer(command.value ?? []);
    const writeType = command.withResponse === false ?
      ble.GattWriteType.WRITE_NO_RESPONSE : ble.GattWriteType.WRITE;
    await device.writeCharacteristicValue(characteristic, writeType);
    this.sendReply({ id: command.id, ok: true });
  }

  private async readDescriptor(id: number, descriptorId: string): Promise<void> {
    const descriptor = this.requireDescriptor(descriptorId);
    const device = this.requireDeviceForIdentifier(descriptorId);
    const result = await device.readDescriptorValue(descriptor);
    this.descriptors.set(descriptorId, result);
    this.sendReply({ id: id, ok: true, value: this.bufferToNumbers(result.descriptorValue) });
  }

  private async writeDescriptor(command: BluetoothCommand): Promise<void> {
    const descriptorId = command.descriptorId ?? '';
    const descriptor = this.requireDescriptor(descriptorId);
    const device = this.requireDeviceForIdentifier(descriptorId);
    descriptor.descriptorValue = this.numbersToBuffer(command.value ?? []);
    await device.writeDescriptorValue(descriptor);
    this.sendReply({ id: command.id, ok: true });
  }

  private async setNotify(command: BluetoothCommand): Promise<void> {
    const characteristicId = command.characteristicId ?? '';
    const characteristic = this.requireCharacteristic(characteristicId);
    const address = this.addressFromIdentifier(characteristicId);
    const device = this.requireDevice(address);
    const enabled = command.enabled === true;
    await device.setCharacteristicChangeNotification(characteristic, enabled);
    if (enabled) {
      this.notifying.add(characteristicId);
      if (!this.characteristicCallbacks.has(address)) {
        const callback: CharacteristicCallback =
          (changed: ble.BLECharacteristic): void => {
            const id = this.findCharacteristicId(address, changed);
            if (id && this.notifying.has(id)) {
              this.sendEvent({
                event: 'characteristicValue',
                address: address,
                characteristicId: id,
                value: this.bufferToNumbers(changed.characteristicValue)
              });
            }
          };
        device.on('BLECharacteristicChange', callback);
        this.characteristicCallbacks.set(address, callback);
      }
    } else {
      this.notifying.delete(characteristicId);
      if (!this.hasNotificationForAddress(address)) {
        const callback = this.characteristicCallbacks.get(address);
        if (callback) {
          device.off('BLECharacteristicChange', callback);
          this.characteristicCallbacks.delete(address);
        }
      }
    }
    this.sendReply({ id: command.id, ok: true });
  }

  private onScanResult(result: ble.ScanResult): void {
    const parsed = this.parseAdvertisement(new Uint8Array(result.data));
    this.sendEvent({
      event: 'scanResult',
      address: result.deviceId,
      name: result.deviceName,
      rssi: result.rssi,
      connectable: result.connectable,
      flags: parsed.flags,
      txPower: parsed.txPower,
      uuids: parsed.uuids,
      serviceData: parsed.serviceData,
      manufacturerData: parsed.manufacturerData
    });
  }

  private parseAdvertisement(raw: Uint8Array): ScanAdvertisement {
    let parsed = this.parseAdvertisementAt(raw, raw.length > 5 ? 5 : 0);
    if (parsed.uuids.length === 0 && parsed.serviceData.length === 0 &&
      parsed.manufacturerData.length === 0 && parsed.flags === undefined) {
      parsed = this.parseAdvertisementAt(raw, 0);
    }
    return parsed;
  }

  private parseAdvertisementAt(raw: Uint8Array, start: number): ScanAdvertisement {
    const parsed: ScanAdvertisement = { uuids: [], serviceData: [], manufacturerData: [] };
    let offset = start;
    while (offset < raw.length) {
      const length = raw[offset];
      if (length === 0) {
        break;
      }
      const end = offset + length + 1;
      if (end > raw.length || length < 1) {
        break;
      }
      const type = raw[offset + 1];
      const data = raw.slice(offset + 2, end);
      if (type === 0x01 && data.length > 0) {
        parsed.flags = data[0];
      } else if (type === 0x0a && data.length > 0) {
        parsed.txPower = data[0] > 127 ? data[0] - 256 : data[0];
      } else if (type === 0x02 || type === 0x03) {
        this.appendUuids(parsed.uuids, data, 2);
      } else if (type === 0x04 || type === 0x05) {
        this.appendUuids(parsed.uuids, data, 4);
      } else if (type === 0x06 || type === 0x07) {
        this.appendUuids(parsed.uuids, data, 16);
      } else if (type === 0x16 && data.length >= 2) {
        parsed.serviceData.push({ uuid: this.uuid(data.slice(0, 2)), value: Array.from(data.slice(2)) });
      } else if (type === 0x20 && data.length >= 4) {
        parsed.serviceData.push({ uuid: this.uuid(data.slice(0, 4)), value: Array.from(data.slice(4)) });
      } else if (type === 0x21 && data.length >= 16) {
        parsed.serviceData.push({ uuid: this.uuid(data.slice(0, 16)), value: Array.from(data.slice(16)) });
      } else if (type === 0xff && data.length >= 2) {
        parsed.manufacturerData.push({
          id: data[0] | (data[1] << 8),
          value: Array.from(data.slice(2))
        });
      }
      offset = end;
    }
    return parsed;
  }

  private appendUuids(output: string[], bytes: Uint8Array, width: number): void {
    for (let offset = 0; offset + width <= bytes.length; offset += width) {
      output.push(this.uuid(bytes.slice(offset, offset + width)));
    }
  }

  private uuid(littleEndian: Uint8Array): string {
    if (littleEndian.length === 2) {
      const value = littleEndian[0] | (littleEndian[1] << 8);
      return `0000${value.toString(16).padStart(4, '0')}-0000-1000-8000-00805f9b34fb`;
    }
    if (littleEndian.length === 4) {
      const value = (littleEndian[0] | (littleEndian[1] << 8) |
        (littleEndian[2] << 16) | (littleEndian[3] << 24)) >>> 0;
      return `${value.toString(16).padStart(8, '0')}-0000-1000-8000-00805f9b34fb`;
    }
    const hex = Array.from(littleEndian).reverse()
      .map((value: number): string => value.toString(16).padStart(2, '0')).join('');
    return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-` +
      `${hex.slice(16, 20)}-${hex.slice(20)}`;
  }

  private properties(value: ble.GattProperties | undefined): number {
    if (!value) {
      return 0;
    }
    let result = 0;
    if (value.read) result |= 1 << 1;
    if (value.writeNoResponse) result |= 1 << 2;
    if (value.write) result |= 1 << 3;
    if (value.notify) result |= 1 << 4;
    if (value.indicate) result |= 1 << 5;
    return result;
  }

  private async ensurePermission(): Promise<boolean> {
    const manager = abilityAccessCtrl.createAtManager();
    const status = manager.getSelfPermissionStatus(ACCESS_BLUETOOTH);
    if (status === abilityAccessCtrl.PermissionStatus.GRANTED) {
      return true;
    }
    const result = await manager.requestPermissionsFromUser(this.context, [ACCESS_BLUETOOTH]);
    return (result.authResults ?? [])[0] === 0;
  }

  private requireDevice(address: string): ble.GattClientDevice {
    const device = this.devices.get(address);
    if (!device) throw new Error(`Unknown Bluetooth device: ${address}`);
    return device;
  }

  private requireDeviceForIdentifier(identifier: string): ble.GattClientDevice {
    return this.requireDevice(this.addressFromIdentifier(identifier));
  }

  private requireCharacteristic(identifier: string): ble.BLECharacteristic {
    const characteristic = this.characteristics.get(identifier);
    if (!characteristic) throw new Error(`Unknown GATT characteristic: ${identifier}`);
    return characteristic;
  }

  private requireDescriptor(identifier: string): ble.BLEDescriptor {
    const descriptor = this.descriptors.get(identifier);
    if (!descriptor) throw new Error(`Unknown GATT descriptor: ${identifier}`);
    return descriptor;
  }

  private addressFromIdentifier(identifier: string): string {
    const marker = '/service/';
    const end = identifier.indexOf(marker);
    if (end <= 0) throw new Error(`Invalid GATT identifier: ${identifier}`);
    return identifier.slice(0, end);
  }

  private clearDeviceAttributes(address: string): void {
    this.deleteKeysWithPrefix(this.characteristics, `${address}/`);
    this.deleteKeysWithPrefix(this.descriptors, `${address}/`);
    const callback = this.characteristicCallbacks.get(address);
    const device = this.devices.get(address);
    if (callback && device) {
      try {
        device.off('BLECharacteristicChange', callback);
      } catch (_error) {
      }
    }
    this.characteristicCallbacks.delete(address);
    Array.from(this.notifying).forEach((identifier: string): void => {
      if (identifier.startsWith(`${address}/`)) this.notifying.delete(identifier);
    });
  }

  private deleteKeysWithPrefix<T>(values: Map<string, T>, prefix: string): void {
    Array.from(values.keys()).forEach((key: string): void => {
      if (key.startsWith(prefix)) values.delete(key);
    });
  }

  private findCharacteristicId(address: string, changed: ble.BLECharacteristic): string | undefined {
    for (const identifier of this.characteristics.keys()) {
      const characteristic = this.characteristics.get(identifier);
      if (!characteristic) {
        continue;
      }
      if (identifier.startsWith(`${address}/`) &&
        characteristic.serviceUuid === changed.serviceUuid &&
        characteristic.characteristicUuid === changed.characteristicUuid) {
        return identifier;
      }
    }
    return undefined;
  }

  private hasNotificationForAddress(address: string): boolean {
    for (const identifier of this.notifying) {
      if (identifier.startsWith(`${address}/`)) return true;
    }
    return false;
  }

  private bufferToNumbers(value: ArrayBuffer): number[] {
    return Array.from(new Uint8Array(value));
  }

  private numbersToBuffer(value: number[]): ArrayBuffer {
    return new Uint8Array(value).buffer;
  }

  private sendReply(reply: BluetoothReply): void {
    this.nativePort.CompleteBluetoothMessage(this.componentId, JSON.stringify(reply));
  }

  private sendEvent(event: BluetoothEvent): void {
    this.nativePort.CompleteBluetoothMessage(this.componentId, JSON.stringify(event));
  }

  private sendError(id: number, error: Error): void {
    const businessError = error as BusinessError;
    this.sendReply({
      id: id,
      ok: false,
      error: businessError.message ?? String(error),
      errorCode: businessError.code ?? 0
    });
  }
}
