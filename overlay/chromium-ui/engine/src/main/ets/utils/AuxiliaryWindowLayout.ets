/*
 * Copyright (c) 2026 The Chromium Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

import { AuxiliaryWindowState } from '../components/webwindow/NativeContext';

/**
 * An auxiliary Chromium window as the shell draws it: the native state, the
 * size it is given on screen in vp, and whether it blocks the page.
 */
export interface AuxiliaryWindowView {
  state: AuxiliaryWindowState;
  modal: boolean;
  widthVp: number;
  heightVp: number;
  // Where the window's top-left corner goes, in the shell's coordinates.
  leftVp: number;
  topVp: number;
}

export interface ScreenSizeVp {
  width: number;
  height: number;
}

export type PxToVp = (px: number) => number;

// Room left between a window and the screen edge.
export const AUXILIARY_WINDOW_MARGIN_VP = 12;

// A modal dialog -- the media source picker, say -- may take most of the
// screen; it is the only thing the user can act on anyway. A non-modal one
// -- the "sharing your screen / stop" bar -- shares the screen with the page
// and must leave it usable.
const MODAL_MAX_HEIGHT_FRACTION = 0.8;
const NON_MODAL_MAX_HEIGHT_FRACTION = 0.3;

// Why a tolerance at all: the size set here becomes the XComponent's surface
// size, OhosPlatformWindow::OnNativeSurfaceBoundsChanged hands that back to
// the widget as its bounds, and the widget reports it here again. Converting
// vp to px and back does not round-trip exactly, so without this every report
// could move the window by a fraction of a vp and ask for another layout.
// Clamping alone is idempotent; the rounding is what could creep.
const SIZE_TOLERANCE_VP = 1;

/**
 * Sizes a window from what Chromium asked for, clamped to a phone screen.
 *
 * The first report for a window carries the size Chromium chose. Every later
 * one carries the surface size this function produced, because the surface
 * overwrites the widget's bounds -- so the value only ever goes down to the
 * clamp and stays there, which is what makes the loop settle instead of
 * oscillate. That holds only as long as the XComponent is given exactly this
 * size: padding or a title bar inside the same box would shrink it on every
 * round.
 */
export function layoutAuxiliaryWindow(
  state: AuxiliaryWindowState,
  modal: boolean,
  screen: ScreenSizeVp,
  pxToVp: PxToVp,
  previous?: AuxiliaryWindowView
): AuxiliaryWindowView {
  if (state.anchored === true) {
    return layoutAnchoredWindow(state, screen, pxToVp, previous);
  }
  const maxWidth = Math.max(screen.width - 2 * AUXILIARY_WINDOW_MARGIN_VP, 1);
  const maxHeight = Math.max(
    screen.height * (modal ? MODAL_MAX_HEIGHT_FRACTION : NON_MODAL_MAX_HEIGHT_FRACTION), 1);

  // A window reported with no size yet still needs a surface to draw into;
  // the largest allowed box is the one that cannot cut it off.
  const wantedWidth = state.width > 0 ? pxToVp(state.width) : maxWidth;
  const wantedHeight = state.height > 0 ? pxToVp(state.height) : maxHeight;
  const width = Math.floor(Math.min(wantedWidth, maxWidth));
  const height = Math.floor(Math.min(wantedHeight, maxHeight));

  const keep = previous !== undefined &&
    Math.abs(previous.widthVp - width) <= SIZE_TOLERANCE_VP &&
    Math.abs(previous.heightVp - height) <= SIZE_TOLERANCE_VP;
  const widthVp = keep ? previous!.widthVp : width;
  const heightVp = keep ? previous!.heightVp : height;
  // Centred across the top, where Chrome hangs bubbles.
  return {
    state: state,
    modal: modal,
    widthVp: widthVp,
    heightVp: heightVp,
    leftVp: Math.max((screen.width - widthVp) / 2, 0),
    topVp: AUXILIARY_WINDOW_MARGIN_VP
  };
}

/**
 * A popup Chromium anchored to page content -- an autofill list under a
 * field, a <select> dropdown, a tooltip -- goes exactly where Chromium put
 * it, only pulled back inside the screen. Moving it would detach it from the
 * field it belongs to. It never blocks the page.
 */
function layoutAnchoredWindow(
  state: AuxiliaryWindowState,
  screen: ScreenSizeVp,
  pxToVp: PxToVp,
  previous?: AuxiliaryWindowView
): AuxiliaryWindowView {
  const maxWidth = Math.max(screen.width, 1);
  const maxHeight = Math.max(screen.height, 1);
  let width = Math.floor(Math.min(state.width > 0 ? pxToVp(state.width) : 1, maxWidth));
  let height = Math.floor(Math.min(state.height > 0 ? pxToVp(state.height) : 1, maxHeight));
  if (previous !== undefined &&
    Math.abs(previous.widthVp - width) <= SIZE_TOLERANCE_VP &&
    Math.abs(previous.heightVp - height) <= SIZE_TOLERANCE_VP) {
    width = previous.widthVp;
    height = previous.heightVp;
  }
  const left = Math.min(Math.max(pxToVp(state.x), 0), Math.max(screen.width - width, 0));
  const top = Math.min(Math.max(pxToVp(state.y), 0), Math.max(screen.height - height, 0));
  return { state: state, modal: false, widthVp: width, heightVp: height, leftVp: left, topVp: top };
}

/** Where a window sits horizontally; kept for shells built against 1.0. */
export function auxiliaryWindowLeftVp(view: AuxiliaryWindowView, screen: ScreenSizeVp): number {
  return view.leftVp;
}
