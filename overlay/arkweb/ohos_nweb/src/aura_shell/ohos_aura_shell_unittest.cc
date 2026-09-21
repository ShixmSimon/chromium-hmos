// Copyright (c) 2026
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "chrome/browser/ui/navigator/browser_navigator_params_utils.h"
#include "chrome/browser/ui/views/chrome_views_delegate_ohos_utils.h"
#include "ohos_nweb/src/aura_shell/ohos_aura_input_router.h"
#include "ohos_nweb/src/aura_shell/ohos_aura_shell_host.h"
#include "ohos_nweb/src/aura_shell/ohos_aura_xcomponent_bridge.h"
#include "ohos_nweb/src/aura_shell/ohos_chrome_main_runner.h"
#include "ui/base/resource/ohos_product_branding.h"
#include "ui/events/event.h"
#include "ui/events/event_constants.h"
#include "ui/events/pointer_details.h"
#include "ui/ozone/platform/ohos/ohos_event_source.h"
#include "ui/ozone/platform/ohos/ohos_native_window_registry.h"
#include "ui/ozone/platform/ohos/ohos_screen.h"

namespace ohos_nweb {
namespace {

bool ContainsArgument(const std::vector<std::string>& arguments,
                      const std::string& expected) {
  return std::ranges::find(arguments, expected) != arguments.end();
}

TEST(NWebAuraShellTest, LocalizedProductBrandingPreservesProtocolAndPlatform) {
  EXPECT_EQ(u"About Chromium", ui::ApplyOhosProductBranding(u"About Chromium"));
  EXPECT_EQ(u"Chromium settings: chrome://settings on ChromeOS",
            ui::ApplyOhosProductBranding(
                u"Google Chrome settings: chrome://settings on ChromeOS"));
}

TEST(NWebAuraShellTest, XComponentStateFollowsLifecycle) {
  OhosAuraXComponentBridge bridge;

  bridge.OnSurfaceCreated();
  bridge.OnVisibilityChanged(true);
  bridge.OnFocusChanged(true);
  bridge.OnSurfaceChanged(1.0, 2.0, 800.0, 600.0, 2.0);

  EXPECT_TRUE(bridge.state().created);
  EXPECT_TRUE(bridge.state().visible);
  EXPECT_TRUE(bridge.state().focused);
  EXPECT_EQ(800.0, bridge.state().width);
  EXPECT_EQ(600.0, bridge.state().height);
  EXPECT_EQ(2.0, bridge.state().density);

  bridge.OnSurfaceDestroyed();
  EXPECT_FALSE(bridge.state().created);
}

TEST(NWebAuraShellTest, KeyEventsOnlyConsumeWhenFocused) {
  OhosAuraInputRouter router;

  EXPECT_FALSE(router.DispatchKeyEvent("{}"));
  EXPECT_FALSE(router.focused_for_testing());
  router.SetFocused(true);
  EXPECT_FALSE(router.DispatchKeyEvent("{}"));
}

TEST(NWebAuraShellTest, ValidKeyEventRestoresRouterFocus) {
  OhosAuraInputRouter router;

  router.SetFocused(false);
  EXPECT_FALSE(
      router.DispatchKeyEvent(R"({"action":0,"keyCode":2017,"keyText":"a"})"));
  EXPECT_TRUE(router.focused_for_testing());
}

TEST(NWebAuraShellTest, PointerPressRestoresInputFocus) {
  OhosAuraInputRouter router;

  router.SetFocused(false);
  router.DispatchPointerEvent(
      R"({"action":2,"pointerType":"mouse","x":10,"y":20})");
  EXPECT_FALSE(router.focused_for_testing());

  router.DispatchPointerEvent(
      R"({"action":0,"pointerType":"mouse","x":10,"y":20})");
  EXPECT_TRUE(router.focused_for_testing());
}

TEST(NWebAuraShellTest, AbsoluteNativeMouseRootIsNotOffsetByWindowOrigin) {
  constexpr char kComponentId[] = "aura_shell_pointer_density_test";
  void* native_window = reinterpret_cast<void*>(0x1234);
  ui::RegisterOhosNativeSurface(kComponentId, native_window,
                                gfx::Rect(640, 320, 1600, 1200), 2.0f);

  OhosAuraInputRouter router;
  router.DispatchPointerEvent(
      R"({"action":0,"pointerType":"mouse","button":0,"x":200,"y":100,"rootX":840,"rootY":420,"physicalPixels":true})");

  EXPECT_DOUBLE_EQ(200.0, router.last_pointer_x_for_testing());
  EXPECT_DOUBLE_EQ(100.0, router.last_pointer_y_for_testing());
  EXPECT_DOUBLE_EQ(840.0, router.last_pointer_root_x_for_testing());
  EXPECT_DOUBLE_EQ(420.0, router.last_pointer_root_y_for_testing());
  ui::UnregisterOhosNativeSurface(kComponentId, native_window);
}

TEST(NWebAuraShellTest, AbsoluteNativeTouchRootIsNotOffsetByWindowOrigin) {
  constexpr char kComponentId[] = "aura_shell_pointer_touch_root_test";
  void* native_window = reinterpret_cast<void*>(0x1234);
  ui::RegisterOhosNativeSurface(kComponentId, native_window,
                                gfx::Rect(640, 320, 1600, 1200), 2.0f);

  OhosAuraInputRouter router;
  router.DispatchPointerEvent(
      R"({"action":0,"pointerType":"touch","button":0,"x":200,"y":100,"rootX":840,"rootY":420,"rootWindowRelative":false,"physicalPixels":true})");

  EXPECT_DOUBLE_EQ(200.0, router.last_pointer_x_for_testing());
  EXPECT_DOUBLE_EQ(100.0, router.last_pointer_y_for_testing());
  EXPECT_DOUBLE_EQ(840.0, router.last_pointer_root_x_for_testing());
  EXPECT_DOUBLE_EQ(420.0, router.last_pointer_root_y_for_testing());
  ui::UnregisterOhosNativeSurface(kComponentId, native_window);
}

TEST(NWebAuraShellTest, NativeTouchPreservesPlatformTimestamp) {
  base::test::SingleThreadTaskEnvironment task_environment;
  ui::OhosEventSource event_source;
  OhosAuraInputRouter router;
  constexpr int64_t kTimestampNs = 123456789;

  router.DispatchNativeTouchEvent({.action = 0,
                                   .pointer_id = 7,
                                   .x = 20.0f,
                                   .y = 30.0f,
                                   .root_x = 120.0f,
                                   .root_y = 230.0f,
                                   .timestamp_ns = kTimestampNs});

  EXPECT_EQ(base::TimeTicks() + base::Nanoseconds(kTimestampNs),
            router.last_touch_timestamp_for_testing());
  EXPECT_DOUBLE_EQ(20.0, router.last_pointer_x_for_testing());
  EXPECT_DOUBLE_EQ(30.0, router.last_pointer_y_for_testing());
  EXPECT_DOUBLE_EQ(120.0, router.last_pointer_root_x_for_testing());
  EXPECT_DOUBLE_EQ(230.0, router.last_pointer_root_y_for_testing());
}

TEST(NWebAuraShellTest, TouchMovesCoalesceWithoutCrossingDiscreteEvents) {
  base::test::SingleThreadTaskEnvironment task_environment;
  ui::OhosEventSource event_source;
  const auto make_touch = [](ui::EventType type, float y) {
    return std::make_unique<ui::TouchEvent>(
        type, gfx::PointF(10.0f, y), gfx::PointF(10.0f, y),
        base::TimeTicks::Now(),
        ui::PointerDetails(ui::EventPointerType::kTouch, 9));
  };

  EXPECT_TRUE(ui::OhosEventSource::PostEvent(
      make_touch(ui::EventType::kTouchMoved, 10.0f)));
  EXPECT_TRUE(ui::OhosEventSource::PostEvent(
      make_touch(ui::EventType::kTouchMoved, 20.0f)));
  auto stats = ui::OhosEventSource::GetTouchMoveQueueStatsForTesting();
  EXPECT_EQ(2u, stats.received);
  EXPECT_EQ(1u, stats.coalesced);
  EXPECT_EQ(1u, stats.pending);

  EXPECT_TRUE(ui::OhosEventSource::PostEvent(
      make_touch(ui::EventType::kTouchReleased, 20.0f)));
  EXPECT_TRUE(ui::OhosEventSource::PostEvent(
      make_touch(ui::EventType::kTouchMoved, 30.0f)));
  stats = ui::OhosEventSource::GetTouchMoveQueueStatsForTesting();
  EXPECT_EQ(3u, stats.received);
  EXPECT_EQ(1u, stats.coalesced);
  EXPECT_EQ(2u, stats.pending);

  task_environment.RunUntilIdle();
  stats = ui::OhosEventSource::GetTouchMoveQueueStatsForTesting();
  EXPECT_EQ(2u, stats.dispatched);
  EXPECT_EQ(0u, stats.pending);
}

TEST(NWebAuraShellTest, LegacyWindowRelativeRootIncludesWindowOrigin) {
  constexpr char kComponentId[] = "aura_shell_pointer_root_test";
  void* native_window = reinterpret_cast<void*>(0x1234);
  ui::RegisterOhosNativeSurface(kComponentId, native_window,
                                gfx::Rect(640, 320, 1600, 1200), 2.0f);

  OhosAuraInputRouter router;
  router.DispatchPointerEvent(
      R"({"action":0,"pointerType":"touch","button":0,"x":200,"y":100,"rootX":200,"rootY":160,"rootWindowRelative":true,"physicalPixels":true})");

  EXPECT_DOUBLE_EQ(840.0, router.last_pointer_root_x_for_testing());
  EXPECT_DOUBLE_EQ(480.0, router.last_pointer_root_y_for_testing());
  ui::UnregisterOhosNativeSurface(kComponentId, native_window);
}

TEST(NWebAuraShellTest, LogicalPointerCoordinatesExpandToPhysicalPixels) {
  constexpr char kComponentId[] = "aura_shell_pointer_density_test";
  void* native_window = reinterpret_cast<void*>(0x1234);
  ui::RegisterOhosNativeSurface(kComponentId, native_window,
                                gfx::Rect(640, 320, 1600, 1200), 2.0f);

  OhosAuraInputRouter router;
  router.DispatchPointerEvent(
      R"({"action":0,"pointerType":"mouse","button":0,"x":100,"y":50})");

  EXPECT_DOUBLE_EQ(200.0, router.last_pointer_x_for_testing());
  EXPECT_DOUBLE_EQ(100.0, router.last_pointer_y_for_testing());
  EXPECT_DOUBLE_EQ(840.0, router.last_pointer_root_x_for_testing());
  EXPECT_DOUBLE_EQ(420.0, router.last_pointer_root_y_for_testing());
  ui::UnregisterOhosNativeSurface(kComponentId, native_window);
}

TEST(NWebAuraShellTest, LosingFocusClearsPointerAndModifierState) {
  OhosAuraInputRouter router;

  router.SetFocused(true);
  router.DispatchPointerEvent(
      R"({"action":0,"pointerType":"mouse","button":0,"x":10,"y":20})");
  EXPECT_NE(0, router.mouse_button_flags_for_testing());

  router.SetFocused(false);
  EXPECT_FALSE(router.focused_for_testing());
  EXPECT_EQ(0, router.event_flags_for_testing());
  EXPECT_EQ(0, router.mouse_button_flags_for_testing());
}

TEST(NWebAuraShellTest, MouseCancelClearsPointerState) {
  OhosAuraInputRouter router;

  router.DispatchPointerEvent(
      R"({"action":0,"pointerType":"mouse","button":0,"x":10,"y":20})");
  EXPECT_NE(0, router.mouse_button_flags_for_testing());

  router.DispatchPointerEvent(
      R"({"action":13,"pointerType":"mouse","button":0,"x":10,"y":20})");
  EXPECT_EQ(0, router.mouse_button_flags_for_testing());
}

TEST(NWebAuraShellTest, DuplicateMouseButtonsAreSuppressedAcrossSources) {
  OhosAuraInputRouter router;

  router.DispatchPointerEvent(
      R"({"action":0,"pointerType":"mouse","dispatchSource":"arkui","button":0,"x":10,"y":20})");
  router.DispatchPointerEvent(
      R"({"action":1,"pointerType":"mouse","dispatchSource":"arkui","button":0,"x":10,"y":20})");
  router.DispatchPointerEvent(
      R"({"action":0,"pointerType":"mouse","dispatchSource":"native","button":0,"x":12,"y":18})");
  router.DispatchPointerEvent(
      R"({"action":1,"pointerType":"mouse","dispatchSource":"native","button":0,"x":12,"y":18})");
  EXPECT_EQ(2, router.suppressed_pointer_events_for_testing());
}

TEST(NWebAuraShellTest, MouseReleaseWithoutButtonClearsPressedState) {
  OhosAuraInputRouter router;

  router.DispatchPointerEvent(
      R"({"action":0,"pointerType":"mouse","dispatchSource":"native","button":0,"x":10,"y":20})");
  EXPECT_NE(0, router.mouse_button_flags_for_testing());

  router.DispatchPointerEvent(
      R"({"action":1,"pointerType":"mouse","dispatchSource":"native","button":-1,"x":10,"y":20})");
  EXPECT_EQ(0, router.mouse_button_flags_for_testing());
}

TEST(NWebAuraShellTest, NewPressRecoversMissedMouseRelease) {
  OhosAuraInputRouter router;

  router.DispatchPointerEvent(
      R"({"action":0,"pointerType":"mouse","dispatchSource":"native","button":0,"x":10,"y":20})");
  EXPECT_NE(0, router.mouse_button_flags_for_testing());

  router.DispatchPointerEvent(
      R"({"action":0,"pointerType":"mouse","dispatchSource":"native","button":0,"x":12,"y":22})");
  EXPECT_EQ(ui::EF_LEFT_MOUSE_BUTTON, router.mouse_button_flags_for_testing());
  EXPECT_EQ(ui::EF_LEFT_MOUSE_BUTTON,
            router.last_mouse_changed_button_flags_for_testing());
}

TEST(NWebAuraShellTest, MouseReleaseRetainsChangedButtonInEventFlags) {
  OhosAuraInputRouter router;

  router.DispatchPointerEvent(
      R"({"action":0,"pointerType":"mouse","dispatchSource":"native","button":0,"x":10,"y":20})");
  EXPECT_EQ(ui::EF_LEFT_MOUSE_BUTTON,
            router.last_mouse_event_flags_for_testing());
  EXPECT_EQ(ui::EF_LEFT_MOUSE_BUTTON,
            router.last_mouse_changed_button_flags_for_testing());

  router.DispatchPointerEvent(
      R"({"action":1,"pointerType":"mouse","dispatchSource":"native","button":0,"x":10,"y":20})");
  EXPECT_EQ(0, router.mouse_button_flags_for_testing());
  EXPECT_EQ(ui::EF_LEFT_MOUSE_BUTTON,
            router.last_mouse_event_flags_for_testing());
  EXPECT_EQ(ui::EF_LEFT_MOUSE_BUTTON,
            router.last_mouse_changed_button_flags_for_testing());
}

TEST(NWebAuraShellTest, MouseReleaseDedupTreatsMissingButtonAsSameButton) {
  OhosAuraInputRouter router;

  router.DispatchPointerEvent(
      R"({"action":0,"pointerType":"mouse","dispatchSource":"native","button":0,"x":10,"y":20})");
  router.DispatchPointerEvent(
      R"({"action":0,"pointerType":"mouse","dispatchSource":"arkui","button":0,"x":10,"y":20})");
  router.DispatchPointerEvent(
      R"({"action":1,"pointerType":"mouse","dispatchSource":"native","button":-1,"x":10,"y":20})");
  router.DispatchPointerEvent(
      R"({"action":1,"pointerType":"mouse","dispatchSource":"arkui","button":0,"x":10,"y":20})");

  EXPECT_EQ(0, router.mouse_button_flags_for_testing());
  EXPECT_EQ(2, router.suppressed_pointer_events_for_testing());
}

TEST(NWebAuraShellTest, BatchedDoubleClickPreservesOneCopyOfEachEvent) {
  OhosAuraInputRouter router;

  for (int click = 0; click < 2; ++click) {
    router.DispatchPointerEvent(
        R"({"action":0,"pointerType":"mouse","dispatchSource":"native","button":0,"x":10,"y":20})");
    router.DispatchPointerEvent(
        R"({"action":1,"pointerType":"mouse","dispatchSource":"native","button":0,"x":10,"y":20})");
  }
  EXPECT_EQ(0, router.suppressed_pointer_events_for_testing());

  for (int click = 0; click < 2; ++click) {
    router.DispatchPointerEvent(
        R"({"action":0,"pointerType":"mouse","dispatchSource":"arkui","button":0,"x":10,"y":20})");
    router.DispatchPointerEvent(
        R"({"action":1,"pointerType":"mouse","dispatchSource":"arkui","button":0,"x":10,"y":20})");
  }
  EXPECT_EQ(4, router.suppressed_pointer_events_for_testing());
}

TEST(NWebAuraShellTest, SeparateClicksFromOneSourceAreNotSuppressed) {
  OhosAuraInputRouter router;

  router.DispatchPointerEvent(
      R"({"action":0,"pointerType":"mouse","dispatchSource":"native","button":0,"x":10,"y":20})");
  router.DispatchPointerEvent(
      R"({"action":1,"pointerType":"mouse","dispatchSource":"native","button":0,"x":10,"y":20})");
  router.DispatchPointerEvent(
      R"({"action":0,"pointerType":"mouse","dispatchSource":"native","button":0,"x":10,"y":20})");

  EXPECT_EQ(0, router.suppressed_pointer_events_for_testing());
}

TEST(NWebAuraShellTest, DuplicateKeysAreSuppressedAcrossSources) {
  OhosAuraInputRouter router;

  EXPECT_FALSE(router.DispatchKeyEvent(
      R"({"action":0,"keyCode":2017,"keyText":"a","dispatchSource":"arkui"})"));
  EXPECT_FALSE(router.DispatchKeyEvent(
      R"({"action":1,"keyCode":2017,"keyText":"a","dispatchSource":"arkui"})"));
  EXPECT_TRUE(router.DispatchKeyEvent(
      R"({"action":0,"keyCode":2017,"keyText":"","dispatchSource":"native"})"));
  EXPECT_TRUE(router.DispatchKeyEvent(
      R"({"action":1,"keyCode":2017,"keyText":"","dispatchSource":"native"})"));
  EXPECT_EQ(2, router.suppressed_key_events_for_testing());
}

TEST(NWebAuraShellTest, SecondaryViewsWidgetsShareTheXComponentCompositor) {
  const gfx::NativeView owner =
      reinterpret_cast<gfx::NativeView>(static_cast<uintptr_t>(1));
  for (const views::Widget::InitParams::Type type :
       {views::Widget::InitParams::TYPE_MENU,
        views::Widget::InitParams::TYPE_TOOLTIP,
        views::Widget::InitParams::TYPE_BUBBLE,
        views::Widget::InitParams::TYPE_POPUP}) {
    views::Widget::InitParams params(
        views::Widget::InitParams::NATIVE_WIDGET_OWNS_WIDGET, type);
    params.parent = owner;
    EXPECT_TRUE(chrome::ohos::ShouldEmbedSecondaryWidget(params));
  }

  views::Widget::InitParams root(
      views::Widget::InitParams::NATIVE_WIDGET_OWNS_WIDGET,
      views::Widget::InitParams::TYPE_WINDOW);
  EXPECT_FALSE(chrome::ohos::ShouldEmbedSecondaryWidget(root));
  root.context = owner;
  EXPECT_TRUE(chrome::ohos::ShouldEmbedSecondaryWidget(root));
}

TEST(NWebAuraShellTest, ChromiumArgumentsEnforceJitlessWasmPhoneFallback) {
  AuraStartupConfig config;
  config.device_class = "phone";
  config.ui_family = "mobile_phone";
  config.user_data_dir = "/data/storage/el2/base/files/chromium-profile";
  config.application_locale = "zh-CN";
  config.color_scheme = "dark";

  OhosChromeMainRunner runner;
  const std::vector<std::string> arguments =
      runner.BuildArgumentsForTesting(config);

  EXPECT_FALSE(
      ContainsArgument(arguments, "--js-flags=--jitless --wasm-jitless"));
  EXPECT_TRUE(ContainsArgument(arguments, "--hide-crash-restore-bubble"));
  EXPECT_TRUE(ContainsArgument(arguments, "--use-gl=angle"));
  EXPECT_TRUE(ContainsArgument(arguments, "--use-angle=gles-egl"));
  EXPECT_TRUE(ContainsArgument(arguments, "--use-mobile-user-agent"));
  EXPECT_TRUE(ContainsArgument(arguments, "--single-process"));
  EXPECT_TRUE(ContainsArgument(
      arguments,
      "--user-data-dir=/data/storage/el2/base/files/chromium-profile"));
  EXPECT_TRUE(ContainsArgument(arguments, "--lang=zh-CN"));
  EXPECT_TRUE(ContainsArgument(arguments, "--ohos-color-scheme=dark"));
  EXPECT_TRUE(
      ContainsArgument(arguments, "--remote-debugging-address=127.0.0.1"));
  EXPECT_TRUE(ContainsArgument(arguments, "--remote-debugging-port=9222"));
  EXPECT_TRUE(ContainsArgument(arguments, kChromiumHomeUrl));
  EXPECT_FALSE(ContainsArgument(arguments, "--no-expose-wasm"));
}

TEST(NWebAuraShellTest, HeadlessRuntimeHasNoWindowGlOrDebugPorts) {
  AuraStartupConfig config;
  config.headless = true;

  OhosChromeMainRunner runner;
  const std::vector<std::string> arguments =
      runner.BuildArgumentsForTesting(config);

  EXPECT_TRUE(ContainsArgument(arguments, "--no-startup-window"));
  EXPECT_TRUE(ContainsArgument(arguments, "--ohos-enable-sync"));
  EXPECT_TRUE(ContainsArgument(arguments, "--disable-gpu"));
  EXPECT_FALSE(ContainsArgument(arguments, "--use-gl=angle"));
  EXPECT_FALSE(ContainsArgument(arguments, "--remote-debugging-port=9222"));
  EXPECT_FALSE(ContainsArgument(arguments, kChromiumHomeUrl));
}

TEST(NWebAuraShellTest, AdditionalSwitchesAreAppended) {
  AuraStartupConfig config;
  config.headless = true;
  config.additional_switches = {
      {"sync-url", "http://192.168.1.20:8295/v2"},
      {"enable-logging", ""},
      {"--no-sandbox", ""},
      {"Bad Key", "x"},
  };

  OhosChromeMainRunner runner;
  const std::vector<std::string> arguments =
      runner.BuildArgumentsForTesting(config);

  EXPECT_TRUE(
      ContainsArgument(arguments, "--sync-url=http://192.168.1.20:8295/v2"));
  EXPECT_TRUE(ContainsArgument(arguments, "--enable-logging"));
  EXPECT_FALSE(ContainsArgument(arguments, "----no-sandbox"));
  EXPECT_FALSE(ContainsArgument(arguments, "--Bad Key=x"));
}

TEST(NWebAuraShellTest, JitlessFollowsTheStartupConfig) {
  AuraStartupConfig config;
  OhosChromeMainRunner runner;

  EXPECT_FALSE(ContainsArgument(runner.BuildArgumentsForTesting(config),
                                "--js-flags=--jitless --wasm-jitless"));

  config.jitless = true;
  EXPECT_TRUE(ContainsArgument(runner.BuildArgumentsForTesting(config),
                               "--js-flags=--jitless --wasm-jitless"));
}

TEST(NWebAuraShellTest, TabletUsesNativeChildProcessMode) {
  AuraStartupConfig config;
  config.device_class = "tablet";
  config.ui_family = "aura_tablet";

  OhosChromeMainRunner runner;
  const std::vector<std::string> arguments =
      runner.BuildArgumentsForTesting(config);

  EXPECT_TRUE(ContainsArgument(arguments, "--renderer-process-limit=16"));
  EXPECT_TRUE(ContainsArgument(arguments, "--use-gl=angle"));
  EXPECT_TRUE(ContainsArgument(arguments, "--use-angle=gles-egl"));
  EXPECT_FALSE(ContainsArgument(arguments, "--use-mobile-user-agent"));
  EXPECT_FALSE(ContainsArgument(arguments, "--single-process"));
}

TEST(NWebAuraShellTest, DisplayMetricsAreIndependentFromWindowBounds) {
  ui::UpdateOhosDisplayMetrics(gfx::Size(3120, 2080), 2.5f);
  const std::optional<ui::OhosDisplayMetrics> metrics =
      ui::GetOhosDisplayMetrics();

  ASSERT_TRUE(metrics.has_value());
  EXPECT_EQ(metrics->pixel_size.width(), 3120);
  EXPECT_EQ(metrics->pixel_size.height(), 2080);
  EXPECT_FLOAT_EQ(metrics->density, 2.5f);
}

TEST(NWebAuraShellTest, PopupWorkAreaTracksEmbeddedChromiumSurface) {
  base::test::SingleThreadTaskEnvironment task_environment;
  constexpr char kComponentId[] = "aura_shell_popup_work_area_test";
  void* native_window = reinterpret_cast<void*>(0x1234);

  ui::UpdateOhosDisplayMetrics(gfx::Size(3120, 2080), 2.0f);
  ui::RegisterOhosNativeSurface(kComponentId, native_window,
                                gfx::Rect(640, 320, 1600, 1200), 2.0f);

  ui::OhosScreen screen;
  const display::Display display =
      screen.GetDisplayNearestPoint(gfx::Point(800, 500));
  EXPECT_EQ(320, display.work_area().x());
  EXPECT_EQ(160, display.work_area().y());
  EXPECT_EQ(800, display.work_area().width());
  EXPECT_EQ(600, display.work_area().height());
  EXPECT_EQ(0, display.bounds().x());
  EXPECT_EQ(0, display.bounds().y());
  EXPECT_EQ(1560, display.bounds().width());
  EXPECT_EQ(1040, display.bounds().height());

  ui::UnregisterOhosNativeSurface(kComponentId, native_window);
}

TEST(NWebAuraShellTest, NativeResizePreservesAuthoritativeScreenOrigin) {
  OhosAuraShellHost host("aura_shell_geometry_test");
  void* native_window = reinterpret_cast<void*>(0x1234);

  host.OnSurfaceChanged(530.0, 357.0, 2091.0, 1394.0, 1.6);
  host.OnNativeSurfaceCreated(native_window, 0.0, 0.0, 2091.0, 1394.0);
  std::optional<ui::OhosNativeSurface> surface =
      ui::GetPrimaryOhosNativeSurface();
  ASSERT_TRUE(surface.has_value());
  EXPECT_EQ(530, surface->bounds.x());
  EXPECT_EQ(357, surface->bounds.y());
  EXPECT_EQ(2091, surface->bounds.width());
  EXPECT_EQ(1394, surface->bounds.height());

  host.OnNativeSurfaceChanged(native_window, 0.0, 0.0, 1800.0, 1200.0);
  surface = ui::GetPrimaryOhosNativeSurface();
  ASSERT_TRUE(surface.has_value());
  EXPECT_EQ(530, surface->bounds.x());
  EXPECT_EQ(357, surface->bounds.y());
  EXPECT_EQ(1800, surface->bounds.width());
  EXPECT_EQ(1200, surface->bounds.height());

  host.OnNativeSurfaceDestroyed(native_window);
}

TEST(NWebAuraShellTest, NativeResizeRebindsUnexpectedReplacementSurface) {
  OhosAuraShellHost host("aura_shell_fold_rebind_test");
  void* first_window = reinterpret_cast<void*>(0x1234);
  void* replacement_window = reinterpret_cast<void*>(0x5678);

  host.OnNativeSurfaceCreated(first_window, 0.0, 0.0, 1200.0, 800.0);
  host.OnNativeSurfaceChanged(replacement_window, 0.0, 0.0, 800.0, 1200.0);

  std::optional<ui::OhosNativeSurface> surface =
      ui::GetPrimaryOhosNativeSurface();
  ASSERT_TRUE(surface.has_value());
  EXPECT_EQ(replacement_window, surface->window);
  EXPECT_EQ(800, surface->bounds.width());
  EXPECT_EQ(1200, surface->bounds.height());

  host.OnNativeSurfaceDestroyed(replacement_window);
}

TEST(NWebAuraShellTest, SurfaceRecreationPreservesRootWindowBinding) {
  constexpr char kComponentId[] = "aura_shell_rebind_test";
  constexpr gfx::AcceleratedWidget kRootWidget = 900001;
  constexpr gfx::AcceleratedWidget kPopupWidget = 900002;
  void* first_window = reinterpret_cast<void*>(0x1234);
  void* replacement_window = reinterpret_cast<void*>(0x5678);

  ui::RegisterOhosNativeSurface(kComponentId, first_window,
                                gfx::Rect(10, 20, 800, 600), 1.5f);
  ASSERT_TRUE(ui::BindOhosNativeSurface(kRootWidget).has_value());
  EXPECT_FALSE(ui::BindOhosNativeSurface(kPopupWidget).has_value());

  ui::UnregisterOhosNativeSurface(kComponentId, first_window);
  ui::RegisterOhosNativeSurface(kComponentId, replacement_window,
                                gfx::Rect(30, 40, 1024, 768), 1.5f);

  std::optional<ui::OhosNativeSurface> rebound_surface =
      ui::GetOhosNativeSurface(kRootWidget);
  ASSERT_TRUE(rebound_surface.has_value());
  EXPECT_EQ(replacement_window, rebound_surface->window);
  EXPECT_EQ(30, rebound_surface->bounds.x());
  EXPECT_EQ(40, rebound_surface->bounds.y());
  EXPECT_EQ(1024, rebound_surface->bounds.width());
  EXPECT_EQ(768, rebound_surface->bounds.height());
  EXPECT_FALSE(ui::GetOhosNativeSurface(kPopupWidget).has_value());

  ui::UnregisterOhosNativeSurface(kComponentId, replacement_window);
  ui::UnbindOhosNativeSurface(kPopupWidget);
  ui::UnbindOhosNativeSurface(kRootWidget);
}

TEST(NWebAuraShellTest, TopmostLogicalWindowReceivesInputWithoutOwnSurface) {
  constexpr gfx::AcceleratedWidget kRootWidget = 900011;
  constexpr gfx::AcceleratedWidget kPopupWidget = 900012;

  ui::RegisterOhosLogicalWindow(kRootWidget, gfx::Rect(0, 0, 1200, 800));
  ui::SetOhosLogicalWindowVisible(kRootWidget, true);
  ui::RegisterOhosLogicalWindow(kPopupWidget, gfx::Rect(700, 40, 360, 520));
  ui::SetOhosLogicalWindowVisible(kPopupWidget, true);

  EXPECT_EQ(kPopupWidget,
            ui::GetOhosAcceleratedWidgetAtScreenPoint(gfx::Point(800, 100)));
  EXPECT_EQ(kRootWidget,
            ui::GetOhosAcceleratedWidgetAtScreenPoint(gfx::Point(100, 100)));
  EXPECT_FALSE(ui::GetOhosNativeSurface(kPopupWidget).has_value());

  ui::SetOhosLogicalWindowVisible(kPopupWidget, false);
  EXPECT_EQ(kRootWidget,
            ui::GetOhosAcceleratedWidgetAtScreenPoint(gfx::Point(800, 100)));

  ui::UnregisterOhosLogicalWindow(kPopupWidget);
  ui::UnregisterOhosLogicalWindow(kRootWidget);
}

TEST(NWebAuraShellTest, NewPressRecoversFromStalePopupInputCapture) {
  base::test::SingleThreadTaskEnvironment task_environment;
  ui::OhosEventSource event_source;
  constexpr gfx::AcceleratedWidget kRootWidget = 900013;
  constexpr gfx::AcceleratedWidget kPopupWidget = 900014;

  ui::RegisterOhosLogicalWindow(kRootWidget, gfx::Rect(0, 0, 1200, 800));
  ui::SetOhosLogicalWindowVisible(kRootWidget, true);
  ui::RegisterOhosLogicalWindow(kPopupWidget, gfx::Rect(700, 40, 360, 520));

  ui::TouchEvent root_touch(
      ui::EventType::kTouchPressed, gfx::PointF(100, 100),
      gfx::PointF(100, 100), base::TimeTicks::Now(),
      ui::PointerDetails(ui::EventPointerType::kTouch, 7));
  EXPECT_EQ(kRootWidget,
            event_source.ResolveDispatchTargetForTesting(&root_touch));

  ui::SetOhosLogicalWindowVisible(kPopupWidget, true);
  ui::TouchEvent popup_touch(
      ui::EventType::kTouchPressed, gfx::PointF(800, 100),
      gfx::PointF(800, 100), base::TimeTicks::Now(),
      ui::PointerDetails(ui::EventPointerType::kTouch, 7));
  EXPECT_EQ(kPopupWidget,
            event_source.ResolveDispatchTargetForTesting(&popup_touch));

  ui::SetOhosLogicalWindowVisible(kPopupWidget, false);
  ui::MouseEvent root_mouse(ui::EventType::kMousePressed, gfx::PointF(100, 100),
                            gfx::PointF(100, 100), base::TimeTicks::Now(),
                            ui::EF_LEFT_MOUSE_BUTTON, ui::EF_LEFT_MOUSE_BUTTON);
  EXPECT_EQ(kRootWidget,
            event_source.ResolveDispatchTargetForTesting(&root_mouse));

  ui::SetOhosLogicalWindowVisible(kPopupWidget, true);
  ui::MouseEvent popup_mouse(ui::EventType::kMousePressed,
                             gfx::PointF(800, 100), gfx::PointF(800, 100),
                             base::TimeTicks::Now(), ui::EF_LEFT_MOUSE_BUTTON,
                             ui::EF_LEFT_MOUSE_BUTTON);
  EXPECT_EQ(kPopupWidget,
            event_source.ResolveDispatchTargetForTesting(&popup_mouse));

  ui::UnregisterOhosLogicalWindow(kPopupWidget);
  ui::UnregisterOhosLogicalWindow(kRootWidget);
}

TEST(NWebAuraShellTest, AuxiliarySurfaceTargetPreventsSheetTouchThrough) {
  base::test::SingleThreadTaskEnvironment task_environment;
  ui::OhosEventSource event_source;
  constexpr gfx::AcceleratedWidget kRootWidget = 900017;
  constexpr gfx::AcceleratedWidget kPopupWidget = 900018;

  ui::RegisterOhosLogicalWindow(kRootWidget, gfx::Rect(0, 0, 1200, 800));
  ui::SetOhosLogicalWindowVisible(kRootWidget, true);
  ui::RegisterOhosLogicalWindow(kPopupWidget, gfx::Rect(700, 40, 360, 520));
  ui::SetOhosLogicalWindowVisible(kPopupWidget, true);

  ui::TouchEvent sheet_press(
      ui::EventType::kTouchPressed, gfx::PointF(40, 80), gfx::PointF(100, 100),
      base::TimeTicks::Now(),
      ui::PointerDetails(ui::EventPointerType::kTouch, 9));
  EXPECT_EQ(kPopupWidget, event_source.ResolveDispatchTargetForTesting(
                              &sheet_press, kPopupWidget));

  ui::TouchEvent sheet_move(
      ui::EventType::kTouchMoved, gfx::PointF(40, 160), gfx::PointF(100, 180),
      base::TimeTicks::Now(),
      ui::PointerDetails(ui::EventPointerType::kTouch, 9));
  EXPECT_EQ(kPopupWidget,
            event_source.ResolveDispatchTargetForTesting(&sheet_move));

  ui::UnregisterOhosLogicalWindow(kPopupWidget);
  ui::UnregisterOhosLogicalWindow(kRootWidget);
}

TEST(NWebAuraShellTest, ExplicitSurfaceTargetAllowsNestedPopupHitTesting) {
  base::test::SingleThreadTaskEnvironment task_environment;
  ui::OhosEventSource event_source;
  constexpr gfx::AcceleratedWidget kRootWidget = 900019;
  constexpr gfx::AcceleratedWidget kPopupWidget = 900020;

  ui::RegisterOhosLogicalWindow(kRootWidget, gfx::Rect(0, 0, 1200, 800));
  ui::SetOhosLogicalWindowVisible(kRootWidget, true);
  ui::RegisterOhosLogicalWindow(kPopupWidget, gfx::Rect(700, 40, 360, 520));
  ui::SetOhosLogicalWindowVisible(kPopupWidget, true);

  ui::TouchEvent popup_touch(
      ui::EventType::kTouchPressed, gfx::PointF(800, 100),
      gfx::PointF(800, 100), base::TimeTicks::Now(),
      ui::PointerDetails(ui::EventPointerType::kTouch, 11));
  EXPECT_EQ(kPopupWidget, event_source.ResolveDispatchTargetForTesting(
                              &popup_touch, kRootWidget));

  ui::MouseEvent popup_mouse(ui::EventType::kMousePressed,
                             gfx::PointF(800, 100), gfx::PointF(800, 100),
                             base::TimeTicks::Now(), ui::EF_LEFT_MOUSE_BUTTON,
                             ui::EF_LEFT_MOUSE_BUTTON);
  EXPECT_EQ(kPopupWidget, event_source.ResolveDispatchTargetForTesting(
                              &popup_mouse, kRootWidget));

  ui::UnregisterOhosLogicalWindow(kPopupWidget);
  ui::UnregisterOhosLogicalWindow(kRootWidget);
}

TEST(NWebAuraShellTest, BoundRootSurfaceIsThePrimaryInputWindow) {
  constexpr char kComponentId[] = "aura_shell_primary_input_test";
  constexpr gfx::AcceleratedWidget kRootWidget = 900015;
  constexpr gfx::AcceleratedWidget kPopupWidget = 900016;
  void* native_window = reinterpret_cast<void*>(0x1234);

  ui::RegisterOhosNativeSurface(kComponentId, native_window,
                                gfx::Rect(76, 151, 2090, 1394), 1.6125f);
  ASSERT_TRUE(ui::BindOhosNativeSurface(kRootWidget).has_value());
  ui::RegisterOhosLogicalWindow(kRootWidget, gfx::Rect(76, 151, 2090, 1394));
  ui::RegisterOhosLogicalWindow(kPopupWidget, gfx::Rect(1550, 340, 570, 1150));

  EXPECT_TRUE(ui::IsOhosPrimaryLogicalWindow(kRootWidget));
  EXPECT_FALSE(ui::IsOhosPrimaryLogicalWindow(kPopupWidget));

  ui::UnregisterOhosLogicalWindow(kPopupWidget);
  ui::UnregisterOhosLogicalWindow(kRootWidget);
  ui::UnbindOhosNativeSurface(kRootWidget);
  ui::UnregisterOhosNativeSurface(kComponentId, native_window);
}

TEST(NWebAuraShellTest, AuxiliaryWindowBindsExactSheetSurfaceAndCloses) {
  constexpr gfx::AcceleratedWidget kWidget = 900021;
  constexpr char kComponentId[] = "aura_aux_900021";
  void* native_window = reinterpret_cast<void*>(0x9876);
  std::vector<ui::OhosLogicalWindowState> states;
  bool close_requested = false;

  EXPECT_FALSE(ui::BindOhosNativeSurface(kWidget).has_value());
  ui::RegisterOhosLogicalWindow(kWidget, gfx::Rect(40, 80, 720, 900));
  ui::SetOhosLogicalWindowCloseCallback(
      kWidget, base::BindRepeating([](bool* requested) { *requested = true; },
                                   base::Unretained(&close_requested)));
  ui::SetOhosLogicalWindowStateCallback(base::BindRepeating(
      [](std::vector<ui::OhosLogicalWindowState>* events,
         const ui::OhosLogicalWindowState& state) { events->push_back(state); },
      base::Unretained(&states)));

  ui::SetOhosLogicalWindowVisible(kWidget, true);
  ASSERT_FALSE(states.empty());
  EXPECT_EQ(kWidget, states.back().widget);
  EXPECT_TRUE(states.back().auxiliary);
  EXPECT_TRUE(states.back().visible);

  ui::RegisterOhosNativeSurface(kComponentId, native_window,
                                gfx::Rect(0, 0, 720, 900), 2.0f);
  std::optional<ui::OhosNativeSurface> surface =
      ui::GetOhosNativeSurface(kWidget);
  ASSERT_TRUE(surface.has_value());
  EXPECT_EQ(native_window, surface->window);
  EXPECT_EQ(kWidget,
            ui::GetOhosAcceleratedWidgetForNativeSurface(kComponentId));
  EXPECT_TRUE(ui::RequestCloseOhosLogicalWindow(kWidget));
  EXPECT_TRUE(close_requested);

  ui::UnregisterOhosNativeSurface(kComponentId, native_window);
  ui::UnregisterOhosLogicalWindow(kWidget);
  ASSERT_FALSE(states.empty());
  EXPECT_TRUE(states.back().destroyed);
  ui::UnbindOhosNativeSurface(kWidget);
  ui::SetOhosLogicalWindowStateCallback({});
}

TEST(NWebAuraShellTest, PwaWindowBindsExactAbilitySurfaceAndBecomesPrimary) {
  constexpr gfx::AcceleratedWidget kWidget = 900022;
  constexpr char kComponentId[] = "aura_pwa_900022";
  void* native_window = reinterpret_cast<void*>(0x9877);
  std::vector<ui::OhosLogicalWindowState> states;
  gfx::Rect callback_bounds;

  EXPECT_FALSE(ui::BindOhosNativeSurface(kWidget).has_value());
  ui::ExpectOhosNativeSurface(kWidget);
  EXPECT_TRUE(ui::IsOhosNativeSurfaceExpected(kWidget));
  ui::RegisterOhosLogicalWindow(kWidget, gfx::Rect(40, 80, 720, 900));
  ui::SetOhosNativeSurfaceBoundsCallback(
      kWidget, base::BindRepeating([](gfx::Rect* observed, gfx::Rect bounds,
                                      float) { *observed = bounds; },
                                   base::Unretained(&callback_bounds)));
  ui::SetOhosLogicalWindowStateCallback(base::BindRepeating(
      [](std::vector<ui::OhosLogicalWindowState>* events,
         const ui::OhosLogicalWindowState& state) { events->push_back(state); },
      base::Unretained(&states)));
  ui::SetOhosLogicalWindowVisible(kWidget, true);
  ASSERT_FALSE(states.empty());
  EXPECT_TRUE(states.back().auxiliary);

  ui::RegisterOhosNativeSurface(kComponentId, native_window,
                                gfx::Rect(120, 160, 1200, 800), 2.0f);
  std::optional<ui::OhosNativeSurface> surface =
      ui::GetOhosNativeSurface(kWidget);
  ASSERT_TRUE(surface.has_value());
  EXPECT_EQ(native_window, surface->window);
  EXPECT_EQ(120, callback_bounds.x());
  EXPECT_EQ(160, callback_bounds.y());
  EXPECT_EQ(1200, callback_bounds.width());
  EXPECT_EQ(800, callback_bounds.height());
  EXPECT_TRUE(
      ui::WaitForOhosNativeSurface(kWidget, base::Milliseconds(1)).has_value());
  EXPECT_TRUE(ui::IsOhosPrimaryLogicalWindow(kWidget));
  ASSERT_FALSE(states.empty());
  EXPECT_TRUE(states.back().destroyed);
  const auto component_id =
      ui::GetOhosNativeSurfaceComponentIdForWidget(kWidget);
  ASSERT_TRUE(component_id.has_value());
  EXPECT_EQ(kComponentId, *component_id);

  ui::UnregisterOhosNativeSurface(kComponentId, native_window);
  ui::UnregisterOhosLogicalWindow(kWidget);
  ui::SetOhosNativeSurfaceBoundsCallback(kWidget, {});
  ui::UnbindOhosNativeSurface(kWidget);
  ui::SetOhosLogicalWindowStateCallback({});
}

TEST(NWebAuraShellTest, ApplicationWindowIdsFollowBoundChromiumWidgets) {
  constexpr gfx::AcceleratedWidget kMainWidget = 900023;
  constexpr gfx::AcceleratedWidget kPwaWidget = 900024;
  constexpr char kMainComponent[] = "aura_shell_window_id_test";
  constexpr char kPwaComponent[] = "aura_pwa_900024";
  void* main_window = reinterpret_cast<void*>(0x9880);
  void* pwa_window = reinterpret_cast<void*>(0x9881);

  ui::RegisterOhosNativeSurface(kMainComponent, main_window,
                                gfx::Rect(0, 0, 1200, 800), 2.0f);
  ASSERT_TRUE(ui::BindOhosNativeSurface(kMainWidget).has_value());
  EXPECT_FALSE(ui::BindOhosNativeSurface(kPwaWidget).has_value());
  ui::RegisterOhosLogicalWindow(kPwaWidget, gfx::Rect(0, 0, 800, 600));
  ui::RegisterOhosNativeSurface(kPwaComponent, pwa_window,
                                gfx::Rect(100, 100, 800, 600), 2.0f);
  ui::SetOhosApplicationWindowId(101);
  ui::SetOhosApplicationWindowIdForNativeSurface(kMainComponent, 101);
  ui::SetOhosApplicationWindowIdForNativeSurface(kPwaComponent, 202);

  EXPECT_EQ(101, ui::GetOhosApplicationWindowIdForWidget(kMainWidget));
  EXPECT_EQ(202, ui::GetOhosApplicationWindowIdForWidget(kPwaWidget));
  EXPECT_EQ(101, ui::GetOhosApplicationWindowId());

  ui::UnregisterOhosNativeSurface(kPwaComponent, pwa_window);
  ui::UnregisterOhosNativeSurface(kMainComponent, main_window);
  ui::UnregisterOhosLogicalWindow(kPwaWidget);
  ui::UnbindOhosNativeSurface(kPwaWidget);
  ui::UnbindOhosNativeSurface(kMainWidget);
}

TEST(NWebAuraShellTest, SeparateWindowsOpenAsForegroundTabs) {
  EXPECT_EQ(WindowOpenDisposition::NEW_FOREGROUND_TAB,
            GetPlatformAdjustedWindowOpenDisposition(
                WindowOpenDisposition::NEW_POPUP));
  EXPECT_EQ(WindowOpenDisposition::NEW_FOREGROUND_TAB,
            GetPlatformAdjustedWindowOpenDisposition(
                WindowOpenDisposition::NEW_WINDOW));
  EXPECT_EQ(WindowOpenDisposition::NEW_BACKGROUND_TAB,
            GetPlatformAdjustedWindowOpenDisposition(
                WindowOpenDisposition::NEW_BACKGROUND_TAB));
  EXPECT_EQ(WindowOpenDisposition::CURRENT_TAB,
            GetPlatformAdjustedWindowOpenDisposition(
                WindowOpenDisposition::CURRENT_TAB));
}

}  // namespace
}  // namespace ohos_nweb
