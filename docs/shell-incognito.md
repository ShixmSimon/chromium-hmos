# Hosting an incognito window from a shell

The engine can open an incognito window. Nothing draws it yet: a shell has to
recognise a third kind of window and give it a surface. This is what that
takes.

## Why it is a second window

A Chromium `Browser` is bound to one `Profile`. Incognito tabs cannot sit in
the same tab strip as ordinary ones, on any platform. Desktop Chrome opens a
second window for them; so does this.

Chrome and Brave on Android show incognito inside a single Activity, but they
do it through `TabModelSelector` -- a tab-model layer above `Browser` that
exists only in Chromium's Android code. This port is built on the desktop
`Browser`, so it inherits the desktop shape. Building the Android shape instead
would mean writing that layer.

Brave is worth mentioning only for one thing it uses that is available here:
`OTRProfileID::CreateUnique(prefix)` makes off-the-record profiles that are
isolated from each other as well as from the record profile. Brave's Tor
windows use it. `PrimaryID()` is the ordinary incognito profile that every
incognito window shares, which is what `newIncognitoWindow` asks for.

## Not every device can do this

Incognito needs a second `Profile`, a second `Profile` needs a second renderer
process, and HarmonyOS grants native child processes only on 2-in-1/PC and
tablet. A phone is refused (`NCP_ERR_NOT_SUPPORTED`, 801), so Chromium starts
with `--single-process`, where it asserts outright:

```
Single-process mode does not support multiple browser contexts.
```

There is no permission that changes this -- `native_child_process.h` declares
none. It is a device capability, and the engine now asks for it by name
(`OH_Ability_IsNativeChildProcessSupported()`, API 26, resolved at run time
with the device class as a fallback) rather than inferring it from the model.

So the browser state carries the answer:

```
incognitoSupported: false   // phone
incognitoSupported: true    // tablet, 2-in-1
```

Read it and disable or hide the menu item. The engine refuses the command on a
single-process device and logs why; it no longer crashes, but a button that
does nothing is still worse than a button that is not there.

The same state also carries `isIncognito` for the window it describes, so a
shell can mark the current window without waiting for a window event.

## The command

```json
{ "command": "newIncognitoWindow" }
```

Sent like any other browser command. It derives the off-the-record profile from
the profile already open, asks for a window on it, and opens a new tab there.
Chromium discards the profile when the last window using it closes -- a shell
does not have to clear anything.

## The window that comes back

Auxiliary window events already carry `windowRole`. It now has three values:

| `windowRole` | What it is | How a shell hosts it |
|---|---|---|
| `auxiliary` | A popup, menu or dialog | `AuxiliaryWindowLayer`, clamped, positioned by the event |
| `pwa` | A PWA window | `PwaWindowLauncher`, a separate UIAbility |
| `browser` | **A browser window of its own** | Full screen, like the first one |

A `browser` event also carries `incognito` (a boolean), so a shell can mark the
window without asking anything else.

A shell that does not know the role must not fall through to the auxiliary
path. That path clamps the window to a fraction of the screen, hangs it from
the centre and puts a scrim behind it -- which is right for a dialog and wrong
for a browser window.

## The surface

The engine reports the component id it expects for the window:

```
aura_win_<widget>
```

beside the existing `aura_aux_<widget>` and `aura_pwa_<widget>`. A shell must
create its XComponent with exactly that id. The registry recognises the prefix
and promotes the window out of "auxiliary", which is what gets its input
routing and stacking treated as a top-level window rather than a popup over the
first one.

Getting the id wrong is not a drawing bug: the window stays auxiliary, presses
land on the wrong window and the stacking order is wrong.

Native touch and pointer routing in `ohos_aura_shell_napi.cc` picks its target
by that same prefix, not by role: `aura_aux_`, `aura_pwa_` and `aura_win_` each
name a surface the shell hosts for a window of its own, and all three count as
independent windows for input dispatch and targeting, distinct from the main
one.

## What a shell still has to decide

The engine stops at "there is a second browser window and here is its surface".
Everything below is a shell decision and none of it is implemented:

- **Showing two windows.** A phone will not put them side by side, so this is a
  switch, not a layout. `AuraShell.ets` assumes one full-screen `WebWindow`
  today; it needs a set of them and a notion of which is current. That is the
  same job `TabModelSelector` does on Android, one level up.
- **Telling the user which is which.** `incognito` is on the event; what the
  shell does with it -- a badge, a different toolbar colour, a separate
  switcher entry -- is a design question.
- **Closing.** Closing the last incognito window discards the profile. Whether
  the shell warns first, and what it shows when the last one goes away, is
  unspecified.
- **The back gesture.** With two windows, back at the first tab of the
  incognito window should probably leave that window rather than the app.

## Checking the engine half without a shell

The command works today; only the drawing is missing. To see it:

```bash
hdc shell "hilog -x" | grep -a 'windowRole'
```

Send `newIncognitoWindow` and a window event with `"windowRole":"browser"` and
`"incognito":true` should appear. That is the whole engine-side contract.

## Related

- `patches/` has no entry for this: it is all in `overlay/`, so CI picks it up
  with no patch to apply.
- The profile the shell already has (`BrowserProfileService.ets`, the
  `profileId` that becomes `--user-data-dir`) is a different thing and does not
  interact. That one chooses whose data the process uses; incognito derives a
  session on top of whatever that is.
