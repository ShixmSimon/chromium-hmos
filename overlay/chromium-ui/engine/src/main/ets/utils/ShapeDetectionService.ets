/*
 * Copyright (c) 2026 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

// The Shape Detection API (BarcodeDetector, FaceDetector, TextDetector) on
// ScanKit and CoreVisionKit.
//
// Methods (see services/shape_detection/*_ohos.cc):
//   barcode { width, height, nv21 }   -> { results: [{ rawValue, format, left, top,
//                                           right, bottom, corners?: [{x, y}] }] }
//   face { width, height, rgba }      -> { faces: [{ left, top, width, height,
//                                           landmarks: [{ type, x, y }] }] }
//   text { width, height, rgba }      -> { texts: [{ rawValue, corners: [{x, y}] }] }
// Pixels are base64. Coordinates are in the pixels of the image sent.

import { detectBarcode, scanBarcode, scanCore } from '@kit.ScanKit';
import { faceDetector, textRecognition } from '@kit.CoreVisionKit';
import { image } from '@kit.ImageKit';
import { util } from '@kit.ArkTS';
import { SystemServiceHandler } from './SystemServiceBridge';

interface Corner {
  x: number;
  y: number;
}

// ScanKit's codes by their Web BarcodeFormat names.
const FORMATS: Map<number, string> = new Map([
  [scanCore.ScanType.AZTEC_CODE, 'aztec'],
  [scanCore.ScanType.CODABAR_CODE, 'codabar'],
  [scanCore.ScanType.CODE39_CODE, 'code_39'],
  [scanCore.ScanType.CODE93_CODE, 'code_93'],
  [scanCore.ScanType.CODE128_CODE, 'code_128'],
  [scanCore.ScanType.DATAMATRIX_CODE, 'data_matrix'],
  [scanCore.ScanType.EAN8_CODE, 'ean_8'],
  [scanCore.ScanType.EAN13_CODE, 'ean_13'],
  [scanCore.ScanType.ITF14_CODE, 'itf'],
  [scanCore.ScanType.PDF417_CODE, 'pdf417'],
  [scanCore.ScanType.QR_CODE, 'qr_code'],
  [scanCore.ScanType.UPC_A_CODE, 'upc_a'],
  [scanCore.ScanType.UPC_E_CODE, 'upc_e']
]);

// CoreVisionKit's five landmarks, in its order, as Web LandmarkType names.
const FACE_LANDMARKS: string[] = ['eye', 'eye', 'nose', 'mouth', 'mouth'];

function decodeBase64(data: Object | undefined): ArrayBuffer {
  const bytes: Uint8Array = new util.Base64Helper().decodeSync(String(data ?? ''));
  return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
}

function toCorners(points: Object | undefined): Corner[] {
  if (!Array.isArray(points)) {
    return [];
  }
  return (points as Array<Record<string, Object>>).map((p: Record<string, Object>): Corner => ({
    x: Number(p['x'] ?? 0),
    y: Number(p['y'] ?? 0)
  }));
}

export class ShapeDetectionService implements SystemServiceHandler {
  async handle(method: string, args: Record<string, Object>): Promise<Object | undefined> {
    const width = Number(args['width'] ?? 0);
    const height = Number(args['height'] ?? 0);
    if (width <= 0 || height <= 0) {
      throw new Error('empty image');
    }
    switch (method) {
      case 'barcode':
        return this.barcode(width, height, decodeBase64(args['nv21']));
      case 'face':
        return this.face(await this.pixelMap(width, height, args['rgba']));
      case 'text':
        return this.text(await this.pixelMap(width, height, args['rgba']));
      default:
        throw new Error(`shape has no method ${method}`);
    }
  }

  private async barcode(width: number, height: number, nv21: ArrayBuffer): Promise<Object> {
    const byteImage: detectBarcode.ByteImage = {
      byteBuffer: nv21,
      width: width,
      height: height,
      format: detectBarcode.ImageFormat.NV21
    };
    const options: scanBarcode.ScanOptions = {
      scanTypes: [scanCore.ScanType.ALL],
      enableMultiMode: true,
      enableAlbum: false
    };
    const detected: detectBarcode.DetectResult = await detectBarcode.decodeImage(byteImage, options);
    const results: Record<string, Object>[] = detected.scanResults.map(
      (result: scanBarcode.ScanResult): Record<string, Object> => {
        const rect = result.scanCodeRect;
        const entry: Record<string, Object> = {
          'rawValue': result.originalValue,
          'format': FORMATS.get(result.scanType) ?? 'unknown',
          'left': rect?.left ?? 0,
          'top': rect?.top ?? 0,
          'right': rect?.right ?? 0,
          'bottom': rect?.bottom ?? 0
        };
        const corners = toCorners(result.cornerPoints);
        if (corners.length > 0) {
          entry['corners'] = corners;
        }
        return entry;
      });
    const found: Record<string, Object> = { 'results': results };
    return found;
  }

  private async face(pixelMap: image.PixelMap): Promise<Object> {
    try {
      const faces: faceDetector.Face[] = await faceDetector.detect({ pixelMap: pixelMap });
      const found: Record<string, Object>[] = faces.map((face: faceDetector.Face): Record<string, Object> => {
        const landmarks: Record<string, Object>[] = [];
        face.points.forEach((point: faceDetector.FacePoint, index: number): void => {
          if (index < FACE_LANDMARKS.length) {
            const landmark: Record<string, Object> = {
              'type': FACE_LANDMARKS[index], 'x': point.x, 'y': point.y
            };
            landmarks.push(landmark);
          }
        });
        const entry: Record<string, Object> = {
          'left': face.rect.left,
          'top': face.rect.top,
          'width': face.rect.width,
          'height': face.rect.height,
          'landmarks': landmarks
        };
        return entry;
      });
      const result: Record<string, Object> = { 'faces': found };
      return result;
    } finally {
      void pixelMap.release();
    }
  }

  private async text(pixelMap: image.PixelMap): Promise<Object> {
    try {
      const result: textRecognition.TextRecognitionResult =
        await textRecognition.recognizeText({ pixelMap: pixelMap });
      const texts: Record<string, Object>[] = [];
      result.blocks.forEach((block: textRecognition.TextBlock): void => {
        block.lines.forEach((line: textRecognition.TextLine): void => {
          const entry: Record<string, Object> = {
            'rawValue': line.value, 'corners': toCorners(line.cornerPoints)
          };
          texts.push(entry);
        });
      });
      const found: Record<string, Object> = { 'texts': texts };
      return found;
    } finally {
      void pixelMap.release();
    }
  }

  private pixelMap(width: number, height: number, rgba: Object | undefined): Promise<image.PixelMap> {
    return image.createPixelMap(decodeBase64(rgba), {
      size: { width: width, height: height },
      srcPixelFormat: image.PixelMapFormat.RGBA_8888,
      pixelFormat: image.PixelMapFormat.RGBA_8888,
      editable: false
    });
  }
}
