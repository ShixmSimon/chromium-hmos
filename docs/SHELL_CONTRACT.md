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

**`EntryAbility` 必须声明后台音频。** 在外壳 `module.json5` 的这个 ability 上加 `"backgroundModes": ["audioPlayback"]`。网页播放音视频期间,引擎会申请"后台音频播放"长时任务;没有这项声明,申请会失败,应用一切到后台就会被系统静音,几秒后被冻结,而控制中心还显示"播放中"。

**不要自己创建 AVSession。** 系统只允许一个 ability 有一个媒体会话,网页的媒体会话(控制中心卡片)由引擎创建。外壳如果启动时就建了一个会话,引擎就建不起来了。投屏选择器需要会话时,等用户打开选择器再建。

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
| `auxiliaryDismissWidget` / `auxiliaryDismissRevision` | number / number | 关闭一个窗口,见第 7 节。对 `windowRole: "browser"` 的独立浏览器窗口(无痕、拖出来的标签)同样有效:标签会按顺序关掉、写进最近关闭,窗口拆完之后回一个 `destroyed: true` 的窗口状态。第一个主窗口不在其列,外壳从来没被告知过它,也就关不掉它。 |
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
| `loadProgress` | 0–1,不在加载时为 1。地址栏进度条用它。 |
| `requestDesktopSite` | 当前标签是否在请求桌面版网站。 |
| `zoomPercent` | 当前标签的网页缩放,100 为正常。 |
| `inReaderMode`、`readerModeAvailable` | 是否在阅读模式;当前页能否进入。 |
| 标签的 `id`、`audible`、`muted` | 稳定 id;正在发声;已静音。 |
| `isPwaWindow`、`pwaAppId`、`pwaStartUrl` | 当前窗口是否是 PWA。 |
| `bookmarked` | 当前标签页的网址是否已加入书签。菜单里的书签开关用它。 |
| `version` | 协议版本。 |

---

## 4.5 谁画浏览器界面(`browserChrome`)

启动配置里的 `browserChrome` 决定 Chromium 画不画浏览器界面:

```
browserChrome?: 'shell' | 'native'
```

不传时跟随 `uiFamily`,也就是一直以来的行为:`mobile_phone` 为 `shell`,其余
为 `native`。**传了就固定不变**——展开折叠屏、2in1 切平板模式会改变
`uiFamily`,但不会把 Chromium 的标签栏画回来。平板和 PC 要的组合是"外壳画界
面、但用平板的 UA",这两件事原本绑在一起,所以要单独说。

**只藏框架**:

| 界面 | 落点 |
|---|---|
| 标签栏 | `BrowserView::ShouldDrawTabStrip()` |
| 工具栏 | `BrowserView::IsToolbarVisible()` |
| 地址栏 | `BrowserView::IsLocationBarVisible()` |
| 工具栏子控件、书签栏 | `BrowserView` / `ToolbarView::OnOhosUiFamilyChanged()`,会记住隐藏了什么,切回时恢复 |

**不受影响**(仍按 `uiFamily`):UA、滚动条样式、触控 / 指针 UI、viewport 规
则、状态里的 `mobileUi` 字段。

**其余界面 Chromium 照画**,外壳接好一项再用 `shellSurfaces` 关掉那一项:

```
shellSurfaces?: string[]
```

| 值 | 关掉的 Chromium 界面 |
|---|---|
| `contextMenu` | 右键 / 长按菜单,改发 `contextMenuRequested` |
| `downloadUi` | 下载气泡和下载栏,改用 `downloadUpdated` |
| `permissionPrompt` | 权限气泡,改发 `sitePermissionRequested` |

手机不传 `shellSurfaces` 时这三项默认就是外壳画(一直如此)。**目前只有这三
项**;其余(查找栏、页面信息、保存密码、翻译、缩放、PWA 安装、扩展工具栏)还
没有接管开关,Chromium 照画。要哪一项告诉内核,逐项加。

`setBrowserChrome { mode }` 可以运行中切换,标签不丢——`uiFamily` 变化仍然不
改判据,只有这条命令改。

**从原生界面切回来。** 换到 `native` 之后外壳的设置页就不画了,用户没有入口切
回去,而且这个选择会保存,重启仍然是原生界面。所以 Chromium 的应用菜单(⋮)里
有一项「使用 Lumie 界面」,在「设置」的上一行、同一组:

```
browserChromeRequested { windowId?: number, mode: 'shell' }
```

**这一项的文字由外壳给**,启动配置里加一个字段:

```
shellUiMenuLabel?: string     // 例:"使用 Lumie 界面"
```

不传、或者传空串,这一项就不出现——**"要不要这一项"和"叫什么"是同一个开关**。
手机默认就是原生界面、没有这个设置可回,外壳不传即可;外壳自己画界面时也不传。
无痕窗口的菜单里一样有。启动时读一次,运行中切换界面样式不用再通知内核:外壳每
次启动传同一个值,原生界面本来也是重启后才生效。

文案归外壳的原因:产品名和翻译都是外壳的事。之前编在内核的资源里,中文系统上显
示的是英文——新加的字符串在翻译进语言包之前就是这样,而产品自己的名字也不该由
内核来翻译。

内核收到点击后**不自己切**,只关掉菜单并发这条事件。切换由外壳做:保存设置、
调 `setBrowserChrome { mode: 'shell' }`、恢复 2in1 的合并标题栏。原因是这个设
置由外壳保存、下次启动也由外壳写进 `browserChrome`,内核自己切会让两边在重启后
不一致。事件发给所有窗口,因为这是整个浏览器的设置,而菜单所在的窗口不一定是外
壳正在看的那个。

**气泡锚点。** 工具栏藏了以后 Chromium 的气泡没有按钮可锚。外壳在布局变化时报
告一整套:

```
setAnchorRects { anchors: { id, x, y, width, height }[] }
```

坐标是窗口自身的 vp。内核会减去网页组件在窗口里的位置(`WebWindow` 自己上报,
外壳不用管),所以网页区上方有标签栏之类的控件也不影响。整套替换:没报告的锚点视为外壳不再画它,对应气泡回落到网
页区右上角。

应用菜单、查找栏、状态气泡、标签悬停卡片、侧边栏**没有单独关闭**:手机上它们
不出现,是因为没有工具栏按钮能触发。平板上如果发现某个仍会弹出(例如通过快捷
键),告诉内核单独处理。侧边栏在 154 里已从 `BrowserView` 搬到
`chrome/browser/ui/side_panel/`,需要单独处理,尚未做。

### 快捷键

网页没有消费的按键,Chromium 解析成命令之后,属于外壳界面的那些改发事件:

```
pageScrollSettled { url, scrollX, scrollY, pageWidth, pageHeight }

滚动停下 300ms 后推一次,一次滚动只推一次。单位是 CSS 像素。

contextMenuActionsChanged { requestId, supportedActions }

菜单已经打开之后才可用的项。目前只有"复制指向所选文字的链接"会这样——Chromium 先把它加成灰的,等渲染器生成文字片段(一次往返)之后才点亮。收到就整份替换掉手里那份;`requestId` 和 `contextMenuRequested` 的是同一个。

openedInGroup { id }

从 Chromium 的长按菜单在组内打开了一页。当前页没有变,`id` 是新标签的 `id`。

shellAccelerator { action }
```

`action` 取值和对应按键见 HAR 的 `SHELL_ACCELERATOR_KEYS`——**只有那一份**,引
擎不发按键名,菜单右侧显示的快捷键直接读它。

`escape` 是特例:引擎发给外壳的同时,Chromium 自己也会停止加载(浏览器本来就该
这样)。

### 扩展程序

| 命令 | 参数 | 返回 |
|---|---|---|
| `getExtensionActions` | `requestId` | `extensionActions { requestId, items }` |
| `runExtensionAction` | `id` | 等同于点工具栏上的扩展图标;弹窗仍由 Chromium 画,锚到 `setAnchorRects` 的 `extensions` |
| `setExtensionPinned` | `id`, `pinned` | |
| `installExtensionFromFile` | `path` | 成功:`extensionActionsChanged`;失败:`extensionInstallFailed { path, reason, message }` |
| `setExtensionEnabled` | `id`, `enabled` | 成功:`extensionActionsChanged` |
| `uninstallExtension` | `id`, `confirm?` | `confirm` 默认 `true`:弹 Chromium 自己的确认框,确认后 `extensionActionsChanged`,取消什么都不发。**手机上请传 `confirm: false`**:Chromium 的确认框画在网页区域里,外壳新标签页这类非网页界面上看不到;外壳自己弹确认框,用户确认后再发这条,内核直接卸载,成功后照常 `extensionActionsChanged`,被策略禁止时 `extensionCommandFailed { reason: 'notAllowed' }` |
| `getExtensionDetails` | `requestId`, `id` | `extensionDetails { requestId, id, permissions, siteAccess, siteAccessChangeable }` |
| `setExtensionSiteAccess` | `id`, `mode` | `mode`:`onClick` / `specificSites` / `allSites`;成功:`extensionActionsChanged` |
| `openExtensionOptions` | `id` | 在新标签页打开扩展的设置页 |

**扩展程序页。** 手机上没有能用的 `chrome://extensions`,外壳自己画一页,平板/PC
也可以用同一套接口:

- `extensionActions` 的每一项除了原有字段,还有 `userEnabled`(用户是否启用了这个扩
  展,也就是页面上的开关;原有的 `enabled` 表示在当前网页上可不可用,两者不同)、
  `hasPopup`、`optionsUrl`(没有设置页时为空串)、`version`、`description`、
  `author`,以及 `disableReasons`(停用的扩展为什么停用,Chromium 自己记的原因,
  可能有几个;`userAction` 是用户关的,`notVerified`、`corrupted`、
  `permissionsIncrease` 等是 Chromium 关的;启用时为空数组)。
- 用户停用的扩展也在列表里,排在启用的后面,`enabled` 和 `userEnabled` 都是
  `false`。
- `permissions` 是 Chromium 已经本地化好的权限说明,和 `chrome://extensions` 详情页
  上的一致;某条下面有细项(网站、设备)时,细项放在同一个字符串的后续行里。能改网站
  访问权限时,网站相关的那几条不在 `permissions` 里,由 `siteAccess` 表示。
  `siteAccessChangeable` 为 `false` 时(扩展不访问任何网站,或者由策略决定),不要让
  用户改。
- 命令做不成时发 `extensionCommandFailed { command, id, reason, message }`。
  `reason`:`notFound`、`notAllowed`(策略不允许)、`stillDisabled`(打开了开关,但
  扩展因为别的原因仍是停用的,比如新版本要了新权限)、`badMode`、`failed`。
  `message` 是 Chromium 给的那句话,可能为空。用户在确认框里取消卸载不算失败,不发。
- 停用、启用、卸载都会触发 `extensionActionsChanged`,包括停用状态下的扩展。

**手机上的扩展面板。** `runExtensionAction` 打开的面板在手机上从窗口底边往上弹,宽度
不超过屏幕(两边各留 8vp),高度不超过屏幕的 80%。

**产品名。** 启动配置 `productName`(比如 `'Lumie'`)会替换 Chromium 在少数地方对
用户说出的 "Chromium",目前是装完扩展后的那个"已添加到 Chromium"气泡。

**安装下载下来的 .crx。** 第三方扩展网站的"安装"是 .crx 下载链接,Chromium 按上游
规则不从这类网站直接安装,只会把文件下载下来。外壳在 .crx 下载完成时提示用户,用户
点"安装"后发 `installExtensionFromFile { path }`,`path` 就是 `downloadUpdated` 里的
`filePath`。内核走的是和把文件拖进 `chrome://extensions` 相同的安装流程,会弹
Chromium 自己的安装确认框,用户确认后才装。

`reason` 取值:`notFound`(文件不存在或读不了)、`notCrx`(不是 .crx 文件)、
`invalid`(文件损坏,或者不是有效的扩展)、`blocked`(被策略或黑名单禁止)、
`unsupported`(设备不满足扩展的要求)、`newerInstalled`(已经装了更新的版本)、
`cancelled`(用户在确认框里取消了)、`other`。`message` 是 Chromium 自己给出的那句
说明,已经是用户的语言,可以直接显示。`cancelled` 一般不用提示。

### 其它宽屏命令

| 命令 | 参数 | 说明 |
|---|---|---|
| `moveTab` | `id`, `toIndex` | 按 id 重排标签 |
| `activateTabById` | `id` | 按 id 切换标签 |
| `handleBack` | `requestId` | `backHandled { requestId, handled, by? }`。系统返回手势**先发这条,不要直接发 `back`**。网页处于全屏(见 `pageFullscreenChanged`)时内核先退出全屏,`by` 为 `fullscreen`;否则内核先关浏览器自己打开的气泡或对话框(装完扩展的"已添加"气泡、权限询问等,手机上没有别的办法关掉它们),`by` 为 `browserDialog`;没有的话再问网页,页面开着 `<dialog>`、全屏、或自己注册了 CloseWatcher 时 `by` 为 `page`。`handled` 为 true 时外壳就不要再后退 |
| `insertText` | `text` | 把文字插到当前输入位置。Chromium 读不了系统剪贴板(要 `READ_PASTEBOARD` 受限权限),外壳用系统粘贴安全控件读出来后发这条 |
| `groupTabs` | `ids`, `openerIds?` | 把这些标签编成一个组。`openerIds` 与 `ids` 等长、`''` 表示没有,用来在重启后把"谁打开了谁"一起交回来(内核按 session id 记 opener,重开的标签是全新的,自己推不出来);长度对不上就整个忽略。自己保存网址列表、启动后逐个 `newTab` 重开的外壳用这个把组重新建起来。少于两个不建组;已经在别的组里的会退出来加入新组 |
| `getPageContinuation` | `requestId` | `pageContinuation { requestId, url, title, scrollX, scrollY, pageWidth, pageHeight }`。当前窗口的当前标签 |
| `closeTabById` | `id`, `returnToOpener?` | 按 id 关闭标签。`returnToOpener: true` 时关掉后切回打开它的那个标签(它还在的话);不传时用 Chromium 自己的规则挑下一个 |
| `passwordAuthReset` | `reason?` | 让上一次身份验证立即失效。外壳在进入后台、以及在普通/无痕窗口之间切换时发。锁屏由内核自己监听,不用发 |
| `moveTabToNewWindow` | `id` | 把标签拖出成为独立窗口。新窗口和无痕窗口同一形状:`windowRole: "browser"` 事件 + `aura_win_<widget>` 表面。同 Profile,不受单进程限制 |
| `setTabMuted` | `id?`, `muted` | 不传 `id` 时作用于当前标签 |
| `setZoom` | `percent?` | 25–500,不传表示回到 100 |
| `toggleReaderMode` | | 进入 / 退出阅读模式 |
| `getTabThumbnails` | `requestId`, `ids`, `widthVp` | `tabThumbnails { requestId, items }` |
| `getRecentlyClosed` | `requestId`, `maxCount` | `recentlyClosed { requestId, items }` |
| `restoreRecentlyClosed` | `id?` | 不传 `id` 时恢复最近一项 |
| `removeTopSite` | `url` | 加入 top sites 黑名单 |

缩略图只缓存最近 12 个标签(约 2MB 封顶):当前标签实时截取、不占额度,后台标
签用它最后一次显示时的快照,没有缓存的不返回,外壳画骨架。无痕标签的快照只在内
存里。

`linkHovered { url }` 在指针移到链接上时发,`url` 为空表示移开了。

---

## 5. 事件(`onRuntimeEvent`)

引擎需要调用系统能力时,会发出一个事件。标为**必须响应**的事件,网页那一侧会一直等待外壳的回复;漏掉的话,文件选择框和权限请求会永远没有结果。

| 事件 | 必须响应 | 处理方式 |
|---|---|---|
| `filePickerRequested` | **是** | `HarmonyFilePickerAdapter.show(context, event)`,拿到结果后发送 `filePickerResult` 命令。**出错或用户取消时也要发**,带上 `canceled: true`。来自网页 `<input type=file>` 时另带 `acceptTypes`(accept 原样,如 `image/*`、`.pdf`)和 `capture`(网页要求直接拍摄):全是图片/视频类型时可以开图库,`capture` 为 true 时可以直接开相机。 |
| `permissionsRequested` | **是** | `PermissionRequestAdapter.request(context, event)`,然后发送 `permissionResult` 命令。**出错时也要发**,把全部权限放进 `denied`。发完之后再上报一次 `systemPermissionState`(见下文)。 |
| `systemPrintReady` | 否 | `SystemPrintAdapter.printPdf(context, event.filePath)` |
| `shareRequested` | 否 | `HuaweiShareAdapter.sharePage(context, event.url, event.title)`。来自网页 `navigator.share()` 时 `fromPage` 为 true,另带 `text`,`url` 可能为空 |
| `externalUrlRequested` | 否 | 交给别的应用打开的链接:`url`(Android intent: 链接已转成对应 scheme)、`fallbackUrl`(没有应用接时打开的网页,可能为空)、`initiator`(发起网页的 origin;浏览器自己发起时为空)。询问用户后用系统打开 |
| `popupBlocked` | 否 | 弹窗被拦截:`pageUrl`、`origin`、`popupUrl`、`count`。显示提示;"显示"发 `showBlockedPopups`,"始终允许"发 `setSiteSetting { origin, type: 'popups', setting: 'allow' }` |
| `dateTimePickerRequested` | **是** | 仅手机。`inputType`(`date`/`datetime-local`/`month`/`time`/`week`)、`value`/`min`/`max`(HTML 值字符串,如 `2026-10-03`、`14:30`、`2026-W40`,未设为空)。用系统的日期/时间选择器选好后发 `dateTimePickerResult { requestId, value }`(`''` 表示清空),取消时发 `{ requestId, canceled: true }` |
| `selectPopupRequested` | **是** | 仅手机。`options`(`{ label, type: 'option'\|'group'\|'separator', enabled, checked }[]`)、`selectedIndex`、`multiple`。选好后发 `selectPopupResult { requestId, indices }`(下标对应 `options`),取消时发 `{ requestId, canceled: true }` |
| `contactsPickerRequested` | **是** | 网页 `navigator.contacts.select()`:`multiple`、`properties`(`name`/`email`/`tel`/`address`/`icon` 中网页要的)。用系统联系人选择器选好后发 `contactsPickerResult { requestId, contacts: { name?, email?, tel?, address? }[] }`(每项都是字符串数组,地址一行一条),取消时发 `{ requestId, canceled: true }` |
| `speechRecognitionRequested` | **是** | 网页语音识别 `SpeechRecognition.start()`:`language`、`continuous`、`interimResults`。用系统语音识别(自己录音)识别,过程用 `speechRecognitionEvent { requestId, speechEvent }` 报告:`audioStart`/`soundStart`/`soundEnd`/`audioEnd`、`result`(`transcript`、`isFinal`、`confidence`)、`error`(`error` 为 Web Speech 的错误名,如 `no-speech`、`not-allowed`),最后一定发 `end` |
| `nfcScanStart`、`nfcScanStop` | **是** | 网页 NDEFReader 开始 / 停止读标签(`requestId` 是这个网页,`origin` 用于询问用户)。先回 `nfcScanStarted { requestId }`(用户拒绝或 NFC 关着时带 `errorType`,网页的 scan() 随之失败),读到标签发 `nfcTagRead { requestId, serialNumber, ndefRecords }`,出错发 `nfcError { requestId, errorType, message }`。记录格式见 HAR 的 `WebNdefRecord` |
| `nfcWrite`、`nfcMakeReadOnly`、`nfcWriteCancel` | **是** | 写入 `ndefRecords`(`overwrite`)/ 把标签设为只读,等用户把手机靠近标签;完成发 `nfcWriteResult { requestId, nfcOperation: 'push' \| 'makeReadOnly', errorType?, message? }`;`nfcWriteCancel` 时放弃等待 |
| `speechRecognitionStop`、`speechRecognitionAbort` | 否 | 停止录音并给出结果 / 直接放弃;两者最后都要发 `end` |
| `dateTimePickerClosed`、`selectPopupClosed` | 否 | 网页收回了 `requestId` 那次请求(输入框失焦或被移除),直接关掉对应的选择器,不用回复 |
| `fileOpenRequested` | 否 | `path` 加 `action`:`open` 打开文件,`openFolder` 打开文件夹,`reveal` 在文件管理里定位这个文件 |
| `systemActionRequested` | 否 | `SystemIntegrationAdapter.handle(context, event.action)` |
| `castRequested` | 否 | 投屏选择器。参考实现在 `entry/.../CurrentTabCastSession.ets`,还没有移进 HAR。 |
| `pwaInstalled`、`pwaMenuModel`、`pwaMenuClosed` | 否 | PWA 相关,需要时参考 `entry/` 的实现。 |
| `systemPrintFailed`、`inputRecovered` | 否 | 记日志即可。 |
| `findResult` | 否 | `matches` 匹配总数，`activeMatch` 当前是第几个（从 1 开始，0 表示没有），`finalUpdate` 为 true 时计数已定 |
| `pageText` | 否 | 回复 `getPageText`：`requestId`、`text` |
| `contextMenuRequested` | **是**(手机) | 长按网页元素。引擎这时不弹自己的菜单，由外壳画，见下文"长按菜单"。**每次都要回一条命令**：选了某项发 `contextMenuAction`，没选就关掉发 `contextMenuDismissed`。 |
| `browserControlsRatio` | 否 | 用了 `setBrowserControls` 才会有。`ratio` 为顶栏当前露出的比例（1 全部显示，0 完全滑走），外壳把顶栏往上移 `(1 - ratio) × 顶栏高度`；传了 `bottom` 时，底栏按归一化的比例 `n = (ratio - minTop/top) / (1 - minTop/top)`（`minTop` 为 0 时 `n = ratio`）往下移 `(1 - n) × bottom`：两栏同时开始、同时收完，底栏完全收起。传了 `minTop` 时，比例的下限是 `minTop / top`（顶栏收起到 `minTop` 就不会再往上滑了）。 |

### 长按菜单

手机上长按网页元素时，引擎仍由 Chromium 决定菜单里有什么、哪些项可用，但不再弹 Chromium 的桌面菜单，而是发 `contextMenuRequested`，由外壳画（例如底部菜单）。参考实现见 `entry/.../ContextMenuSheet.ets`。

| 字段 | 说明 |
|---|---|
| `requestId` | 之后的命令用它对应这次长按。新的长按会替换上一次，旧 `requestId` 的命令会被忽略 |
| `x`、`y` | 长按位置，窗口坐标，单位 vp |
| `linkUrl`、`linkText` | 长按的是链接时有值，否则为空字符串 |
| `srcUrl`、`mediaType` | `mediaType` 为 `'image' \| 'video' \| 'audio' \| 'none'` |
| `mediaFlags` | 视频和音频的状态：`paused`、`muted`、`loop`、`canLoop`、`controls`、`canToggleControls`、`canSave`、`hasAudio`、`inError` |
| `selectionText` | 有选中文字时有值 |
| `isEditable` | 是否是输入框 |
| `frameUrl`、`pageUrl`、`incognito` | 所在框架、页面，是否无痕 |
| `supportedActions` | 这次能做的动作，已按 Chromium 的判断过滤（例如没有可粘贴的内容时没有 `paste`）。**外壳只显示列表里的项**，顺序可以自己排 |

动作（`ContextMenuAction`）：

- 链接：`openLinkInNewTab`、`copyLinkAddress`、`copyLinkText`、`saveLinkAs`
- 图片：`openImageInNewTab`、`saveImageAs`、`copyImage`、`copyImageAddress`
- 视频和音频：`toggleLoop`、`toggleControls`、`openMediaInNewTab`、`saveMediaAs`、`copyMediaAddress`、`copyVideoFrame`
- 文字：`copy`、`cut`、`paste`、`selectAll`、`searchSelection`

`toggleLoop`、`toggleControls` 的文案按 `mediaFlags.loop`、`mediaFlags.controls` 显示成"取消循环""隐藏控件"。检查、投放、画中画、以图搜图不提供。

谁来画由启动配置的 `contextMenu` 决定：`'shell'` 外壳画，`'native'` 用 Chromium 自己的菜单。不填时手机为 `'shell'`，平板、2in1 等为 `'native'`。

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
| `setBrowserControls` | `top`（vp）, `minTop?`（vp）, `bottom?`（vp） | `bottom` 是随滚动一起收起的底栏高度（手机的 dock，连同它上面延伸出的小工具栏），不传或 0 表示没有；它不算在 `setViewportInsets` 里。外壳顶栏的高度，交给 Chromium 当作 browser controls：网页顶部给它留出位置，网页下滑时顶栏被滑走，上滑时再出现。引擎按当前设备的缩放比例把 vp 换算成物理像素后再交给 Chromium。`minTop` 是顶栏收起后仍保留的高度（vp），不传或传 0 表示可以完全滑走；`minTop` 会被夹到 `[0, top]` 之间。显示比例通过 `browserControlsRatio` 事件告诉外壳，外壳据此移动顶栏；`top` 传 0 取消 |
| `setBrowserControlsState` | `state`: `'shown' \| 'hidden' \| 'both'`, `animate?` | 主动把顶栏收起或展开，而不是等网页滚动触发。`'shown'` 强制展开、`'hidden'` 强制收起（收到 `minTop`）、`'both'`（默认）交还给滚动控制。`animate` 默认 true |
| `findInPage` | `text`, `forward?` | 在当前标签页查找，同一段文字再发一次即跳到下一个（`forward: false` 为上一个）。结果通过 `findResult` 事件返回 |
| `stopFind` | | 结束查找，清除高亮 |
| `getPageText` | `requestId` | 读取当前网页的可见文字（最多 20000 字），在独立的脚本环境里执行，网页自己的脚本看不到。结果通过 `pageText` 事件返回 |
| `contextMenuAction` | `requestId`, `action` | 回复 `contextMenuRequested`：执行选中的项。`action` 必须是事件 `supportedActions` 里的一项 |
| `contextMenuDismissed` | `requestId` | 回复 `contextMenuRequested`：用户没选任何项就关掉了菜单 |
| `setViewportInsets` | `bottom`（vp） | 外壳在网页底部盖了多高、且不随滚动收起的东西。dock 随滚动收起的手机上，这里只传系统手势条的高度。网页照常画到底，但可视区域缩小这么多，网页末尾能滚到悬浮栏上方。切换标签、新建标签后引擎会自动沿用，不用重发；传 0 取消 |
| `pwaHome`、`pwaMenu`、`pwaMenuAction`、`pwaMenuDismiss` | 见 `BrowserCommandPayload` | PWA 窗口菜单 |
| `defaultBrowserState`、`systemCapabilities` | 见参考实现 | 系统集成相关状态 |

后退、前进、刷新这类命令,以 `aura_shell_runtime_bridge.cc` 里 `ExecuteAuraShellBrowserCommand` 实际接受的为准。这张表是按 `286a844` 整理的。

### 手机：滚动时收起地址栏和 dock

页面不再避让 dock。页面刚加载时两栏都在，页面上边在地址栏下方、下边在 dock 上方；
网页往上滑(看下面的内容)时两栏跟手收起、页面同步拉伸，直到完全收起；往下滑时两栏
跟手出现并常驻。两栏同时开始、同时收完。平板和 2in1 不变。

外壳要做的:

1. `WebWindow` 铺满整个窗口(包括 dock 和系统手势条下面)。
2. `setBrowserControls { top: 地址栏高度, minTop: 0, bottom: dock 高度 }`。`bottom`
   包括 dock 上方延伸出的小工具栏,它和 dock 一起收起。
3. `setViewportInsets { bottom: 系统手势条高度 }`。这一段不收起,也不给网页布局:内容
   最多滚到手势条上方,最后一行不会被挡;手势条下面露出的是网页底色。不要再把 dock 算
   进来。
4. 收到 `browserControlsRatio { ratio }`:地址栏 `translateY = -(1 - ratio) × top`;
   dock 用归一化的 `n = (ratio - minTop/top) / (1 - minTop/top)`,
   `translateY = (1 - n) × bottom`(`minTop` 为 0 时 `n` 就是 `ratio`)。外壳自己不判断滚动方向,收起和出现都听
   这个比例。
5. 地址栏获得焦点、页面输入框弹出键盘时,发 `setBrowserControlsState { state:
   'shown' }` 把两栏钉住;失焦或键盘收起后发 `state: 'both'` 交还给滚动。

内核自动处理:切换标签、打开新页面(包括前进后退)时两栏重新显示;页面短到不能滚动时
两栏一直显示;滚到页面最底部时不会自动显示(和 Chrome 安卓一致)。

### Profile 数据（书签、历史、下载、设置）

书签、历史、下载、偏好设置都存在 Chromium 的 Profile 里。外壳想自己画这几个页面时，不要打开 `chrome://bookmarks` 这类页面，改用下面的命令查询和修改。结果通过事件返回。

**共同约定**

- 查询命令带 `requestId`，返回的事件里带同一个 `requestId`。
- 数据在别处变了（其它标签页加了书签、下载进度变化、历史被清除），引擎会主动推一条 `…Changed` 事件（下载是 `downloadUpdated`），外壳收到后刷新正在显示的页面。
- 每个事件都带 `incognito`。命令作用于发命令的窗口所在的 Profile。
- 时间一律是毫秒时间戳（UTC），未知时为 -1。字节数未知时为 -1。Chromium 的 64 位 id 以字符串传递。
- 事件的字段类型在 HAR 的 `ShellServiceTypes.ets` 里，每种事件一个接口，用 `shellEvent<HistoryResultsEvent>(event)` 这种方式读取。

**书签**

`aboutInfo` 里的 `bookmarkApiVersion` 说明这份引擎支持到哪一版：`1` 是最初
的一组命令，`2` 加上插入位置、操作结果、`childCount`/`rootType`、批量命令和两个
查询，`3` 加上导入导出。外壳启动时读一次，不必逐条用超时去探测。

| 命令 | 参数 | 返回 |
|---|---|---|
| `getBookmarks` | `requestId`, `parentId?` | `bookmarkList { requestId, parentId, nodes }`。不传 `parentId` 时返回三个根文件夹：移动设备书签、书签栏、其他书签 |
| `searchBookmarks` | `requestId`, `query`, `maxCount` | `bookmarkList`，`parentId` 为空。按标题和网址匹配，每个节点另带 `path` |
| `getBookmarksForUrl` | `requestId`, `url` | `bookmarkList`，网址完全相同的全部书签，按 `dateAdded` 倒序 |
| `getRecentBookmarks` | `requestId`, `maxCount` | `recentBookmarks { requestId, nodes }`,按添加时间倒序,只含网址书签 |
| `getBookmarkPath` | `requestId`, `id` | `bookmarkPath { requestId, nodes }`，从根文件夹到 `id` 自身，含两端；`id` 不存在时为空 |
| `addBookmark` | `url`, `title`, `parentId?`, `index?`, `requestId?` | 默认加到"移动设备书签"、末尾。带 `requestId` 时回 `bookmarkCreated { requestId, node }` |
| `createBookmarkFolder` | `requestId`, `parentId`, `title`, `index?` | `bookmarkCreated { requestId, node }` |
| `removeBookmarkByUrl` | `url`, `requestId?` | 删除这个网址的所有书签 |
| `updateBookmark` | `id`, `title?`, `url?`, `requestId?` | |
| `moveBookmark` | `id`, `parentId`, `index`, `requestId?` | |
| `removeBookmark` | `id`, `requestId?` | 文件夹连同里面的内容一起删 |
| `moveBookmarks` | `ids`, `parentId`, `index`, `requestId?` | 按数组顺序连续放到 `index` 起的位置 |
| `removeBookmarks` | `ids`, `requestId?` | |
| `exportBookmarks` | `requestId`, `path` | `bookmarkExportDone { requestId, ok, bookmarkCount, folderCount, error? }` |
| `importBookmarks` | `requestId`, `path`, `parentId?`, `title?` | `bookmarkImportDone { requestId, ok, folderId, bookmarkCount, folderCount, skippedCount, error? }` |

```
BookmarkNode {
  id, parentId, type: 'url' | 'folder', title, url?, dateAdded, index,
  childCount?,   // 文件夹的直接子项数量，不递归
  rootType?,     // 'mobile' | 'bookmarkBar' | 'other'，仅三个根文件夹
  path?          // 仅 searchBookmarks：从根文件夹到父文件夹的标题
}
```

任何变化后推送 `bookmarksChanged { revision }`。

`index` 省略时是末尾，超出范围按末尾、小于 0 按 0，不算失败。

**修改类命令的结果。** 上表里带 `requestId?` 的命令，传了 `requestId` 就会在执行
后回一条：

```
bookmarkOpResult { requestId, ok, error?, failedIds? }
```

不传 `requestId` 就不回，旧外壳的行为不变。成功时照常推 `bookmarksChanged`，
失败时不推。

| `error` | 含义 |
|---|---|
| `notFound` | `id` 不存在 |
| `permanentNode` | 试图修改、移动或删除三个根文件夹 |
| `invalidParent` | 目标 `parentId` 不存在或不是文件夹 |
| `cycle` | 试图把文件夹移到它自己或它的子孙里 |
| `invalidUrl` | `url` 不是合法网址，或对文件夹设网址 |
| `unknown` | 其它失败，包括被策略管理的书签 |

`updateBookmark` 先校验后写：`url` 会被拒时，`title` 也不会改。空 `title` 是允许的。

批量命令（`moveBookmarks` / `removeBookmarks`）整批在一次 `BeginExtensiveChanges`
里完成，**只推一次 `bookmarksChanged`**。同一批里同时包含某个文件夹和它的子孙
时，只处理这个文件夹。部分失败时跳过失败项、继续处理其余项，回 `ok: false`、
`error` 取第一个失败原因，并带 `failedIds`。

**无痕窗口。** 无痕 Profile 没有自己的书签：命令和事件都作用在原始 Profile 上，
和 Chrome 一致。`bookmarksChanged` 会同时推给普通窗口和无痕窗口。

**导入导出（v3）。** 格式是通用的书签 HTML（Netscape Bookmark File Format），
Chrome、Edge、Firefox、Safari 导出的都是它。

`path` 是**应用沙箱里的绝对路径**。系统文件选择器、沙箱内外的复制、界面提示都归
外壳；引擎只读写这一个路径，不接触文件 URI，也不需要存储权限——两边在同一个进程
里。

导出写全部书签，三个根文件夹的写法与 Chrome 一致（书签栏带
`PERSONAL_TOOLBAR_FOLDER="true"`），带 `ADD_DATE` 和 `ICON`，UTF-8。写在后台线程，
写完才回复。计数不含三个根文件夹。

导入**建一个新文件夹装进去，不合并、不去重**，放在 `parentId`（省略时"移动设备
书签"）的最前面，文件夹名用 `title`（省略时"Imported bookmarks"）。文件里原来的
层级原样保留。整批在一次 `BeginExtensiveChanges` 里完成，**只推一次
`bookmarksChanged`**；解析在后台线程，写模型回 UI 线程。`folderId` 是新文件夹的
id。无效网址（Firefox 的 `place:` 智能书签等）计入 `skippedCount`，不算失败。
文件里的搜索引擎不导入。

**导入不带图标。** 解码图标要 `content::DecodeImage`，那是子进程设施（用 blink
解码），而这里在浏览器进程解析——手机根本没有子进程。书签正常导入，图标由
Chromium 首次访问时重新抓。这是一直如此，不是偶尔。

| `error` | 命令 | 含义 |
|---|---|---|
| `writeFailed` | 导出 | 路径写不了：目录不存在、没权限、磁盘满 |
| `fileNotFound` | 导入 | `path` 不存在或读不了 |
| `notBookmarkFile` | 导入 | 能读，但解析不出任何书签 |
| `tooLarge` | 导入 | 超过 20 MB |
| `invalidParent` | 导入 | `parentId` 不存在或不是文件夹 |
| `unknown` | 两者 | 其它失败 |

失败时不留半截数据，也不推 `bookmarksChanged`。

**历史**

| 命令 | 参数 | 返回 |
|---|---|---|
| `queryHistory` | `requestId`, `text`（空为全部）, `beforeTime?`（分页游标）, `maxCount` | `historyResults { requestId, items, reachedEnd }`，按时间倒序，每个网址每天一条。翻页时把最后一条的 `visitTime` 作为下一次的 `beforeTime` |
| `removeHistoryItems` | `items: { url, visitTime }[]` | 删除这个网址在 `visitTime` 所在那一天的全部访问。`historyResults` 本来就是每个网址每天一条，和 chrome://history 的做法一致 |
| `removeHistoryForUrl` | `url` | 删除这个网址的全部访问记录 |
| `clearHistory` | `beginTime?`, `endTime?` | 不传为全部时间 |
| `autocomplete` | `requestId`, `text` | `autocompleteResults { requestId, items, done }`。同一个 `requestId` 可能收到多次，直到 `done` 为 true |
| `getTopSites` | `requestId`, `count` | `topSites { requestId, items: { url, title }[] }` |

`HistoryItem { url, title, visitTime, visitCount }`。有变化时推送 `historyChanged { revision }`，最多每秒一次。

**下载**

下载开始、进度变化（每个下载约 500ms 最多一次）、状态变化、被删除时，引擎推送 `downloadUpdated`，字段见 `DownloadItem`：`id`、`url`、`fileName`、`filePath`、`mimeType`、`receivedBytes`、`totalBytes`、`state`（`'inProgress' | 'paused' | 'completed' | 'cancelled' | 'failed'`）、`failReason`、`canResume`、`dangerous`、`dangerType`、`startTime`、`endTime`，被删除时另带 `removed: true`。

**危险文件**：`dangerous` 为 true 的下载会停在 `inProgress`，等用户决定。外壳必须给出"仍然保留"和"丢弃"两个选项，分别发 `keepDangerous` 和 `discardDangerous`，否则这个下载会一直停在那里。

| 命令 | 参数 | 返回 |
|---|---|---|
| `listDownloads` | `requestId` | `downloadList { requestId, items }`，最新的在前 |
| `setDownloadDirectory` | `uri` | 设定下载目录，见下文"保存位置" |
| `downloadAction` | `id`, `action`: `'pause' \| 'resume' \| 'cancel' \| 'retry' \| 'remove' \| 'removeAndDeleteFile' \| 'keepDangerous' \| 'discardDangerous'` | `remove` 只删记录，`removeAndDeleteFile` 连文件一起删 |

- **保存位置**：
  - 外壳页面出现后调用 HAR 的 `ShellDownloadDirectory.prepare(context)`。它用 `DocumentViewPicker` 的下载模式取到 `Download/<包名>` 目录，不弹界面，授权长期有效。
  - 再把拿到的 URI 用 `setDownloadDirectory { uri }` 命令发给引擎。引擎把它设为 Chromium 的默认下载目录，并关掉"下载前询问保存位置"。
  - **不能在页面出现之前调用**：在 `onWindowStageCreate` 里调用，选择器会报 13900042 错误。所以这个目录没法通过启动配置传，启动配置里的 `downloadDirectoryUri` 只适用于外壳已经保存了上次拿到的 URI 的情况。
  - 引擎收到目录之前开始的下载，仍会弹出系统的"选择保存位置"面板。
  - `filePath` 是真实路径，外壳用 `fileUri.getUriFromPath` 转成 URI 后再打开或分享。
- **下载提示由谁显示**：启动配置 `downloadUi: 'shell' | 'native'`。手机默认为 `'shell'`，此时 Chromium 不显示下载气泡和下载栏，由外壳根据 `downloadUpdated` 自己提示。
- 长按菜单里的各种"另存为"也走这套下载流程。手机上（`downloadUi` 为 `'shell'` 时）"另存为"不弹保存面板，直接存进下载目录，同名文件会自动加序号。原因是手机应用只能按路径写自己的 `Download/<包名>` 目录，保存面板选中的文件只能通过 URI 访问，Chromium 按路径写会报 `FILE_ACCESS_DENIED`。
- **通知栏**：HAR 提供 `DownloadNotifier`。外壳把每个 `downloadUpdated` 事件都交给 `notifier.update(shellEvent<DownloadUpdatedEvent>(event))`，它会在通知栏显示下载状态：下载中用系统的进度条模板（`downloadTemplate`，最多每秒刷新一次）；完成后变成"下载完成"，点击就用系统里对应的应用打开文件；失败时显示"下载失败"；需要用户确认的危险文件会提示去下载页处理；取消或删除后通知消失。外壳还要在 `EntryAbility` 的 `onCreate` 和 `onNewWant` 里调用 `handleDownloadNotificationWant(context, want)`，因为点击通知拉起的只有外壳的 ability 能收到。参考实现见 `entry/` 的 `AuraShell.ets` 和 `EntryAbility.ets`。
- 下载开始、确定目标路径、失败时，hilog 里各有一行 `OHOS download started` / `target` / `failed`，失败那一行带原因。

**设置**

| 命令 | 参数 | 返回 |
|---|---|---|
| `clearBrowsingData` | `requestId`, `types`, `timeRange` | `clearBrowsingDataDone { requestId, ok }`，`ok` 为 false 表示请求无效或有数据没清掉。已安装网页应用的数据不清。`passwords` 只清 Chromium 自己存的密码，不影响系统密码保险箱 |
| `getBrowsingDataCounts` | `requestId`, `timeRange` | `browsingDataCounts { requestId, historyCount, cacheBytes, siteCount }`。`historyCount` 按每个网址每天计一条；10 秒内算不出的项为 -1 |
| `getSearchEngines` | `requestId` | `searchEngines { requestId, items: { id, name, keyword, url, searchUrl, isDefault }[] }`，只列出能设为默认的搜索引擎。**用 `searchUrl`**:`url` 是 Chromium 的原始模板,带 `{google:baseURL}` 这类外壳解析不了的占位符;`searchUrl` 已全部解析,只留 `{searchTerms}` 一处待替换 |
| `setDefaultSearchEngine` | `id` | 策略或扩展控制默认搜索引擎时不生效 |
| `getPrefs` | `requestId`, `keys?` | `prefs { requestId, values: { key, value }[] }`。不传 `keys` 返回全部；本版本不支持的键不返回 |
| `setPref` | `key`, `value` | 写入成功后推送 `prefsChanged { keys }`。被策略锁定的项写不进去 |
| `getSiteSettings` | `requestId`, `type?` | `siteSettings { requestId, items: { origin, type, setting, embeddingOrigin? }[] }`，只列出用户单独设置过的网站。`origin` 是 Chromium 的匹配规则，例如 `https://a.com:443`、`[*.]a.com` |
| `getSiteSettingsForOrigin` | `requestId`, `origin` | 同上，列出这个网站每一类权限的当前值 |
| `setSiteSetting` | `origin`, `type`, `setting` | `setting` 为 `'allow' \| 'block' \| 'ask' \| 'default'`，`default` 表示删除这条单独设置。定位、摄像头、麦克风、通知、剪贴板只能对 https 网站设置；`storageAccess` 不能按单个网站设置 |
| `setDefaultSiteSetting` | `type`, `setting` | |
| `resetSiteSettings` | `origin` | 清除这个网站的全部权限和存储 |
| `installWebApp` | | 把当前网页安装成应用(Chromium 的安装对话框),装好后发 `pwaInstalled`。浏览器状态里 `canInstallWebApp` 为 true 时才在菜单里提供 |
| `getWebApps` | `requestId` | 回 `webApps { requestId, apps: { appId, title, startUrl, canUninstall }[] }`,已安装的网页应用,按名字排序 |
| `uninstallWebApp` | `appId` | 卸载用户装的网页应用,回 `webAppUninstalled { appId, ok }`;策略或系统装的(`canUninstall` 为 false)卸不掉 |
| `launchWebApp` | `appId` | 在独立窗口里打开已安装的网页应用(pwaInstalled 的 `appId`),给桌面卡片用 |
| `exportWebAppIcon` | `requestId`, `appId`, `directory` | 把网页应用的图标写成 `<directory>/<appId>.png`(192px 或更大的最小一张),回 `webAppIcon { requestId, appId, title, path }`,没有图标时 `path` 为空 |
| `restoreLastSession` | `restore`, `mainWindowOnly?` | 回应 `lastSessionRestorable`。`mainWindowOnly` 为 true 时只把上次主窗口的标签（带前进后退历史）加进当前窗口，回 `lastSessionRestored { count }`；否则恢复上次的所有窗口 |
| `showBlockedPopups` | | 打开当前窗口当前标签页被拦截的全部弹窗,回应 `popupBlocked` |
| `getAboutInfo` | `requestId` | `aboutInfo { requestId, chromiumVersion, engineCommit, userAgent, bookmarkApiVersion, browsingApiVersion, jitEnabled }`。`jitEnabled` 是 V8 这次启动实际有没有用上 JIT(启动日志里 `AuraShell JIT available` 那个判断的结果):启动配置要求 jitless、或进程拿不到可执行内存时为 `false` |

`types`：`history`、`cookies`、`cache`、`siteSettings`、`formData`、`passwords`、`downloads`。`timeRange`：`lastHour`、`lastDay`、`lastWeek`、`last4Weeks`、`all`。

偏好键（只接受这些）：`blockThirdPartyCookies`、`doNotTrack`、`safeBrowsing`（`'off' | 'standard' | 'enhanced'`）、`preloadPages`、`popupsBlocked`、`javascriptEnabled`、`textScale`、`autofillAddresses`、`autofillCards`、`downloadAskWhereToSave`、`passwordFillRequiresAuth`、`passwordVault`、`forceDarkWebContents`(网页强制深色,已打开的标签页立即生效)。

- `textScale` 是 50–200 的百分比。桌面版 Chromium 没有只放大文字的设置，所以这里改的是网页的默认缩放比例，整页一起放大。
- 这个版本没有配置 Google API 密钥，`safeBrowsing` 开关能保存，但实际上很可能不起作用，设置页不要承诺有安全浏览保护。
- `passwordFillRequiresAuth` 默认开。关掉只影响"把已保存的密码填进网页"这一件事；查看、复制、编辑、导出密码永远要验证身份，没有开关。设备上没有锁屏也没有录入生物特征时，这个开关不起作用——没有东西可以拿来验证。
- `passwordVault` 默认关。打开后由系统密码保险箱填充和保存网页密码：点登录框时内核直接调起保险箱（每个表单问一次），用户选的账号由内核填进网页；登录成功后保险箱询问是否保存，同时 Chromium 不弹提示、悄悄存一份，关掉开关后照常可用。打开期间 Chromium 自己的填充建议、自动填充和强密码提示都不出现。保险箱调用由外壳用 `registerShellSystemService('passwordvault', …)` 实现（`autoFillManager` 的 ViewData 接口要 API 26，引擎 HAR 按 API 20 编译）。保险箱要求设备设了锁屏密码，外壳只在可用时让开关可点。

`passwordvault` 系统服务（外壳实现，内核调用）：
- `fill {url, kind: 'login' | 'signup', focus: 'username' | 'password'}` → `{username, password}`（用户选中的账号）或 `{}`（没有账号、用户取消、应用进了后台）。用 `requestAutoFill`，`triggerType` 必须是 `AUTO_REQUEST`（`MANUAL_REQUEST` 会列出保险箱里全部账号），`pageUrl` 填 `url`（保险箱按它的域名匹配）。同一时间只能有一个请求。
- `save {url, username, password}` → `{}`。用 `requestAutoSave`，保险箱自己问用户。
- 密码只在内存里经过：不记日志、不落盘、不进界面状态。

**密码**

密码管理器是开的。存起来的密码用 HUKS 里的密钥加密，那把密钥出不了安全世界，所以 `Login Data` 文件被单独拷走也读不出来。

| 什么时候验证身份 | 能不能关 |
|---|---|
| 把密码填进网页 | 能，`passwordFillRequiresAuth` |
| 查看、复制、编辑、导出密码 | 不能 |

- 验证界面是鸿蒙自己的，`userauth` 服务在 HAR 里（`UserAuthService.ets`），外壳不用注册也不用实现。内核不自己画密码输入框。
- 验证成功后 60 秒内不再问。这 60 秒用单调时钟计时，改系统时间没用。
- 以下几件事会让这 60 秒立刻作废：锁屏或息屏（内核自己监听公共事件，外壳不用管）；应用进入后台；在普通窗口和无痕窗口之间切换。后两件外壳发 `passwordAuthReset` 告诉内核。
- 设备上什么都没录入(没有锁屏密码、人脸、指纹)时不弹验证,直接放行。内核是先问过外壳"确实没有"才放行的,外壳还没回答时不会当成"没有"。
- **升级会清空已存的密码。** 这个版本之前的密码是用每个 Chromium 构建都一样的固定密钥存的，等于没加密，所以第一次启动这个版本时会全部删掉，不迁移。换机、克隆、恢复备份或卸载重装之后解不开包裹密钥时也一样：密码库当作空的。这两种情况 hilog 里都有一行 `OHOS passwords: cleared stored credentials`。设置页可以提一句"因安全升级，此前保存的密码已清除"。
- 无痕窗口不保存密码，行为和之前一致。
- 平板 / PC 的 chrome://password-manager 里「显示 / 复制 / 编辑 / 导出」同样先弹这个验证(查看类 60 秒内不重复问,导出每次都问);设备上什么都没录入时直接放行。

**手机自绘密码页**

手机不用 chrome://password-manager,外壳自己画列表、详情、编辑,内核只给数据和操作。
外壳在「显示 / 复制 / 编辑 / 删除 / 导出」前先弹系统验证(见 `UserAuthService`),通过后发
`passwordAuthGranted`,再发对应命令。

| 命令 | 参数 | 回包 |
|---|---|---|
| `getSavedPasswords` | `requestId` | `savedPasswords { requestId, items }`。`items`:`{ id, origin, signonRealm, username, hasNote, dateCreated, dateLastUsed, isAndroidCredential }`,不含明文和备注内容;时间是毫秒时间戳,未知为 `-1` |
| `passwordAuthGranted` | `validMs` | 外壳的验证通过了。`validMs` 内(最多 60000)允许下面四条敏感命令。无回包 |
| `revealPassword` | `requestId`, `id` | `passwordRevealed { requestId, id, password, note }`;失败时 `passwordCommandResult { command: 'revealPassword', ok: false, reason }` |
| `updatePassword` | `requestId`, `id`, `username?`, `password?`, `note?` | `passwordCommandResult { requestId, command, ok, reason?, newId? }`。只传要改的字段。改了用户名时这条密码换了 id,新的在 `newId` |
| `deletePassword` | `requestId`, `id` | `passwordCommandResult` |
| `exportPasswords` | `requestId`, `path` | `passwordsExported { requestId, ok, count, reason? }`。导出 Chromium 格式的 CSV 到外壳沙箱里的 `path`(绝对路径),之后外壳用系统保存框让用户选位置 |
| `importPasswords` | `requestId`, `path`, `overwrite?` | `passwordsImported { requestId, ok, imported, skipped, failed, reason? }`。`path` 是外壳沙箱里的 CSV(外壳从用户选的文件复制过来),格式按 Chromium 的导入器,兼容 Chrome / Edge / Firefox 导出的 CSV。同网站同用户名、密码不同的条目默认跳过(计入 `skipped`);传 `overwrite: true` 用导入的覆盖。和库里完全一样的条目 Chromium 算作已导入,计入 `imported`。`failed` 是读不出来的行(缺网址、网址不对、缺密码、字段太长)。`reason`:`authRequired`、`badFormat`(表头缺失或不对)、`ioError`、`tooLarge`(超过 1000KB 或条数超上限)、`unknown`(含上一次导入还没结束)。导入有变化时照常 `savedPasswordsChanged`。内核只把文件读进内存,不留副本,缓存文件由外壳删 |
| `passwordAuthReset` | `reason?` | 已有,让验证立即失效 |

- `reason`:`authRequired`(验证过期或没验证)、`notFound`、`duplicate`(改成的用户名在这个网站已经有了)、`ioError`(导出写文件失败或路径不可用)、`unknown`。
- 验证窗口和原生页、填充共用:锁屏、息屏、`passwordAuthReset` 都会让它立即失效,之后五条敏感命令(显示、修改、删除、导出、导入)一律回 `authRequired`。
- `savedPasswordsChanged {}`:网页保存了新密码、同步、或其他窗口改了密码时广播,外壳重新拉列表。外壳至少发过一次 `getSavedPasswords` 之后才会收到。
- `id` 在同一次运行中稳定(按网站 + 用户名分配),改密码不换 id,改用户名换 id。
- 无痕窗口里发的这些命令读写的是普通 Profile 的密码,和 Chromium 一致。
- 内核不缓存交给外壳的明文,任何日志都不带明文、用户名、备注。联调时 hilog 里搜 `OHOS passwords:`,只有条数、id 和结果。

权限类型：`location`、`camera`、`microphone`、`notifications`、`javascript`、`popups`、`sound`、`clipboard`、`storageAccess`。没有 `autoplay`：桌面版 Chromium 实际上不执行这项设置，要控制声音请用 `sound`。

所有设置都读写普通 Profile，从无痕窗口发的命令也一样。

**标签组(手机)**

手机把标签藏在网格后面,网页打开的新标签页用户看不见,返回也回不到来处。所以
**在手机布局下**,一个页面打开另一个页面时,内核把两者归进同一个组;平板和 PC
跟桌面 Chromium 一样,平铺、不建组。组由内核保存,外壳只决定怎么画。

`aboutInfo` 里的 `browsingApiVersion` 说明支持到哪一版:`1` 是浏览 UI 那一轮
(权限、加载进度、标签 id、缩略图、最近关闭、桌面版、阅读模式),`2` 加上标签组。

每个标签多两个字段:

| 字段 | 说明 |
|---|---|
| `groupId` | 所在组,不在组里为 `''`。就是 Chromium 的 `TabGroupId`,同组标签在标签栏里总是挨着,顺序就是打开的顺序 |
| `openerId` | 打开它的那个标签的 `id`,没有、或那个标签已经关掉时为 `''`。跨重启保留 |

什么时候自动建组(只在 `uiFamily = mobile_phone`):

- 点 `target="_blank"` 链接,或用户手势触发、不带窗口特性的 `window.open`:新页
  插到来源标签所在组的末尾并切过去;来源标签还没有组就新建一个,**来源页排在第
  一个**。
- 带窗口特性的 `window.open`(弹窗)不进组,照旧走弹窗拦截。`opener` 关系原样
  保留,OAuth 和支付弹窗不受影响。
- 长按菜单里的"在新标签页中打开"同样进组,但**后台打开**,当前页不动,随后发
  `openedInGroup { id }`。
- 组里只剩一个标签时自动解散,那个标签的 `groupId` 变回 `''`。解散由内核在任何
  一次标签变动之后自己做,外壳不用管。
- 折叠屏运行中改变布局:已有的组和 opener 不动,之后新开的按当前布局的规则走。

`newTab` 加了两个参数:`groupId` 非空时新页插到这个组的**末尾**(不是当前页旁边)、
`openerId` 设为当前标签;`background: true` 时不切过去。

关闭标签(`closeTab` / `closeTabById`)会写进"最近关闭",`restoreRecentlyClosed`
能把它恢复回来 —— 组的撤销靠这个。

组成员变化不另发事件,照常在 `tabs` 里读。

**拖放**

拖进网页已经实现，外壳不用接任何东西：页面那块 XComponent 是引擎自己的，系统拖
放的进入、移动、离开、落下全由引擎收下来交给 Blink。外壳只在页面自己不要的时候
收到下面这两类事件。

拖进网页:

```
dropNotHandled {
  windowId?: number,
  urls: string[],
  text: string,
  files: [{ path, name, mimeType, size, viewPath? }][]
}
```

网页自己处理掉的落下**不会**发这条 —— 只有渲染器回了 operation none、或者页面没有
`preventDefault` 时才发,让外壳按产品规则接手。文件在发这条之前已经复制到
`<userDataDir>/dropped/<随机目录>/<原文件名>`，`path` 就是它。当初写的是
cacheDir，改成了引擎自己的数据目录 —— Chromium 在鸿蒙上取不到应用的 cacheDir，
而引擎必须能自己校验这个路径：交给渲染器的文件路径同时就是它获得读权限
的范围，只收自己放进去的东西。对外壳没区别。那个目录每次启动清空。

页面不接的落下，引擎**不会**再自己导航到那个文件地址了。Blink 默认会把没人要的
拖放当成「打开它」，一路导航过去 —— 而那同时算作接受了这次拖放，所以
`dropNotHandled` 反而不发。现在这条默认行为在鸿蒙上关掉了：没人要的落下就是没人
要，页面不动，事件照发，接不接、怎么接由外壳决定。

`viewPath` 只在 HEIC / HEIF 上出现（相册里拖出来的照片就是）：那是引擎用鸿蒙的
解码器转出来的 JPEG，因为 Chromium 任何构建都没有 HEIC 解码器。要把图片展示
出来就用 `viewPath`，有它用它、没它用 `path`。上传永远用 `path`：页面要的是那
张照片，不是我们重新编的版本。

从网页拖出：

```
pageDragStarted { windowId?: number, url: string, text: string, html: string,
                  filePath?: string }
pageDragEnded   { windowId?: number, dropped: boolean }
```

手机上长按不动照常出 `contextMenuRequested`；手指开始移动才发 `pageDragStarted`，外壳
收到就把已经弹出的长按菜单收掉。平板 / PC 用鼠标拖也发这两条。

拖出去的东西本身不用外壳管：引擎自己把链接、文字、HTML 放进 UDMF 并启动
系统拖放。`pageDragStarted` 里的 `url` / `text` / `html` 只是告诉外壳拖的是什么，
方便它做自己的事（收菜单、埋点）。

图片拖出也做了。图片按**网页加载时的原格式原样**写成临时文件（不重新编码），
文件名和「图片另存为」取的一样，落在 `<userDataDir>/dropped/` 下，和拖进来的文件
同一套规矩、启动时清空。UDMF 里 **Image（file-uri）在前、Hyperlink（图片地址）
在后** —— 顺序是有意的，拖进聊天窗口应该发图片而不是发链接。拖动预览就是
那张图。

`filePath` 就是那份临时文件，只在拖图片时出现；`url` 是图片的原地址。拖出去
的东西本身不用外壳管，这两个字段只是告诉你们拖的是什么。

**网页全屏**

```
pageFullscreenChanged { windowId?: number, fullscreen: boolean }
```

网页调 `requestFullscreen()`(视频全屏、游戏等)时发 `fullscreen: true`,退出时
发 `false`。内核只能让网页区域铺满,标签栏、工具栏、手机的浮动工具栏是外壳自己
的,要外壳收到后隐藏,并把窗口切到沉浸式全屏;`false` 时恢复。全屏期间网页区域
尺寸变化照常上报,内核按新尺寸布局。

退出全屏:系统返回走 `handleBack`,网页全屏时内核退出全屏并回
`backHandled { handled: true, by: 'fullscreen' }`,随后发 `pageFullscreenChanged
{ fullscreen: false }`。网页自己退出(Esc、页面按钮)也会发这条。

内核在 200ms 一次的状态轮询里读全屏状态,所以事件最多晚 200ms 到。

**下拉刷新(仅手机)**

```
pullToRefresh { windowId?: number, state: 'start', threshold: number }
pullToRefresh { windowId?: number, state: 'pull', distance: number }
pullToRefresh { windowId?: number, state: 'release', refresh: boolean }
pullToRefresh { windowId?: number, state: 'reset' }
```

用的是 Chromium 自己的下拉刷新(和安卓 Chrome 同一套判断):网页已经在最顶部、手指
继续往下拉才开始;网页内部的滚动容器会先把拉动吃掉,只有它也到顶了才轮到下拉刷新;
网页设置了 `overscroll-behavior-y: contain/none` 时不会触发。内核不画刷新圈,外壳画。

- `start`:开始。`threshold` 是松手即可刷新的拉动距离(vp)。
- `pull`:拖动中。`distance` 是已拉动的距离(vp,已扣掉开始前约 50vp 的起始距离),
  外壳据此画跟手的刷新圈,`distance >= threshold` 时可以提示"松手刷新"。
- `release`:松手。`refresh: true` 时**内核已经开始 reload**,外壳不用再发 `reload`,
  把刷新圈转到页面加载完成(看 `loadProgress`/`isLoading`)再收起;`false` 表示没拉够、
  拉动变成了反向滚动或被打断(比如切了标签),外壳直接收起刷新圈。
- `reset`:手势所在的页面没了(标签被关闭等),外壳收起刷新圈。

只在 `uiFamily` 为手机时开启;平板和 2in1 不会收到。

**GPU 上下文丢失**

```
gpuContextLost { windowId?: number, recovered: boolean }
```

手机上 GPU 跑在浏览器进程里(`--single-process --in-process-gpu`),上下文丢了
没有 GPU 进程可以重启——上游的恢复手段在这里是空操作,于是画面停在最后一帧不
动,只能重启应用。现在内核改成通知所有客户端重建并重绘,等价于重启的效果而不
重启;这条事件告诉外壳发生过,以及有没有救回来。

`recovered: true` 时画面会自己回来,外壳通常不用做什么,记一笔即可;
`recovered: false` 说明连重建都没做成,建议提示用户或重载当前页。

一次丢失只发一条。内核里每个 GPU 客户端会各报一次,内核收集 500ms 后合成一条
发出;其中任何一次没救回来,`recovered` 就是 `false`。

测试方法:在地址栏输入 `chrome://gpu-lose-context/`,内核会主动丢一次上下文。

触发场景(还没有逐一验证):切后台较久、锁屏解锁、内存吃紧、折叠展开、窗口大小
变化、旋转、切换界面样式。

**上次异常退出后恢复标签页**

```
lastSessionRestorable { windowId?: number }
restoreLastSession { restore: boolean }       // 命令
```

应用被杀(系统因 `GpuError`、内存查杀,或者用户划掉)时 Chromium 没有正常退出,
下次启动只会打开启动 URL,之前的标签页就丢了。现在内核会在第一次状态快照时检查
上次是不是异常退出,是的话发 `lastSessionRestorable`,状态快照里的
`lastSessionRestorable` 也会一直是 `true`,直到外壳回复 `restoreLastSession`。
`restore: true` 恢复上次的所有标签页,`false` 放弃。不回复的话,上次的会话一直留
着,下次启动还会再问。

要不要问、怎么问由外壳决定,因为外壳知道应用为什么退出
(`LaunchParam.lastExitReason`)。建议:用户划掉或内存查杀直接恢复;`GpuError`
这类可能是某个网页把应用搞崩的情况,先问用户,否则一恢复可能又被同一个网页搞崩。

**画中画**

长按视频的菜单里有 `pictureInPicture`,用的是 Chromium 自己的画中画。它出来的是一个
**辅助窗口**,和无痕窗口、拖出来的标签同一个形状:`windowRole: "browser"` 之外的
auxiliary 窗口状态 + 自己的 surface,外壳照已有的那套摆放就行,不用新接口。播放/暂停、
关闭、回到标签页这些控件由 Chromium 自己画在窗口里。

辅助窗口状态里带 `pictureInPicture: true`。**用它把画中画和网页自己的弹窗区分开** ——
两者都是浮在页面上、没有自己的关闭按钮,但返回键应该关掉弹窗、放过画中画,而画中画
应该待在角落而不是屏幕中间。旧引擎上这个字段不存在。

**注意它不是鸿蒙的系统小窗**:它活在应用里,应用退到后台就没了。要"离开浏览器还continue
播放"的系统小窗,得把视频的 viz surface 嵌进外壳用 `@ohos.PiPWindow` 建的窗口里,那是另
一件大得多的工作(见 docs 里的讨论),现在没有做。

**应用接续:读取和恢复网页位置**

`browsingApiVersion` 3 起。

- **优先用 `pageScrollSettled`**。滚动停下 300ms 后内核主动推一次,外壳存着就行,
  `onContinue` 里直接拿,不用等回复。
- 拿不准的时候再用 `getPageContinuation`,它要问渲染进程,答案晚一两帧才到 ——
  `onContinue` 是同步回调,等不起。
- 位置一律是 **CSS 像素**;`pageWidth` / `pageHeight` 是文档总宽高。目标设备宽度
  不同、排版会变,所以按 `scrollY / pageHeight` 这个比例换算,不要直接搬像素。
- 恢复:`newTab { url, scrollRatio }`,`scrollRatio` 是 0–1。页面**加载完成**后
  滚一次(`load` 事件;4 秒还没 load 完就按当时的高度滚),**只滚这一次**。用户在
  这之前自己滚过、或者用了手势/滚轮,就不滚了 —— 到了地方又被拽走比停在顶部更糟。
- 无痕窗口不接续,内核这边没有特别处理,外壳不发就行。

**网站图标**

| 命令 | 参数 | 返回 |
|---|---|---|
| `getFavicons` | `requestId`, `urls`, `sizeVp` | `favicons { requestId, items: { url, pngBase64 }[] }`。取不到的网址不返回，外壳用首字母占位。一次最多 100 个 |

---

## 7. 辅助窗口

Chromium 的一部分窗口是独立的原生窗口,需要外壳给它们放一个 XComponent,比如屏幕共享时的"正在共享"提示条。权限气泡和设备选择框这类弹窗不属于这种情况,它们画在网页区域里面,外壳不用处理。

`onAuxiliaryWindowState` 每次收到一个窗口的完整状态:

- `visible && !destroyed`:显示或更新这个窗口;否则移除。
- `windowRole === 'pwa'`:这是 PWA 窗口,要用单独的应用窗口打开。参考 `entry/.../PwaWindowLauncher.ets`。
- `windowRole === 'browser'`:这是外壳自己的第二个浏览器窗口(比如无痕窗口),不要按辅助窗口的布局处理——不裁剪、不居中、不加遮罩。外壳需要一个独立的、全屏的 `WebWindow`,XComponent 的 `id`/`componentId` 用引擎给的 `aura_win_<widget>`。事件上还带 `incognito`,标记这个窗口的 profile 是否是无痕的。
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

### ANGLE 后端(调试用)

启动配置里的 `angleBackend?: 'gles' | 'vulkan'` 决定 ANGLE 用哪个后端画。不填或
`'gles'` 是鸿蒙自己的 EGL/GLES,设备上应该用这个;`'vulkan'` 走 Vulkan
(`VK_OHOS_surface`),只用于和 GLES 对比测量,还没有在设备上长时间跑过。只在启
动时读一次,改了要重启应用。`chrome://flags` 里的对应开关会被这个启动参数盖掉,
不起作用。

### 独立 GPU 进程(测试中)

启动配置里的 `gpuProcess?: 'in-process' | 'separate'` 决定 GPU 放在哪。不填或
`'in-process'` 是现在的做法,GPU 在浏览器进程里;`'separate'` 在平板和 2in1 上
给 GPU 单独开一个进程,GPU 出错时只会影响这个进程,不会连带浏览器。手机不能创建
子进程,传了也会忽略,hilog 里记一行 `AuraShell ignored gpuProcess separate`。

**Skia 走 Vulkan(测试中)。** `skiaBackend?: 'gl' | 'vulkan'`,不填是 GL。只在
`gpuProcess: 'separate'` 时有效,进程内 GPU 时忽略,hilog 里记一行
`AuraShell ignored skiaBackend vulkan`。Vulkan 起不来时 Chromium 自己退回 GL,不会黑
屏;GPU 进程启动时的 `OHOS GPU child Vulkan probe` 几行日志说明卡在哪一步(加载器、
实例扩展、实例、物理设备、交换链支持),Chromium 自己的步骤看 `OHOS Vulkan:` 开头的
日志。`chrome://gpu` 里 Vulkan 为 Enabled 才算真的在用。

所有窗口走的是同一条通道,但还没有逐一验证(主窗口之外:无痕、拖出的标签、画中
画、PWA、弹出菜单);GPU 进程崩溃重启和 `gpuContextLost`/`gpuFallback` 事件也还没
接。只在测试版里打开。

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

**JIT 需要重新签名（必须做）**：引擎 HAR 声明了 `ohos.permission.kernel.ALLOW_WRITABLE_CODE_MEMORY`，这是个 ACL 权限，外壳的签名 profile 里也必须带上它，否则会出现两种情况：
- 安装时报 `9568289 grant request permissions failed`；
- 装上后系统不给 JIT，引擎退回解释器（日志 `AuraShell JIT unavailable`）。JS 会慢好几倍，Cloudflare 人机验证也会失败（600010）。

做法：外壳自己的 `module.json5` 里也声明一遍这个权限，然后在 DevEco Studio 的 Signing Configs 里重新生成自动签名。

HAR 的 `module.json5` 声明了 13 项权限(包括 WebAuthn 用到的 `ACCESS_BIOMETRIC`、后台播放用到的 `KEEP_BACKGROUND_RUNNING`),并附带权限说明文案。打包时这些权限会自动合并进外壳的 HAP,外壳不需要重复声明。
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
