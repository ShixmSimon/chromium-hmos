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
| `bookmarked` | 当前标签页的网址是否已加入书签。菜单里的书签开关用它。 |
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
| `contextMenuRequested` | **是**(手机) | 长按网页元素。引擎这时不弹自己的菜单，由外壳画，见下文"长按菜单"。**每次都要回一条命令**：选了某项发 `contextMenuAction`，没选就关掉发 `contextMenuDismissed`。 |
| `browserControlsRatio` | 否 | 用了 `setBrowserControls` 才会有。`ratio` 为顶栏当前露出的比例（1 全部显示，0 完全滑走），外壳把顶栏往上移 `(1 - ratio) × 顶栏高度`。传了 `minTop` 时，比例的下限是 `minTop / top`（顶栏收起到 `minTop` 就不会再往上滑了）。 |

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
| `setBrowserControls` | `top`（vp）, `minTop?`（vp） | 外壳顶栏的高度，交给 Chromium 当作 browser controls：网页顶部给它留出位置，网页下滑时顶栏被滑走，上滑时再出现。引擎按当前设备的缩放比例把 vp 换算成物理像素后再交给 Chromium。`minTop` 是顶栏收起后仍保留的高度（vp），不传或传 0 表示可以完全滑走；`minTop` 会被夹到 `[0, top]` 之间。显示比例通过 `browserControlsRatio` 事件告诉外壳，外壳据此移动顶栏；`top` 传 0 取消 |
| `setBrowserControlsState` | `state`: `'shown' \| 'hidden' \| 'both'`, `animate?` | 主动把顶栏收起或展开，而不是等网页滚动触发。`'shown'` 强制展开、`'hidden'` 强制收起（收到 `minTop`）、`'both'`（默认）交还给滚动控制。`animate` 默认 true |
| `findInPage` | `text`, `forward?` | 在当前标签页查找，同一段文字再发一次即跳到下一个（`forward: false` 为上一个）。结果通过 `findResult` 事件返回 |
| `stopFind` | | 结束查找，清除高亮 |
| `getPageText` | `requestId` | 读取当前网页的可见文字（最多 20000 字），在独立的脚本环境里执行，网页自己的脚本看不到。结果通过 `pageText` 事件返回 |
| `contextMenuAction` | `requestId`, `action` | 回复 `contextMenuRequested`：执行选中的项。`action` 必须是事件 `supportedActions` 里的一项 |
| `contextMenuDismissed` | `requestId` | 回复 `contextMenuRequested`：用户没选任何项就关掉了菜单 |
| `setViewportInsets` | `bottom`（vp） | 外壳在网页底部盖了多高的悬浮栏。网页照常画到底，但可视区域缩小这么多，网页末尾能滚到悬浮栏上方。切换标签、新建标签后引擎会自动沿用，不用重发；传 0 取消 |
| `pwaHome`、`pwaMenu`、`pwaMenuAction`、`pwaMenuDismiss` | 见 `BrowserCommandPayload` | PWA 窗口菜单 |
| `defaultBrowserState`、`systemCapabilities` | 见参考实现 | 系统集成相关状态 |

后退、前进、刷新这类命令,以 `aura_shell_runtime_bridge.cc` 里 `ExecuteAuraShellBrowserCommand` 实际接受的为准。这张表是按 `286a844` 整理的。

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
| `getSearchEngines` | `requestId` | `searchEngines { requestId, items: { id, name, keyword, url, isDefault }[] }`，只列出能设为默认的搜索引擎 |
| `setDefaultSearchEngine` | `id` | 策略或扩展控制默认搜索引擎时不生效 |
| `getPrefs` | `requestId`, `keys?` | `prefs { requestId, values: { key, value }[] }`。不传 `keys` 返回全部；本版本不支持的键不返回 |
| `setPref` | `key`, `value` | 写入成功后推送 `prefsChanged { keys }`。被策略锁定的项写不进去 |
| `getSiteSettings` | `requestId`, `type?` | `siteSettings { requestId, items: { origin, type, setting, embeddingOrigin? }[] }`，只列出用户单独设置过的网站。`origin` 是 Chromium 的匹配规则，例如 `https://a.com:443`、`[*.]a.com` |
| `getSiteSettingsForOrigin` | `requestId`, `origin` | 同上，列出这个网站每一类权限的当前值 |
| `setSiteSetting` | `origin`, `type`, `setting` | `setting` 为 `'allow' \| 'block' \| 'ask' \| 'default'`，`default` 表示删除这条单独设置。定位、摄像头、麦克风、通知、剪贴板只能对 https 网站设置；`storageAccess` 不能按单个网站设置 |
| `setDefaultSiteSetting` | `type`, `setting` | |
| `resetSiteSettings` | `origin` | 清除这个网站的全部权限和存储 |
| `getAboutInfo` | `requestId` | `aboutInfo { requestId, chromiumVersion, engineCommit, userAgent }` |

`types`：`history`、`cookies`、`cache`、`siteSettings`、`formData`、`passwords`、`downloads`。`timeRange`：`lastHour`、`lastDay`、`lastWeek`、`last4Weeks`、`all`。

偏好键（只接受这些）：`blockThirdPartyCookies`、`doNotTrack`、`safeBrowsing`（`'off' | 'standard' | 'enhanced'`）、`preloadPages`、`popupsBlocked`、`javascriptEnabled`、`textScale`、`autofillAddresses`、`autofillCards`、`downloadAskWhereToSave`。

- `textScale` 是 50–200 的百分比。桌面版 Chromium 没有只放大文字的设置，所以这里改的是网页的默认缩放比例，整页一起放大。
- 这个版本没有配置 Google API 密钥，`safeBrowsing` 开关能保存，但实际上很可能不起作用，设置页不要承诺有安全浏览保护。

权限类型：`location`、`camera`、`microphone`、`notifications`、`javascript`、`popups`、`sound`、`clipboard`、`storageAccess`。没有 `autoplay`：桌面版 Chromium 实际上不执行这项设置，要控制声音请用 `sound`。

所有设置都读写普通 Profile，从无痕窗口发的命令也一样。

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
