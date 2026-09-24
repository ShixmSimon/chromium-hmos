# Incognito: the shell half

The engine half is on `hmos-154-adapter` (`0aa2291`). It can create the window;
nothing draws it. This is what the shell has to add. The contract it implements
is in `shell-incognito.md`; this is the order to build it in.

## What the engine gives you

One command:

```json
{ "command": "newIncognitoWindow" }
```

It derives the off-the-record profile from the one already open, makes a
browser window on it, and opens a tab. Chromium discards the profile when the
last window on it closes -- there is nothing for the shell to clean up.

One new window event, alongside the `auxiliary` and `pwa` ones the shell
already handles:

```
windowRole: "browser"
incognito:  true
componentId: "aura_win_<widget>"
```

`WebWindow` is already parameterised on the surface it draws into
(`xComponentId`, default `aura_shell`), so a second browser window is a second
`WebWindow` with `xComponentId: state.componentId`. That is the whole reason
this is a small job.

## The four steps

### 1. Do not let it fall into the auxiliary path

`AuraShell.handleAuxiliaryWindowState()` currently sends everything that is not
`pwa` into `auxiliaryWindows`. A `browser` window there gets clamped to a
fraction of the screen, hung from the centre and given a scrim.

```ets
private handleAuxiliaryWindowState(state: AuxiliaryWindowState): void {
  if (state.windowRole === 'pwa') { this.handlePwaWindowState(state); return; }
  if (state.windowRole === 'browser') { this.handleBrowserWindowState(state); return; }
  ...
}
```

Do this first and on its own. Until the next step exists the window simply does
not appear, which is a better failure than appearing as a broken dialog.

### 2. Hold more than one browser window

Today the shell renders one `WebWindow` for the first browser. Give it a list
and a current index:

```ets
interface BrowserWindowView {
  widget: number;
  componentId: string;   // "aura_win_<widget>", must be used verbatim
  incognito: boolean;
}

@State private browserWindows: BrowserWindowView[] = [];
@State private currentBrowserIndex: number = 0;
```

The first window keeps `xComponentId: 'aura_shell'` -- do not renumber it, the
engine binds that surface at startup.

Render the current one only. Two full-screen XComponents at once cost two
surfaces and the phone will not show both anyway.

`handleBrowserWindowState` adds on `visible`, removes on `destroyed`, and when
the removed one was current, falls back to index 0.

### 3. The entry point and the switch

An item in the existing menu that sends `newIncognitoWindow`, and some way back
-- the smallest honest version is an entry in the same menu listing the open
windows, marked by `incognito`.

Worth deciding before writing it: on a phone this reads better as one toggle
("go to incognito" / "leave incognito") than as a window list, because there
will usually be exactly two.

### 4. Say which one the user is in

`incognito` is on the event. What to do with it is a design call -- a badge, a
different toolbar colour, a different tab-strip tint. Pick something that
survives a screenshot: the whole point is that the user can tell at a glance.

## Two things that will bite

**The component id is load-bearing.** The engine keys the "this window is not a
popup" promotion on the `aura_win_` prefix in the surface's component id. Use
`state.componentId` verbatim. Inventing an id, or reusing `aura_shell`, leaves
the window marked auxiliary in the registry: it still draws, but presses land
on the wrong window and the stacking order is a popup's. It will not look like
an id problem.

**Back must not leave the app from the incognito window.** `AuraShell` handles
back by closing the topmost modal, then falling through. With a second browser
window, back at its first tab should close that window and return to the first,
not exit.

## What this does not cover

- **Multi-process.** Phones run `--single-process`; tablets and 2-in-1s get
  `--renderer-process-limit=16`. On those, opening an incognito window spawns a
  renderer process and takes a code path nothing has exercised yet. Test there
  separately.
- **Session semantics.** Every incognito window shares one session today
  (`OTRProfileID::PrimaryID()`). Per-window isolation is a one-line engine
  change (`CreateUnique`) if the product wants it -- say so and it can be done.
- **Guest mode.** A different thing: a standalone temporary profile rather than
  an off-the-record view of this one. Not wired, not needed for this.

## Checking the engine half without any of the above

```bash
hdc shell "hilog -x" | grep -a windowRole
```

Send `newIncognitoWindow`; an event with `"windowRole":"browser"` and
`"incognito":true` should appear. If it does, the engine side is doing its job
and everything left is in the shell.
