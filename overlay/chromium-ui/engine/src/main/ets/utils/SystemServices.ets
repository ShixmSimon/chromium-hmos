/*
 * Copyright (c) 2026 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

// Which HarmonyOS service answers which name on the system service bridge.
// The names are the ones the C++ side calls with; keep the two in step.

import { common } from '@kit.AbilityKit';
import { SystemServiceBridge, SystemServiceEmitter, SystemServiceHandler,
  SystemServiceHandlerFactory } from './SystemServiceBridge';
import { BackgroundAudioService } from './BackgroundAudioService';
import { NotificationService } from './NotificationService';
import { OrientationService } from './OrientationService';
import { ShapeDetectionService } from './ShapeDetectionService';
import { TtsService } from './TtsService';
import { UserAuthService } from './UserAuthService';

// Services the shell answers rather than the engine. Registered by the shell
// before the first WebWindow exists, so they are held here until there is a
// bridge to put them on -- and put on every bridge after that, because a
// second browser window gets a bridge of its own and a service that only
// worked in the first window would be a bug nobody could see coming.
//
// Kept by name, so registering the same name twice replaces rather than
// stacks: a shell that re-registers on a hot reload should not end up with
// two handlers and one of them stale.
const shellServices: Map<string, SystemServiceHandlerFactory> = new Map();

// Lets the shell answer a service the engine does not implement. The engine
// calls into these exactly as it calls into its own: one reply per request,
// and a handler that throws becomes an error the web API can see.
//
// The engine's own services win a name clash. They are what Chromium's C++
// side was written against -- 'userauth' unlocks saved passwords, 'tts'
// backs speechSynthesis -- and letting a shell take one of those names over
// would break a web API rather than extend one.
export function registerShellSystemService(service: string,
  factory: SystemServiceHandlerFactory): void {
  shellServices.set(service, factory);
}

export function registerSystemServices(bridge: SystemServiceBridge,
  context: common.UIAbilityContext): void {
  // The shell's first, so the engine's own overwrite them on a clash.
  shellServices.forEach((factory: SystemServiceHandlerFactory, name: string) => {
    bridge.register(name, factory);
  });
  bridge.register('tts', (emit: SystemServiceEmitter): SystemServiceHandler => new TtsService(emit));
  bridge.register('notification',
    (emit: SystemServiceEmitter): SystemServiceHandler => new NotificationService(context, emit));
  bridge.register('orientation', (): SystemServiceHandler => new OrientationService(context));
  bridge.register('shape', (): SystemServiceHandler => new ShapeDetectionService());
  bridge.register('userauth', (): SystemServiceHandler => new UserAuthService());
  bridge.register('backgroundaudio', (): SystemServiceHandler => new BackgroundAudioService(context));
}
