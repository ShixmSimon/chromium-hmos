/*
 * Copyright 2026 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

import {
  BusinessError,
  commonEventManager,
  serialManager as serial,
  usbManager as usb
} from '@kit.BasicServicesKit';

export interface UsbNativePort {
  SetUsbEventCallback(componentId: string, callback: (commandJson: string) => void): void;
  CompleteUsbMessage(componentId: string, messageJson: string): void;
}

interface UsbCommand {
  id: number;
  method: string;
  deviceName?: string;
  pipeId?: number;
  connectionId?: number;
  platformId?: string;
  reportId?: number;
  size?: number;
  data?: number[];
  portId?: number;
  baudRate?: number;
  dataBits?: number;
  parity?: number;
  stopBits?: number;
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
  hidDevices?: HidDeviceInfo[];
  connectionId?: number;
  data?: number[];
  ports?: SerialPortInfo[];
  bytesTransferred?: number;
}

interface UsbEvent {
  event: string;
  connectionId?: number;
  data?: number[];
}

interface HidDeviceInfo {
  platformId: string;
  physicalDeviceId: string;
  vendorId: number;
  productId: number;
  productName: string;
  serialNumber: string;
  reportDescriptor: number[];
}

interface HidConnectionState {
  connectionId: number;
  pipe: usb.USBDevicePipe;
  iface: usb.USBInterface;
  inputEndpoint: usb.USBEndpoint | undefined;
  outputEndpoint: usb.USBEndpoint | undefined;
  inputReportSize: number;
  active: boolean;
}

interface SerialPortInfo {
  portId: number;
  deviceName: string;
}

/** Owns HarmonyOS USBDevicePipe objects used by Chromium's usbfs backend. */
export class UsbBridge {
  private readonly componentId: string;
  private readonly nativePort: UsbNativePort;
  private readonly pipes: Map<number, usb.USBDevicePipe> = new Map();
  private readonly hidConnections: Map<number, HidConnectionState> = new Map();
  private readonly openSerialPorts: Set<number> = new Set();
  private subscriber: commonEventManager.CommonEventSubscriber | undefined = undefined;
  private nextPipeId: number = 1;
  private nextHidConnectionId: number = 1;

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
    this.openSerialPorts.forEach((portId: number): void => {
      try {
        serial.close(portId);
      } catch (_error) {
      }
    });
    this.openSerialPorts.clear();
    this.hidConnections.forEach((connection: HidConnectionState): void => {
      connection.active = false;
      try {
        usb.releaseInterface(connection.pipe, connection.iface);
      } catch (_error) {
      }
      try {
        usb.closePipe(connection.pipe);
      } catch (_error) {
      }
    });
    this.hidConnections.clear();
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
      case 'hidEnumerate':
        await this.hidEnumerate(command.id);
        return;
      case 'hidOpen':
        this.hidOpen(command.id, command.platformId ?? '', command.size ?? 0);
        return;
      case 'hidClose':
        this.hidClose(command.id, command.connectionId ?? 0);
        return;
      case 'hidWrite':
        await this.hidWrite(command.id, command.connectionId ?? 0,
          command.data ?? [], command.reportId ?? 0);
        return;
      case 'hidGetFeature':
        await this.hidGetFeature(command.id, command.connectionId ?? 0,
          command.reportId ?? 0, command.size ?? 0);
        return;
      case 'hidSendFeature':
        await this.hidSendFeature(command.id, command.connectionId ?? 0,
          command.data ?? [], command.reportId ?? 0);
        return;
      case 'serialEnumerate':
        this.serialEnumerate(command.id);
        return;
      case 'serialOpen':
        await this.serialOpen(command);
        return;
      case 'serialConfigure':
        this.serialConfigure(command);
        return;
      case 'serialRead':
        await this.serialRead(command.id, command.portId ?? -1, command.size ?? 0);
        return;
      case 'serialWrite':
        await this.serialWrite(command.id, command.portId ?? -1, command.data ?? []);
        return;
      case 'serialClose':
        this.serialClose(command.id, command.portId ?? -1);
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

  private async hidEnumerate(id: number): Promise<void> {
    const hidDevices: HidDeviceInfo[] = [];
    for (const device of usb.getDevices()) {
      const hidInterfaces = this.getHidInterfaces(device);
      if (hidInterfaces.length === 0) {
        continue;
      }
      const granted = usb.hasRight(device.name) || await usb.requestRight(device.name);
      if (!granted) {
        continue;
      }

      const pipe = usb.connectDevice(device);
      try {
        for (const iface of hidInterfaces) {
          const descriptor = await this.readHidReportDescriptor(pipe, iface.id);
          if (descriptor.length === 0) {
            continue;
          }
          hidDevices.push({
            platformId: `${device.name}#${iface.id}`,
            physicalDeviceId: device.name,
            vendorId: device.vendorId,
            productId: device.productId,
            productName: device.productName,
            serialNumber: device.serial,
            reportDescriptor: descriptor
          });
        }
      } finally {
        usb.closePipe(pipe);
      }
    }
    this.sendReply({ id: id, ok: true, hidDevices: hidDevices });
  }

  private hidOpen(id: number, platformId: string, inputReportSize: number): void {
    const separator = platformId.lastIndexOf('#');
    if (separator <= 0) {
      throw new Error('Invalid HID platform device ID');
    }
    const deviceName = platformId.substring(0, separator);
    const interfaceNumber = Number(platformId.substring(separator + 1));
    const device = this.findDevice(deviceName);
    if (!usb.hasRight(deviceName)) {
      throw new Error('USB device permission has not been granted');
    }
    const iface = this.getHidInterfaces(device).find(
      (candidate: usb.USBInterface): boolean => candidate.id === interfaceNumber
    );
    if (!iface) {
      throw new Error('HID interface is no longer available');
    }

    const pipe = usb.connectDevice(device);
    const result = usb.claimInterface(pipe, iface, true);
    if (result !== 0) {
      usb.closePipe(pipe);
      throw new Error(`Could not claim HID interface: ${result}`);
    }
    const connectionId = this.nextHidConnectionId++;
    const connection: HidConnectionState = {
      connectionId: connectionId,
      pipe: pipe,
      iface: iface,
      inputEndpoint: iface.endpoints.find(
        (endpoint: usb.USBEndpoint): boolean => endpoint.direction ===
          usb.USBRequestDirection.USB_REQUEST_DIR_FROM_DEVICE
      ),
      outputEndpoint: iface.endpoints.find(
        (endpoint: usb.USBEndpoint): boolean => endpoint.direction ===
          usb.USBRequestDirection.USB_REQUEST_DIR_TO_DEVICE
      ),
      inputReportSize: inputReportSize,
      active: true
    };
    this.hidConnections.set(connectionId, connection);
    this.sendReply({ id: id, ok: true, connectionId: connectionId });
    this.readHidInput(connection).catch((error: Error): void => {
      if (connection.active) {
        console.error(`HID input loop failed: ${String(error)}`);
      }
    });
  }

  private hidClose(id: number, connectionId: number): void {
    const connection = this.hidConnections.get(connectionId);
    if (connection) {
      connection.active = false;
      this.hidConnections.delete(connectionId);
      try {
        usb.releaseInterface(connection.pipe, connection.iface);
      } catch (_error) {
      }
      usb.closePipe(connection.pipe);
    }
    this.sendReply({ id: id, ok: true });
  }

  private async hidWrite(id: number, connectionId: number, data: number[],
    reportId: number): Promise<void> {
    const connection = this.requireHidConnection(connectionId);
    const buffer = new Uint8Array(data);
    let transferred: number;
    if (connection.outputEndpoint) {
      transferred = await usb.bulkTransfer(
        connection.pipe, connection.outputEndpoint, buffer, 5000);
    } else {
      transferred = await this.hidSetReport(
        connection, 2, reportId, buffer);
    }
    this.sendReply({ id: id, ok: transferred >= 0 });
  }

  private async hidGetFeature(id: number, connectionId: number,
    reportId: number, size: number): Promise<void> {
    const connection = this.requireHidConnection(connectionId);
    const buffer = new Uint8Array(Math.max(size, 1));
    if (reportId !== 0) {
      buffer[0] = reportId;
    }
    const params: usb.USBDeviceRequestParams = {
      bmRequestType: 0xA1,
      bRequest: 0x01,
      wValue: (3 << 8) | reportId,
      wIndex: connection.iface.id,
      wLength: buffer.length,
      data: buffer
    };
    const transferred = await usb.usbControlTransfer(connection.pipe, params, 5000);
    if (transferred < 0) {
      throw new Error('Could not read HID feature report');
    }
    this.sendReply({
      id: id,
      ok: true,
      data: this.toNumberArray(buffer, transferred)
    });
  }

  private async hidSendFeature(id: number, connectionId: number, data: number[],
    reportId: number): Promise<void> {
    const connection = this.requireHidConnection(connectionId);
    const transferred = await this.hidSetReport(
      connection, 3, reportId, new Uint8Array(data));
    this.sendReply({ id: id, ok: transferred >= 0 });
  }

  private async hidSetReport(connection: HidConnectionState, reportType: number,
    reportId: number, buffer: Uint8Array): Promise<number> {
    const params: usb.USBDeviceRequestParams = {
      bmRequestType: 0x21,
      bRequest: 0x09,
      wValue: (reportType << 8) | reportId,
      wIndex: connection.iface.id,
      wLength: buffer.length,
      data: buffer
    };
    return await usb.usbControlTransfer(connection.pipe, params, 5000);
  }

  private async readHidInput(connection: HidConnectionState): Promise<void> {
    const endpoint = connection.inputEndpoint;
    if (!endpoint) {
      return;
    }
    const bufferSize = Math.max(endpoint.maxPacketSize,
      connection.inputReportSize, 1);
    while (connection.active) {
      const buffer = new Uint8Array(bufferSize);
      let transferred: number;
      try {
        transferred = await usb.bulkTransfer(
          connection.pipe, endpoint, buffer, 1000);
      } catch (error) {
        if (!connection.active) {
          return;
        }
        const businessError = error as BusinessError;
        throw new Error(businessError.message ?? String(error));
      }
      if (connection.active && transferred > 0) {
        this.sendEvent({
          event: 'hidInput',
          connectionId: connection.connectionId,
          data: this.toNumberArray(buffer, transferred)
        });
      }
    }
  }

  private serialEnumerate(id: number): void {
    const ports = serial.getPortList().map((port: serial.SerialPort): SerialPortInfo => {
      return { portId: port.portId, deviceName: port.deviceName };
    });
    this.sendReply({ id: id, ok: true, ports: ports });
  }

  private async serialOpen(command: UsbCommand): Promise<void> {
    const portId = this.requireSerialPort(command.portId ?? -1);
    const granted = serial.hasSerialRight(portId) ||
      await serial.requestSerialRight(portId);
    if (!granted) {
      this.sendReply({ id: command.id, ok: false, error: 'Serial port permission denied' });
      return;
    }
    serial.open(portId);
    try {
      serial.setAttribute(portId, this.toSerialAttribute(command));
      this.openSerialPorts.add(portId);
      this.sendReply({ id: command.id, ok: true });
    } catch (error) {
      serial.close(portId);
      const businessError = error as BusinessError;
      throw new Error(businessError.message ?? String(error));
    }
  }

  private serialConfigure(command: UsbCommand): void {
    const portId = this.requireOpenSerialPort(command.portId ?? -1);
    serial.setAttribute(portId, this.toSerialAttribute(command));
    this.sendReply({ id: command.id, ok: true });
  }

  private async serialRead(id: number, portIdValue: number, size: number): Promise<void> {
    const portId = this.requireOpenSerialPort(portIdValue);
    const buffer = new Uint8Array(Math.min(Math.max(size, 1), 8192));
    try {
      const transferred = await serial.read(portId, buffer, 1000);
      this.sendReply({
        id: id,
        ok: true,
        data: this.toNumberArray(buffer, transferred),
        bytesTransferred: transferred
      });
    } catch (error) {
      const businessError = error as BusinessError;
      if (businessError.code === 31400006) {
        this.sendReply({ id: id, ok: true, data: [], bytesTransferred: 0 });
        return;
      }
      throw new Error(businessError.message ?? String(error));
    }
  }

  private async serialWrite(id: number, portIdValue: number,
    data: number[]): Promise<void> {
    const portId = this.requireOpenSerialPort(portIdValue);
    const buffer = new Uint8Array(data.slice(0, 4096));
    const transferred = await serial.write(portId, buffer, 5000);
    this.sendReply({ id: id, ok: true, bytesTransferred: transferred });
  }

  private serialClose(id: number, portIdValue: number): void {
    if (this.openSerialPorts.has(portIdValue)) {
      serial.close(portIdValue);
      this.openSerialPorts.delete(portIdValue);
    }
    this.sendReply({ id: id, ok: true });
  }

  private toSerialAttribute(command: UsbCommand): serial.SerialAttribute {
    return {
      baudRate: command.baudRate as serial.BaudRates,
      dataBits: command.dataBits as serial.DataBits,
      parity: command.parity as serial.Parity,
      stopBits: command.stopBits as serial.StopBits
    };
  }

  private requireSerialPort(portId: number): number {
    const exists = serial.getPortList().some(
      (port: serial.SerialPort): boolean => port.portId === portId
    );
    if (!exists) {
      throw new Error(`Unknown serial port: ${portId}`);
    }
    return portId;
  }

  private requireOpenSerialPort(portId: number): number {
    if (!this.openSerialPorts.has(portId)) {
      throw new Error(`Serial port is not open: ${portId}`);
    }
    return portId;
  }

  private async readHidReportDescriptor(pipe: usb.USBDevicePipe,
    interfaceNumber: number): Promise<number[]> {
    const buffer = new Uint8Array(4096);
    const params: usb.USBDeviceRequestParams = {
      bmRequestType: 0x81,
      bRequest: 0x06,
      wValue: 0x2200,
      wIndex: interfaceNumber,
      wLength: buffer.length,
      data: buffer
    };
    try {
      const transferred = await usb.usbControlTransfer(pipe, params, 5000);
      return transferred > 0 ? this.toNumberArray(buffer, transferred) : [];
    } catch (error) {
      console.error(`Could not read HID report descriptor: ${String(error)}`);
      return [];
    }
  }

  private getHidInterfaces(device: usb.USBDevice): usb.USBInterface[] {
    const result: usb.USBInterface[] = [];
    if (device.configs.length === 0) {
      return result;
    }
    for (const iface of device.configs[0].interfaces) {
      if (iface.clazz === 3 && iface.alternateSetting === 0) {
        result.push(iface);
      }
    }
    return result;
  }

  private requireHidConnection(connectionId: number): HidConnectionState {
    const connection = this.hidConnections.get(connectionId);
    if (!connection || !connection.active) {
      throw new Error('HID connection is closed');
    }
    return connection;
  }

  private toNumberArray(buffer: Uint8Array, length: number): number[] {
    const result: number[] = [];
    const count = Math.min(Math.max(length, 0), buffer.length);
    for (let index = 0; index < count; ++index) {
      result.push(buffer[index]);
    }
    return result;
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
