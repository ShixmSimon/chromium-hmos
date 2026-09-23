/*
 * Copyright (c) 2026 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

// Which HarmonyOS service answers which name on the system service bridge.
// The names are the ones the C++ side calls with; keep the two in step.

import { common } from '@kit.AbilityKit';
import { SystemServiceBridge, SystemServiceEmitter, SystemServiceHandler } from './SystemServiceBridge';
import { NotificationService } from './NotificationService';
import { OrientationService } from './OrientationService';
import { ShapeDetectionService } from './ShapeDetectionService';
import { TtsService } from './TtsService';
import { UserAuthService } from './UserAuthService';

export function registerSystemServices(bridge: SystemServiceBridge,
  context: common.UIAbilityContext): void {
  bridge.register('tts', (emit: SystemServiceEmitter): SystemServiceHandler => new TtsService(emit));
  bridge.register('notification',
    (emit: SystemServiceEmitter): SystemServiceHandler => new NotificationService(context, emit));
  bridge.register('orientation', (): SystemServiceHandler => new OrientationService(context));
  bridge.register('shape', (): SystemServiceHandler => new ShapeDetectionService());
  bridge.register('userauth', (): SystemServiceHandler => new UserAuthService());
}
