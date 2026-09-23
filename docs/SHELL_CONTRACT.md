# 外壳接口约定

这份文档写给开发浏览器外壳的人。外壳只负责界面,Chromium 引擎通过 `engine` HAR 提供。文档说明 HAR 提供什么、外壳必须做什么、哪些事情不能做。

内容以 `hmos-154-adapter` 分支 `286a844` 为准。HAR 的对外接口定义在 `overlay/chromium-ui/engine/Index.ets`。现在仓库里的 `overlay/chromium-ui/entry` 就是一个完整的外壳,可以当作参考实现。

---

## 1. 职责划分

| 由 HAR 负责(外壳不用管) | 由外壳负责 |
|---|---|
| Chromium 引擎(`libweb_engine.so`、`libnweb_render.so`)和运行时资源 | 应用入口 `EntryAbility` 和页面 |
| 网页渲染:SURFACE 上屏、VSync、帧节奏 | 地址栏、标签栏、菜单等所有界面 |
| 硬件能力:定位、蓝牙、USB/HID/串口、摄像头、麦克风、屏幕共享、传感器、振动、电池、屏幕常亮 | 启动配置、前后台状态、窗口尺寸和折叠状态的变化 |
| 蓝牙和 USB 的 ArkTS 桥接(由 `WebWindow` 自动创建) | 响应引擎发来的事件:文件选择、系统权限、打印、分享等(见第 5 节) |
| 权限声明、权限说明文案 | 辅助窗口的显示位置(见第 7 节) |
| | 应用签名、包名、图标 |

原则是:**凡是 Chromium 会回调的东西,都在 HAR 里。** 外壳漏接某个事件,网页那一侧会一直等待,不会报错。所以第 5 节标为"必须响应"的事件,一个都不能漏。

---

## 2. 引入 HAR

### 获取

每次 CI 构建都会把 `engine.har` 发布到 release `build-<commit 前 8 位>`,和同一次构建的 HAP 放在一起。在国内网络下,可以通过代理下载:

```
https://gh-proxy.org/https://github.com/miramira8295/chromium-hmos/releases/download/build-<sha8>/engine.har
```

HAR 约 128MB,包含:
- 两个 `.so`;
- 64 个运行时文件(`resources/rawfile/chromium/`);
- 全部 ArkTS 类型声明;
- 权限声明。

### 依赖声明

把 `engine.har` 放到外壳工程里,例如 `libs/engine.har`,然后在模块的 `oh-package.json5` 中声明:

```json5
{
  "dependencies": {
    "engine": "file:./libs/engine.har"
  }
}
```

然后执行 `ohpm install`。

> **注意:HAR 的版本号目前固定是 `1.0.0`。** 如果替换了 `engine.har` 但版本号没变,ohpm 可能继续用缓存里的旧包。替换后请删除 `oh_modules/engine` 再执行 `ohpm install`。之后我们会把 commit 号写进版本号。

### 平台要求

- **仅支持 arm64-v8a。**
- `compatibleSdkVersion` 最低为 `6.0.0(20)`。引擎用到了 API 20 的 `getSelfPermissionStatus`。
- 包名不限。引擎里没有写死任何包名。

---

## 3. 最小外壳

外壳至少需要两部分:一个 `EntryAbility`,负责把启动配置写进 `AppStorage` 并在前后台切换时更新;一个页面,放一个 `WebWindow`。

### EntryAbility

```ts
import { UIManager, CHROMIUM_HOME_URL, handleNotificationWant } from 'engine';

export default class EntryAbility extends UIAbility {
  onCreate(want: Want): void { handleNotificationWant(want); }
  onNewWant(want: Want): void { handleNotificationWant(want); }
  onWindowStageCreate(windowStage: window.WindowStage): void {
    AppStorage.setOrCreate('startupConfig',
      UIManager.serializeAuraStartupConfig(CHROMIUM_HOME_URL));
    AppStorage.setOrCreate('routingRevision', 0);
    AppStorage.setOrCreate('appVisible', true);
    windowStage.loadContent('pages/Browser');
  }
  onForeground(): void { AppStorage.setOrCreate('appVisible', true); }
  onBackground(): void { AppStorage.setOrCreate('appVisible', false); }
}
```

窗口尺寸变化、折叠状态变化、深浅色切换时,需要做两件事:
1. 用 `UIManager.serializeAuraStartupConfig(url, windowState, foldStatus, colorScheme)` 重新生成配置;
2. 把 `routingRevision` 加 1。

完整写法参考 `entry/src/main/ets/entryability/EntryAbility.ets` 里的 `refreshRouting`。

`AppStorage` 的键名可以随意取,引擎只看 `WebWindow` 收到的参数。

**`handleNotificationWant` 必须接上。** 用户点网页通知时,系统通过 `onCreate`(应用已被杀掉)或 `onNewWant`(应用在后台)把应用拉起来,只有外壳的 `EntryAbility` 能收到这次拉起。不转交的话,通知照常显示,但点了只会打开应用,网页收不到 click 事件。函数返回 true 表示这次拉起来自通知,外壳把浏览器调到前台即可,不需要再做别的。

### 页面

```ts
import {
  WebWindow, BrowserCommandChannel, BrowserCommandPayload,
  BrowserStateSnapshot, BrowserRuntimeEvent, AuxiliaryWindowState
} from 'engine';

@Entry
@ComponentV2
struct Browser {
  @Local state?: BrowserStateSnapshot;
  @Local commandText: string = '';
  @Local commandRevision: number = 0;
  @Local startupConfig: string = AppStorage.get<string>('startupConfig') ?? '';
  // routingRevision / appVisible 同理,从 AppStorage 取值并保持同步

  private readonly commands = new BrowserCommandChannel((text: string): void => {
    this.commandText = text;
    this.commandRevision += 1;
  });

  build() {
    Stack() {
      WebWindow({
        startupConfigText: this.startupConfig,
        browserCommandText: this.commandText,
        browserCommandRevision: this.commandRevision,
        onBrowserState: (s: BrowserStateSnapshot) => { this.state = s; },
        onRuntimeEvent: (e: BrowserRuntimeEvent) => { /* 见第 5 节,必须处理 */ },
        onAuxiliaryWindowState: (w: AuxiliaryWindowState) => { /* 见第 7 节 */ }
      })
        .width('100%')
        .height('100%')
      // 你的地址栏、按钮等叠在这里,注意第 8 节的约束
    }
  }
}
```

---

## 4. `WebWindow` 参数

| 参数 | 类型 | 说明 |
|---|---|---|
| `startupConfigText` | string | `UIManager.serializeAuraStartupConfig(...)` 生成的 JSON。**必填。** |
| `routingRevision` | number | 窗口、折叠或深浅色变化后加 1。引擎会重新读取 `startupConfigText`。 |
| `appVisible` | boolean | 应用在前台时为 true。引擎用它更新网页的可见状态和输入焦点,切到后台时会通知 Chromium 页面已隐藏。 |
| `url` | string | 初始网址,默认 `chrome://newtab/`。启动后改它会触发导航。 |
| `profileId` | string | 用户配置目录的 ID,默认 `'local'`。 |
| `browserCommandText` / `browserCommandRevision` | string / number | 命令通道。**只能通过 `BrowserCommandChannel` 写入**,见第 6 节。 |
| `auxiliaryDismissWidget` / `auxiliaryDismissRevision` | number / number | 关闭一个辅助窗口,见第 7 节。 |
| `themeFontId` / `themeFontRevision` | string / number | 系统字体变化时传入。 |
| `xComponentId` | string | 默认 `'aura_shell'`。一个进程里只能有一个浏览器 `WebWindow`,不要改。 |
| `manageWindowDecor` | boolean | 默认 true。PWA 窗口模式下由引擎控制窗口装饰。 |
| `surfaceReadyStorageKey` | string | 渲染就绪后,引擎把 true 写到 `AppStorage` 的这个键,可用来撤掉启动画面。 |
| `onBrowserState` | 回调 | 浏览器状态有变化时调用,见下文。 |
| `onRuntimeEvent` | 回调 | 引擎请求外壳做事,见第 5 节。 |
| `onAuxiliaryWindowState` | 回调 | 辅助窗口出现、移动或消失,见第 7 节。 |
| `onWindowStatus` | 回调 | 窗口状态(全屏、最大化等)变化。 |
| `onError` | 回调 | 引擎初始化失败等错误。 |

`WebWindow` 按自己实际占的区域通知 Chromium，不必铺满窗口：地址栏这类不透明的栏放在它上方或下方，网页不会被挡。要让网页从悬浮栏下面透出来，就让 `WebWindow` 铺到那条栏下面，再用 `setViewportInsets` 命令告诉引擎被盖住的高度（见第 6 节）。

### `BrowserStateSnapshot`

引擎每 200ms 检查一次状态,有变化才回调。常用字段:

| 字段 | 说明 |
|---|---|
| `url`、`domain`、`title` | 当前标签页。**`url` 只用于显示**:`data:` 网址只保留开头的类型部分,其他网址最长 2048 个字符。 |
| `loading`、`canGoBack`、`canGoForward` | 用于驱动地址栏和前进后退按钮。 |
| `tabs`、`tabCount`、`activeTabIndex` | 标签列表。每个标签有 `index`、`active`、`url`、`title`、`loading`。 |
| `mobileUi`、`uiFamily` | 引擎当前使用的 UI 形态。 |
| `isPwaWindow`、`pwaAppId`、`pwaStartUrl` | 当前窗口是否是 PWA。 |
| `version` | 协议版本。 |

---

## 5. 事件(`onRuntimeEvent`)

引擎需要调用系统能力时,会发出一个事件。标为**必须响应**的事件,网页那一侧会一直等待外壳的回复;漏掉的话,文件选择框和权限请求会永远没有结果。

| 事件 | 必须响应 | 处理方式 |
|---|---|---|
| `filePickerRequested` | **是** | `HarmonyFilePickerAdapter.show(context, event)`,拿到结果后发送 `filePickerResult` 命令。**出错或用户取消时也要发**,带上 `canceled: true`。 |
| `permissionsRequested` | **是** | `PermissionRequestAdapter.request(context, event)`,然后发送 `permissionResult` 命令。**出错时也要发**,把全部权限放进 `denied`。发完之后再上报一次 `systemPermissionState`(见下文)。 |
| `systemPrintReady` | 否 | `SystemPrintAdapter.printPdf(context, event.filePath)` |
| `shareRequested` | 否 | `HuaweiShareAdapter.sharePage(context, event.url, event.title)` |
| `systemActionRequested` | 否 | `SystemIntegrationAdapter.handle(context, event.action)` |
| `castRequested` | 否 | 投屏选择器。参考实现在 `entry/.../CurrentTabCastSession.ets`,还没有移进 HAR。 |
| `pwaInstalled`、`pwaMenuModel`、`pwaMenuClosed` | 否 | PWA 相关,需要时参考 `entry/` 的实现。 |
| `systemPrintFailed`、`inputRecovered` | 否 | 记日志即可。 |
| `findResult` | 否 | `matches` 匹配总数，`activeMatch` 当前是第几个（从 1 开始，0 表示没有），`finalUpdate` 为 true 时计数已定 |
| `pageText` | 否 | 回复 `getPageText`：`requestId`、`text` |
| `browserControlsRatio` | 否 | 用了 `setBrowserControls` 才会有。`ratio` 为顶栏当前露出的比例（1 全部显示，0 完全滑走），外壳把顶栏往上移 `(1 - ratio) × 顶栏高度`。 |

### 定位权限状态(必须做)

Chromium 在弹出网站的定位授权框之前,会先看应用自己有没有定位权限。外壳需要在两个时机上报这个状态:
- 第一次收到 `onBrowserState` 时;
- 每次处理完 `permissionsRequested` 后。

```ts
PermissionRequestAdapter.observeLocation(() => report());   // 用户在系统设置里改了权限时,也会重新上报
function report() {
  commands.send({ command: 'systemPermissionState',
                  location: PermissionRequestAdapter.locationState() });
}
```

页面销毁时调用 `PermissionRequestAdapter.stopObservingLocation()`。

---

## 6. 命令

外壳通过 `BrowserCommandChannel.send(payload)` 给引擎发命令。

**不要直接写 `browserCommandText`。** ArkUI 会把同一段同步代码里的多次状态修改合并成一次,直接连写两条命令,第一条会悄无声息地丢掉。`BrowserCommandChannel` 会把同一批命令打包成数组一次发出。

| `command` | 参数 | 作用 |
|---|---|---|
| `navigate` | `url` | 在当前标签页打开网址 |
| `home` | | 回到主页 |
| `newTab` | `url?` | 新建标签页 |
| `activateTab` | `index` | 切换标签页 |
| `closeTab` | `index` | 关闭标签页 |
| `requestState` | | 让引擎立即推送一次 `BrowserStateSnapshot` |
| `filePickerResult` | `requestId`, `paths`, `fileTypeIndex`, `canceled` | 回复 `filePickerRequested` |
| `permissionResult` | `requestId`, `granted`, `denied` | 回复 `permissionsRequested` |
| `systemPermissionState` | `location`: `'allowed' \| 'denied' \| 'notDetermined'` | 上报应用的定位权限状态 |
| `recoverInput` | | 触摸或焦点异常时让引擎恢复输入 |
| `setBrowserControls` | `top`（vp） | 外壳顶栏的高度，交给 Chromium 当作 browser controls：网页顶部给它留出位置，网页下滑时顶栏被滑走，上滑时再出现。显示比例通过 `browserControlsRatio` 事件告诉外壳，外壳据此移动顶栏；传 0 取消 |
| `findInPage` | `text`, `forward?` | 在当前标签页查找，同一段文字再发一次即跳到下一个（`forward: false` 为上一个）。结果通过 `findResult` 事件返回 |
| `stopFind` | | 结束查找，清除高亮 |
| `getPageText` | `requestId` | 读取当前网页的可见文字（最多 20000 字），在独立的脚本环境里执行，网页自己的脚本看不到。结果通过 `pageText` 事件返回 |
| `setViewportInsets` | `bottom`（vp） | 外壳在网页底部盖了多高的悬浮栏。网页照常画到底，但可视区域缩小这么多，网页末尾能滚到悬浮栏上方。切换标签、新建标签后引擎会自动沿用，不用重发；传 0 取消 |
| `pwaHome`、`pwaMenu`、`pwaMenuAction`、`pwaMenuDismiss` | 见 `BrowserCommandPayload` | PWA 窗口菜单 |
| `defaultBrowserState`、`systemCapabilities` | 见参考实现 | 系统集成相关状态 |

后退、前进、刷新这类命令,以 `aura_shell_runtime_bridge.cc` 里 `ExecuteAuraShellBrowserCommand` 实际接受的为准。这张表是按 `286a844` 整理的。

---

## 7. 辅助窗口

Chromium 的一部分窗口是独立的原生窗口,需要外壳给它们放一个 XComponent,比如屏幕共享时的"正在共享"提示条。权限气泡和设备选择框这类弹窗不属于这种情况,它们画在网页区域里面,外壳不用处理。

`onAuxiliaryWindowState` 每次收到一个窗口的完整状态:

- `visible && !destroyed`:显示或更新这个窗口;否则移除。
- `windowRole === 'pwa'`:这是 PWA 窗口,要用单独的应用窗口打开。参考 `entry/.../PwaWindowLauncher.ets`。
- `stackingOrder` 越大越靠上。
- `modal` 可能缺省,缺省时沿用上一次的值。模态窗口下方应该加一层遮罩。

每个辅助窗口需要一个这样的 XComponent,三个参数都不能改:

```ts
XComponent({
  id: state.componentId,          // 必须用引擎给的 componentId
  type: XComponentType.SURFACE,
  libraryname: 'web_engine'
})
  .id(state.componentId)
```

尺寸和位置用 HAR 提供的 `layoutAuxiliaryWindow(state, modal, screenSizeVp, px2vp, previous)` 和 `auxiliaryWindowLeftVp(...)` 计算。它们会把窗口限制在屏幕内,并水平居中。

- **不要给它的外层容器加 `hitTestBehavior(HitTestMode.Block)`**,否则 XComponent 收不到点击,提示条上的"停止"按钮会失灵。用默认值就行。
- **返回键**:如果有模态辅助窗口,在 `onBackPress` 里通过 `auxiliaryDismissWidget = widget`、`auxiliaryDismissRevision += 1` 关闭最上层的那个,并返回 true。

完整实现参考 `entry/.../AuraShell.ets` 里的 `AuxiliaryWindowLayer` 和 `onBackPress`。这部分之后会移进 HAR,做成一个组件。

---

## 8. 渲染约束

网页通过 SURFACE 模式的 XComponent 上屏,是系统合成器里一个独立的图层,可以交给显示硬件直接合成。下面两种情况会让它退回 GPU 合成:系统每帧多做一次全屏绘制,更费电,也更容易掉帧。

1. **网页区域上方不要叠带模糊效果的控件**,比如 `backgroundBlurStyle`、`blur`、`backdropBlur`。地址栏、底栏这类悬浮在网页上方的控件请用不透明或半透明的纯色背景。
2. **不要给 `WebWindow` 或它的任何父容器设置 `opacity`,也不要做透明度动画。**

另外:
- **不要把 `WebWindow` 包进 TEXTURE 模式的容器,也不要对它截图做特效。** 引擎已经测过,TEXTURE 模式会让每一帧都绕道 ArkUI 主线程。
- 页面背景色建议和网页默认背景一致,这样旋转或加载时的空隙不会闪烁。

### 怎么自查

用 DevEco Profiler 抓一段 trace,在 `render_service` 进程里搜 `DrawImage(GPU)`。滑动网页时,如果 `RSUniRenderThread` 上出现大量这类标记,说明网页图层没有走硬件合成。

---

## 9. 输入约束

触摸事件的路径是:系统 → ArkUI 主线程 → XComponent → Chromium 浏览器主线程 → 网页。

1. **ArkUI 主线程上不要有长任务。** 外壳主线程每占用 8ms,网页的触摸就会晚一帧送达,用户会感觉"钝一下"。耗时操作请放到 `taskpool` 或 `worker` 里。
2. **不要在 `WebWindow` 的父容器上拦截触摸。** 不要给父容器加 `onTouch` 并调用 `stopPropagation`,也不要加会抢占滑动的手势,比如父容器上的 `PanGesture` 或下拉刷新。需要侧滑返回之类的手势,只放在屏幕边缘的窄条里。
3. **悬浮在网页上方的控件只占用自己的区域。** 不要用透明的全屏容器去接收点击。

---

## 10. 权限

HAR 的 `module.json5` 声明了 12 项权限(包括 WebAuthn 用到的 `ACCESS_BIOMETRIC`),并附带权限说明文案。打包时这些权限会自动合并进外壳的 HAP,外壳不需要重复声明。
- 外壳自己用到的权限,在外壳里另外声明。
- 如果外壳的资源和 HAR 里的同名,外壳的会覆盖 HAR 的。所以不要在外壳里定义同名的权限说明字符串,除非你是有意要改这段文案。

蓝牙权限在网页第一次调用 `navigator.bluetooth` 时由 HAR 申请,定位、摄像头、麦克风等权限通过第 5 节的 `permissionsRequested` 事件申请。外壳不需要在启动时提前申请任何权限。

---

## 11. 已知限制

| 限制 | 说明 |
|---|---|
| 一个进程只能有一个浏览器 `WebWindow` | 同一个进程里只能初始化一次 Chromium。多窗口(PWA)通过 `onAuxiliaryWindowState` 的 `pwa` 角色另开窗口。 |
| 手机上是单进程模式 | 手机上网页和浏览器在同一个进程里(`--single-process`)。平板和 2in1 是多进程。 |
| 远程调试端口始终打开 | 非 headless 模式下,引擎监听 `127.0.0.1:9222`。**发布正式版之前必须关闭**,目前还没有开关。 |
| JS 没有 JIT | JS 只用解释器执行(`v8_jitless`)。JS 很重的网页会明显偏慢。 |
| SURFACE 模式下的旋转未验证 | 手机横竖屏、平板和折叠屏的展开折叠,还需要在真机上确认。 |
| 投屏、PWA 菜单的实现还在 `entry/` 里 | 新外壳需要的话,先从参考实现复制,之后会移进 HAR。 |

---

## 12. 版本兼容

- HAR 和引擎来自同一次构建,不存在单独升级引擎的情况。换 HAR 就是换引擎。
- `BrowserStateSnapshot`、`BrowserRuntimeEvent`、`AuxiliaryWindowState` 都带有 `version` 字段。从这份文档起约定:只新增字段时版本号不变;删除字段或修改字段含义时,版本号加 1。外壳应当容忍未知字段和未知事件,遇到时忽略即可,不要报错。
- 这份文档跟随仓库更新。接口有变化时,改动 `engine/Index.ets` 的同一个提交里要同步修改这份文档。
