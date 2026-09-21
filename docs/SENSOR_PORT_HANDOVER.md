# Handover: the generic sensor port

Everything needed to finish `services/device/generic_sensor` on HarmonyOS,
written for a session picking this up on another machine. Geolocation went the
same route and is complete; this follows its shape deliberately, so when a
question here is unanswered, `overlay/services/device/geolocation/ohos/` and
`patches/ohos-geolocation.patch` are the worked example.

Branch: `hmos-154-adapter`. Chromium 154.0.8037.51, `target_os = "ohos"`,
arm64, `enable_arkweb = false`.

## Ground rules

These are not style preferences. Breaking them produces a build that goes
green and ships nothing.

1. **New files go in `overlay/`. Edits to files upstream already tracks go in
   `patches/`.** An overlay copy of an upstream file freezes it: one such
   override pinned a stale `llvm_version` and cost a 2h25m rebuild before
   anyone noticed.
2. **CI never applies `patches/`.** The runner's Chromium tree is persistent,
   so a patch is applied by hand once (`git -C <tree> apply <patch>`) and stays
   applied. A patch that has not been applied on the runner simply does not
   exist as far as CI is concerned, and CI still passes.
3. **CI skips three overlay subtrees when syncing into the tree** --
   `chromium-ui/*`, `arkweb/*`, `ohos_arkweb_playground/*`
   (`scripts/ci-incremental-build.sh`, the `case "$rel"` at the top of the sync
   loop). Of the overlay's ~5000 files only a few hundred reach the tree.
   **Anything under `overlay/arkweb/` compiles nothing.** It is reference
   material, not code that runs.
4. **The ArkWeb adapter route is a dead end.** `enable_arkweb = false`, and
   `overlay/ohos/adapter/` contains only a `BUILD.gn` -- the sources for
   `libadapter.so` were never published. Do not try to revive it. Port the
   pure-NDK implementation into the CI-synced part of the overlay instead.
5. **`DCHECK` and `NOTREACHED()` are compiled out in this build.** A
   `NOTREACHED()` default implementation becomes a trap or a null deref on
   device, not a diagnostic. This has bitten HID, Serial, GLFence and the
   system-permission handle. Assume the release behaviour when reading
   upstream code that "cannot happen".

## What exists already

| Path | What it is |
| --- | --- |
| `overlay/services/device/generic_sensor/ohos/sensor_types_ohos.{h,cc}` | Written, **not yet wired into any BUILD.gn**, so it compiles nothing. mojom↔`Sensor_Type` mapping and a cached capability snapshot. Start here. |
| `overlay/arkweb/chromium_ext/services/device/generic_sensor/platform_sensor_ohos.{h,cc}` | Huawei's Chromium-side sensor, 97 + 263 lines. Reference only (rule 3). |
| `overlay/arkweb/chromium_ext/services/device/generic_sensor/platform_sensor_provider_ohos.{h,cc}` | Huawei's provider, 61 + 185 lines. Almost pure upstream dependencies -- the fusion algorithms -- so it ports nearly verbatim. |
| `overlay/arkweb/ohos_adapter_ndk/sensor_adapter/sensor_adapter_impl.{h,cpp}` | Huawei's NDK layer, 59 + 400 lines. Real `OH_Sensor_*` code. Its only outside dependency is `nweb_hilog.h`. |

The port is: take the three pieces above, drop the `OHOS::NWeb::SensorAdapter`
indirection (it exists to cross the ArkWeb glue boundary, which we do not
have), and call the NDK directly from `platform_sensor_ohos`.

Target layout, mirroring the geolocation port:

```
overlay/services/device/generic_sensor/ohos/
  sensor_types_ohos.{h,cc}            # mapping + capabilities        [written]
  sensor_subscription_ohos.{h,cc}     # RAII subscription + registry  [to do]
  platform_sensor_ohos.{h,cc}         # PlatformSensor                [to do]
  platform_sensor_provider_ohos.{h,cc}# PlatformSensorProvider        [to do]
  sensor_types_ohos_unittest.cc       # host-checkable part           [to do]
patches/ohos-generic-sensor.patch     # BUILD.gn + the factory        [to do]
```

## Defects in the reference code -- do not carry these over

Found by reading, not by running. Each is real in
`sensor_adapter_impl.cpp` as published.

1. **The mojom type map is wrong for 154.** `SensorTypeToOhosSensorType` keys
   on bare `mojom::SensorType` integers (`2` → ACCELEROMETER, `5` →
   GYROSCOPE, `9` → ABSOLUTE_ORIENTATION_QUATERNION, …). In 154 the enum is
   `AMBIENT_LIGHT=0, ACCELEROMETER=1, LINEAR_ACCELERATION=2, GRAVITY=3,
   GYROSCOPE=4, MAGNETOMETER=5, ABSOLUTE_ORIENTATION_EULER_ANGLES=6,
   ABSOLUTE_ORIENTATION_QUATERNION=7, RELATIVE_ORIENTATION_EULER_ANGLES=8,
   RELATIVE_ORIENTATION_QUATERNION=9`. Every entry is off by one, and the
   numbers stay valid, so each sensor would quietly deliver its neighbour's
   data. `sensor_types_ohos.cc` writes the map with enumerators for this
   reason; keep it that way.
2. **`SubscribeOhosSensor` leaks on every error path.** It creates a
   `Sensor_Subscriber`, a `Sensor_SubscriptionId` and a
   `Sensor_SubscriptionAttribute`, then `return ret`s out of four failure
   checks without destroying any of them. Make the subscription RAII.
3. **`OhosSensorCallback` dereferences before its null check.** It calls
   `OH_SensorEvent_GetType(event, &type)` and only afterwards tests
   `event == nullptr`.
4. **`OH_Sensor_CreateInfos` is not null-checked** before being passed to
   `OH_Sensor_GetInfos`, and the first `OH_Sensor_GetInfos(nullptr, &count)`
   return value is ignored.
5. **Every capability question re-enumerates every sensor on the device.**
   `IsOhosSensorSupported`, `GetOhosSensorMinSupportedFrequency`,
   `GetOhosSensorMaxSupportedFrequency` each do
   `GetInfos`/`CreateInfos`/`GetInfos`/`DestroyInfos`, and
   `CheckSensorConfiguration` calls two of them per configuration change.
   `sensor_types_ohos.cc` reads the list once instead.
6. **It targets an older Chromium.** `CreateSensorInternal` and
   `PlatformSensorFusion::Create` still carry commented-out `reading_buffer`
   parameters. The 154 signatures are below -- they happen to match what the
   commented-out version becomes, but check rather than assume.

## Chromium 154 API facts (verified against the tag)

Confirmed by reading `refs/tags/154.0.8037.51` on chromium.googlesource.com.
Quoted so this does not need re-deriving.

```cpp
// services/device/generic_sensor/platform_sensor.h
PlatformSensor(mojom::SensorType type,
               SensorReadingSharedBuffer* reading_buffer,
               base::WeakPtr<PlatformSensorProvider> provider);
virtual bool StartSensor(const PlatformSensorConfiguration&) = 0;
virtual void StopSensor() = 0;
virtual bool CheckSensorConfiguration(const PlatformSensorConfiguration&) = 0;
// Thread-safe, per its own comment:
void UpdateSharedBufferAndNotifyClients(const SensorReading& reading);
void PostTaskToMainSequence(const base::Location&, base::OnceClosure);

// services/device/generic_sensor/platform_sensor_provider.h
virtual void CreateSensorInternal(mojom::SensorType type,
                                  CreateSensorCallback callback) = 0;
SensorReadingSharedBuffer* GetSensorReadingSharedBufferForType(mojom::SensorType);
scoped_refptr<PlatformSensor> GetSensor(mojom::SensorType type);

// services/device/generic_sensor/platform_sensor_fusion.h
static void Create(base::WeakPtr<PlatformSensorProvider> provider,
                   std::unique_ptr<PlatformSensorFusionAlgorithm> algorithm,
                   PlatformSensorProvider::CreateSensorCallback callback);
```

`PlatformSensorProvider::Create()` in
`services/device/generic_sensor/platform_sensor_provider.cc` is a
`#if BUILDFLAG(IS_MAC) / #elif …` chain ending in `#else return nullptr`.
OHOS falls into the `#else` today; the patch adds an `#elif BUILDFLAG(IS_OHOS)`
arm and the matching `#include`. `BUILDFLAG(IS_OHOS)` and GN's `is_ohos` both
exist in this tree.

## OHOS NDK facts

Header: `<sensors/oh_sensor.h>` and `<sensors/oh_sensor_type.h>` in the NDK
sysroot. Library: `libohsensor.so`, so GN wants `libs = [ "ohsensor" ]`.
(`overlay/services/device/geolocation/BUILD.gn`'s sibling uses
`libs = [ "location_ndk" ]`; same shape.)

Sensor types present in the SDK, with the mapping the port uses:

| `mojom::SensorType` | `Sensor_Type` | value |
| --- | --- | --- |
| ACCELEROMETER | `SENSOR_TYPE_ACCELEROMETER` | 1 |
| GYROSCOPE | `SENSOR_TYPE_GYROSCOPE` | 2 |
| MAGNETOMETER | `SENSOR_TYPE_MAGNETIC_FIELD` | 6 |
| ABSOLUTE_ORIENTATION_EULER_ANGLES | `SENSOR_TYPE_ORIENTATION` | 256 |
| GRAVITY | `SENSOR_TYPE_GRAVITY` | 257 |
| LINEAR_ACCELERATION | `SENSOR_TYPE_LINEAR_ACCELERATION` | 258 |
| ABSOLUTE_ORIENTATION_QUATERNION | `SENSOR_TYPE_ROTATION_VECTOR` | 259 |
| RELATIVE_ORIENTATION_QUATERNION | `SENSOR_TYPE_GAME_ROTATION_VECTOR` | 262 |

`RELATIVE_ORIENTATION_EULER_ANGLES` has no OHOS sensor; the provider fuses it
from the quaternion, as the other platforms do. `SENSOR_TYPE_AMBIENT_LIGHT`
(5) exists but AMBIENT_LIGHT is left unmapped.

Functions the port needs, all in those two headers:

```
OH_Sensor_GetInfos / CreateInfos / DestroyInfos
OH_SensorInfo_GetType / GetMinSamplingInterval / GetMaxSamplingInterval
OH_Sensor_CreateSubscriber / DestroySubscriber / OH_SensorSubscriber_SetCallback
OH_Sensor_CreateSubscriptionId / DestroySubscriptionId / OH_SensorSubscriptionId_SetType
OH_Sensor_CreateSubscriptionAttribute / DestroySubscriptionAttribute
    / OH_SensorSubscriptionAttribute_SetSamplingInterval
OH_Sensor_Subscribe / OH_Sensor_Unsubscribe
OH_SensorEvent_GetType / GetTimestamp / GetData / GetAccuracy
```

Result codes: `SENSOR_SUCCESS = 0`, `SENSOR_PERMISSION_DENIED = 201`,
`SENSOR_PARAMETER_ERROR = 401`, `SENSOR_SERVICE_EXCEPTION = 14500101`.

**The event callback is a bare `void(*)(Sensor_Event*)` with no user data.**
The only route back to the subscriber is a registry keyed on the type the
event carries, guarded by a lock, because the callback arrives on a platform
sensor thread. This is forced by the API, not a design choice.

Teardown order matters, the same way it does for LocationKit in
`location_provider_ohos.cc`: `OH_Sensor_Unsubscribe` must return before the
callback state it refers to is dropped, because the platform only stops
dispatching once that call returns.

## Permissions

`ohos.permission.ACCELEROMETER` and `ohos.permission.GYROSCOPE` are **already
declared** in `overlay/chromium-ui/entry/src/main/module.json5`, with
`motion_sensor_permission_reason` as their reason string. The runtime bridge
already maps `ContentSettingsType::SENSORS` to both (`OhosPermissionsFor` in
`overlay/chrome/browser/ui/ohos/aura_shell_runtime_bridge.cc`). Nothing on the
shell side should need changing; confirm rather than assume.

## Do not invent unit or axis conversions

Huawei's `UpdatePlatformSensorReading` copies the four floats straight into
`SensorReading::raw.values[0..3]` and converts only the timestamp
(nanoseconds → seconds). There is no axis remapping and no unit conversion
anywhere in their code.

Carry that behaviour unchanged. It is the part that was presumably validated
on real hardware, and it is the part that cannot be checked by reading. If a
reading looks wrong on device, fix it then, with the device in hand -- do not
"correct" a convention up front.

## Build and verify

There is no Chromium tree or toolchain on a development machine. The split is:

| What | Where | How long |
| --- | --- | --- |
| ArkTS (`overlay/chromium-ui`) | local, DevEco's `hvigorw` | seconds |
| Everything C++ | the self-hosted runner, via a push | minutes |

The sensor port is entirely C++, so every iteration is a push.

```bash
# 1. Commit and push to hmos-154-adapter. The push triggers two workflows:
#    "Adapter CI" (~20s) and "Incremental build" (~3min).
git push

# 2. Watch them.
gh run list --limit 3

# 3. On success the build publishes a prerelease tagged build-<first 8 of sha>.
gh release view build-$(git rev-parse HEAD | cut -c1-8)
#    Assets: entry-default-signed.hap, libweb_engine.unstripped.so.gz
gh release download build-<sha8> -p entry-default-signed.hap \
  -R miramira8295/chromium-hmos

# 4. Install. Use uninstall + install, NOT install -r: -r keeps the access
#    token and the Chromium profile, so it never exercises a first grant.
#    Two defects hid in exactly that window during the geolocation work.
hdc uninstall com.caidingding233.chromium
hdc install entry-default-signed.hap
hdc shell aa start -a EntryAbility -b com.caidingding233.chromium
```

`hdc` lives in
`<DevEco>/Contents/sdk/default/openharmony/toolchains` on macOS. The test
device is a PLA-AL10; a debug signing config is already in
`overlay/chromium-ui/build-profile.json5` (locally modified, deliberately not
committed).

The browser exposes CDP; `hdc fport tcp:9222 tcp:9222` then
`http://127.0.0.1:9222/json` drives it, which is how the geolocation port was
verified without touching the screen.

**The device owner runs the on-device verification.** Build it, install it,
state what each step should do -- including the step that used to fail -- and
hand over. Do not drive the phone with `uinput` taps unless asked.

If a `patches/` file is added or changed, it must be applied by hand on the
runner once (rule 2). Say so explicitly when handing work over; CI will go
green either way.

## The patch to write

Model it on `patches/ohos-geolocation.patch`, which has the same three parts:

1. `services/device/generic_sensor/BUILD.gn` -- an `if (is_ohos)` block adding
   the six sources and `libs = [ "ohsensor" ]`.
2. `services/device/generic_sensor/platform_sensor_provider.cc` -- the
   `#elif BUILDFLAG(IS_OHOS)` arm and its `#include`.
3. `services/device/BUILD.gn` -- the unit test, under `if (is_ohos)`, next to
   where `geolocation/ohos/geoposition_ohos_unittest.cc` is added.

That patch also carries a prose header explaining why each hunk exists and
which tag it was generated against. Match it.

## Conventions

- Conventional commits (`feat:`, `fix:`, `refactor:`, `docs:`, `chore:`). No
  attribution trailers -- the repository has none.
- Commit bodies explain the *why*, in full sentences, including what was
  observed and what was ruled out. Read `git log` for the register.
- Comments explain why, not what. A comment that restates the code is noise; a
  comment recording a constraint that is not visible from the code -- a
  threading invariant, an ordering requirement, a platform quirk -- is the
  point.
- Files 200-400 lines, 800 max. Functions under 50.
- Handle errors explicitly at every level. Never swallow one silently: on this
  platform a swallowed error becomes a page that waits forever.
