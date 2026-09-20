# Symbolizing a device crash

A HarmonyOS crash report names `libweb_engine.so` and an offset:

```
Reason:Signal:SIGSEGV(SEGV_MAPERR)@0x0000000000000040
Tid:53256, Name:CrBrowserMain
#00 pc 0000000005dc6284 /data/storage/el1/bundle/libs/arm64/libweb_engine.so(5c85dde2...)
#01 pc 0000000005da9a74 /data/storage/el1/bundle/libs/arm64/libweb_engine.so(5c85dde2...)
```

The shipped library is stripped, so those offsets mean nothing on their own.
Every build publishes `libweb_engine.unstripped.so.gz` alongside the HAP for
exactly this.

## Getting the crash report

The faultlog directory is not readable on a production device, but
`processdump` also writes the frames to hilog, which is:

```bash
hdc shell hilog -x | grep -E 'Reason:Signal|DfxFaultLogger: (Tid|#[0-9]+ pc)'
```

## Resolving the frames

```bash
gh release download build-<sha> -p 'libweb_engine.unstripped.so.gz'
gunzip libweb_engine.unstripped.so.gz

llvm-symbolizer --obj=libweb_engine.unstripped.so --demangle --inlines \
  0x5dc6284 0x5da9a74
```

Which gives, for the crash above:

```
base::ObserverList<device::HidService::Observer>::AddObserver(...)
device::DeviceService::BindHidManager(...)
```

The file carries DWARF, so file and line come out too where the compiler kept
them.

## Check the build id first

Every frame carries the build id of the library it came from:

```
libweb_engine.so(5c85dde2dba08f6db39ecd1d32c325792a62077d)
```

Compare it against the symbols you downloaded:

```bash
llvm-readelf -n libweb_engine.unstripped.so | grep -i 'build id'
```

If they differ, the device is running a different build and every symbol you
get back will be wrong in a way that looks entirely plausible — a real function
name, from the wrong place. This is worth checking every time, not only when a
result looks odd.

## Reading the signal

The signal usually says what kind of defect it is before you resolve anything:

| Signal | Usually means |
|---|---|
| `SIGSEGV(SEGV_MAPERR)@0x0` or a small offset | null dereference — an unimplemented platform backend returned null and nothing checked |
| `SIGTRAP(TRAP_BRKPT)` | a `CHECK`/`NOTREACHED` fired, or UBSan trapped. This build compiles with `-fsanitize-trap=return`, so a function falling off the end also lands here |

Both have shown up on this port. `NOTREACHED()` in particular is how upstream
marks "no platform implementation", and on OHOS those paths are reachable from
a page.
