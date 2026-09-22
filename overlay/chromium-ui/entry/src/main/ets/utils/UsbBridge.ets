/*
 * Copyright 2026 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

import { BusinessError, commonEventManager, usbManager as usb } from '@kit.BasicServicesKit';

export interface UsbNativePort {
  SetUsbEventCallback(componentId: string, callback: (commandJson: string) => void): void;
  CompleteUsbMessage(componentId: string, messageJson: string): void;
}

interface UsbCommand {
  id: number;
  method: string;
  deviceName?: string;
  pipeId?: number;
}

interface UsbEndpointInfo {
  address: number;
  attributes: number;
  maximumPacketSize: number;
  pollingInterval: number;
}

interface UsbInterfaceInfo {
  interfaceNumber: number;
  alternateSetting: number;
  classCode: number;
  subclassCode: number;
  protocolCode: number;
  interfaceName: string;
  endpoints: UsbEndpointInfo[];
}

interface UsbConfigurationInfo {
  configurationValue: number;
  configurationName: string;
  selfPowered: boolean;
  remoteWakeup: boolean;
  maximumPower: number;
  interfaces: UsbInterfaceInfo[];
}

interface UsbDeviceInfo {
  name: string;
  busNumber: number;
  deviceAddress: number;
  vendorId: number;
  productId: number;
  classCode: number;
  subclassCode: number;
  protocolCode: number;
  manufacturerName: string;
  productName: string;
  serialNumber: string;
  permissionGranted: boolean;
  configurations: UsbConfigurationInfo[];
}

interface UsbReply {
  id: number;
  ok: boolean;
  error?: string;
  errorCode?: number;
  granted?: boolean;
  fileDescriptor?: number;
  pipeId?: number;
  devices?: UsbDeviceInfo[];
}

interface UsbEvent {
  event: string;
}

/** Owns HarmonyOS USBDevicePipe objects used by Chromium's usbfs backend. */
export class UsbBridge {
  private readonly componentId: string;
  private readonly nativePort: UsbNativePort;
  private readonly pipes: Map<number, usb.USBDevicePipe> = new Map();
  private subscriber: commonEventManager.CommonEventSubscriber | undefined = undefined;
  private nextPipeId: number = 1;

  constructor(componentId: string, nativePort: UsbNativePort) {
    this.componentId = componentId;
    this.nativePort = nativePort;
    this.nativePort.SetUsbEventCallback(
      this.componentId,
      (commandJson: string): void => this.onCommand(commandJson)
    );
    this.subscribeHotplug().catch((error: Error): void => {
      console.error(`USB hotplug subscription failed: ${String(error)}`);
    });
  }

  destroy(): void {
    this.pipes.forEach((pipe: usb.USBDevicePipe): void => {
      try {
        usb.closePipe(pipe);
      } catch (_error) {
      }
    });
    this.pipes.clear();
    if (this.subscriber) {
      try {
        commonEventManager.unsubscribe(this.subscriber);
      } catch (_error) {
      }
      this.subscriber = undefined;
    }
  }

  private onCommand(commandJson: string): void {
    let command: UsbCommand;
    try {
      command = JSON.parse(commandJson) as UsbCommand;
    } catch (error) {
      console.error(`Invalid USB command: ${String(error)}`);
      return;
    }
    this.execute(command).catch((error: Error): void => {
      this.sendError(command.id, error);
    });
  }

  private async execute(command: UsbCommand): Promise<void> {
    switch (command.method) {
      case 'enumerate':
        this.enumerate(command.id);
        return;
      case 'requestPermission':
        await this.requestPermission(command.id, command.deviceName ?? '');
        return;
      case 'open':
        this.open(command.id, command.deviceName ?? '');
        return;
      case 'close':
        this.close(command.id, command.pipeId ?? 0);
        return;
      default:
        throw new Error(`Unknown USB method: ${command.method}`);
    }
  }

  private enumerate(id: number): void {
    const devices = usb.getDevices().map((device: usb.USBDevice): UsbDeviceInfo => {
      return this.toDeviceInfo(device);
    });
    this.sendReply({ id: id, ok: true, devices: devices });
  }

  private async requestPermission(id: number, deviceName: string): Promise<void> {
    if (!deviceName) {
      throw new Error('Missing USB device name');
    }
    const granted = usb.hasRight(deviceName) || await usb.requestRight(deviceName);
    this.sendReply({ id: id, ok: true, granted: granted });
  }

  private open(id: number, deviceName: string): void {
    const device = this.findDevice(deviceName);
    if (!usb.hasRight(deviceName)) {
      throw new Error('USB device permission has not been granted');
    }
    const pipe = usb.connectDevice(device);
    const fileDescriptor = usb.getFileDescriptor(pipe);
    if (fileDescriptor < 0) {
      usb.closePipe(pipe);
      throw new Error('Could not obtain the USB device file descriptor');
    }
    const pipeId = this.nextPipeId++;
    this.pipes.set(pipeId, pipe);
    this.sendReply({
      id: id,
      ok: true,
      fileDescriptor: fileDescriptor,
      pipeId: pipeId
    });
  }

  private close(id: number, pipeId: number): void {
    const pipe = this.pipes.get(pipeId);
    if (pipe) {
      usb.closePipe(pipe);
      this.pipes.delete(pipeId);
    }
    this.sendReply({ id: id, ok: true });
  }

  private findDevice(deviceName: string): usb.USBDevice {
    const device = usb.getDevices().find((candidate: usb.USBDevice): boolean => {
      return candidate.name === deviceName;
    });
    if (!device) {
      throw new Error(`Unknown USB device: ${deviceName}`);
    }
    return device;
  }

  private toDeviceInfo(device: usb.USBDevice): UsbDeviceInfo {
    const permissionGranted = usb.hasRight(device.name);
    return {
      name: device.name,
      busNumber: device.busNum,
      deviceAddress: device.devAddress,
      vendorId: device.vendorId,
      productId: device.productId,
      classCode: device.clazz,
      subclassCode: device.subClass,
      protocolCode: device.protocol,
      manufacturerName: device.manufacturerName,
      productName: device.productName,
      serialNumber: permissionGranted ? device.serial : '',
      permissionGranted: permissionGranted,
      configurations: device.configs.map(
        (config: usb.USBConfiguration): UsbConfigurationInfo => {
          return {
            configurationValue: config.id,
            configurationName: config.name,
            selfPowered: config.isSelfPowered,
            remoteWakeup: config.isRemoteWakeup,
            maximumPower: config.maxPower,
            interfaces: config.interfaces.map(
              (iface: usb.USBInterface): UsbInterfaceInfo => {
                return {
                  interfaceNumber: iface.id,
                  alternateSetting: iface.alternateSetting,
                  classCode: iface.clazz,
                  subclassCode: iface.subClass,
                  protocolCode: iface.protocol,
                  interfaceName: iface.name,
                  endpoints: iface.endpoints.map(
                    (endpoint: usb.USBEndpoint): UsbEndpointInfo => {
                      return {
                        address: endpoint.address,
                        attributes: endpoint.attributes,
                        maximumPacketSize: endpoint.maxPacketSize,
                        pollingInterval: endpoint.interval
                      };
                    })
                };
              })
          };
        })
    };
  }

  private async subscribeHotplug(): Promise<void> {
    const subscribeInfo: commonEventManager.CommonEventSubscribeInfo = {
      events: [
        commonEventManager.Support.COMMON_EVENT_USB_DEVICE_ATTACHED,
        commonEventManager.Support.COMMON_EVENT_USB_DEVICE_DETACHED
      ]
    };
    const subscriber = await commonEventManager.createSubscriber(subscribeInfo);
    this.subscriber = subscriber;
    commonEventManager.subscribe(
      subscriber,
      (error: BusinessError, _data: commonEventManager.CommonEventData): void => {
        if (error) {
          console.error(`USB hotplug event failed: ${error.message}`);
          return;
        }
        this.sendEvent({ event: 'devicesChanged' });
      }
    );
  }

  private sendReply(reply: UsbReply): void {
    this.nativePort.CompleteUsbMessage(this.componentId, JSON.stringify(reply));
  }

  private sendEvent(event: UsbEvent): void {
    this.nativePort.CompleteUsbMessage(this.componentId, JSON.stringify(event));
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
