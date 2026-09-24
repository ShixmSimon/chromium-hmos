import { common } from '@kit.AbilityKit';
import { AVCastPicker, AVCastPickerColorMode, AVCastPickerStyle } from '@kit.AVSessionKit';
import { hdsEffect } from '@kit.UIDesignKit';
import { inputMethod } from '@kit.IMEKit';
import {
  BrowserCommandPayload,
  BrowserStateSnapshot,
  BrowserTabState
} from 'engine';
import { HuaweiShareAdapter } from 'engine';
import { GripSide, HoldingHandRouter } from './HoldingHandRouter';
import { CurrentTabCastSession } from './CurrentTabCastSession';

enum MobileEdgeAction {
  BACK = 'back',
  MENU = 'menu'
}

const PRIMARY_ACTION_KEY: string = 'mobilePrimaryAction';
const FOLLOW_GRIP_KEY: string = 'mobileFollowGrip';
const LAST_GRIP_SIDE_KEY: string = 'mobileLastGripSide';

@Component
export struct MobileBrowserOverlay {
  @StorageLink('auraColorScheme') colorScheme: string = 'light';
  @Prop @Watch('onBrowserStateChanged') browserState: BrowserStateSnapshot;
  onCommand: (command: BrowserCommandPayload) => void = (_command: BrowserCommandPayload): void => {};

  @State private menuVisible: boolean = false;
  @State private editingAddress: boolean = false;
  @State private addressDraft: string = '';
  @State private primaryAction: MobileEdgeAction = MobileEdgeAction.BACK;
  @State private followGrip: boolean = true;
  @State private lastGripSide: GripSide = GripSide.LEFT;

  private readonly holdingHandRouter: HoldingHandRouter = new HoldingHandRouter();
  private readonly currentTabCastSession: CurrentTabCastSession = new CurrentTabCastSession();
  private keyboardRecoveryTimers: number[] = [];

  aboutToAppear(): void {
    const storedAction = String(AppStorage.get(PRIMARY_ACTION_KEY) ?? MobileEdgeAction.BACK);
    this.primaryAction = storedAction === MobileEdgeAction.MENU ?
      MobileEdgeAction.MENU : MobileEdgeAction.BACK;
    this.followGrip = Boolean(AppStorage.get(FOLLOW_GRIP_KEY) ?? true);
    const storedSide = String(AppStorage.get(LAST_GRIP_SIDE_KEY) ?? GripSide.LEFT);
    this.lastGripSide = storedSide === GripSide.RIGHT ? GripSide.RIGHT : GripSide.LEFT;
    if (this.followGrip) {
      this.startHoldingHandAwareness();
    }
    // The cast session starts when the cast picker opens, not here. HarmonyOS
    // allows one AVSession per ability, and an idle one held from launch left
    // the engine's media session (control center card) unable to exist.
  }

  aboutToDisappear(): void {
    this.clearKeyboardRecoveryTimers();
    this.holdingHandRouter.stop();
    this.currentTabCastSession.stop().catch((error: Error) => {
      console.error('Failed to stop current-tab cast session:', error);
    });
  }

  private onBrowserStateChanged(): void {
    this.currentTabCastSession.update(this.browserState);
  }

  build() {
    Column() {
      Row({ space: 10 }) {
        this.EdgeButton(this.leftAction())
        this.AddressBubble()
        this.EdgeButton(this.rightAction())
      }
      .width('100%')
      .height(56)
      .alignItems(VerticalAlign.Center)
    }
    .width('100%')
    .padding({ left: 12, right: 12, bottom: 12 })
    .bindSheet($$this.menuVisible, this.MenuSheet, {
      height: SheetSize.LARGE,
      showClose: false,
      enableOutsideInteractive: false,
      maskColor: Color.Transparent
    })
  }

  @Builder
  private EdgeButton(action: MobileEdgeAction) {
    Button({ type: ButtonType.Circle, stateEffect: true }) {
      Image(action === MobileEdgeAction.BACK ?
        $r('app.media.chromium_back') : $r('app.media.chromium_more'))
        .width(24)
        .height(24)
        .fillColor($r('app.color.icon_primary'))
    }
    .width(52)
    .height(52)
    .backgroundColor($r('app.color.surface_floating'))
    .shadow({ radius: 16, color: $r('app.color.shadow'), offsetY: 4 })
    .visualEffect(this.buildBubbleEffect())
    .opacity(action === MobileEdgeAction.BACK && !this.browserState.canGoBack ? 0.52 : 1)
    .enabled(action !== MobileEdgeAction.BACK || this.browserState.canGoBack)
    .accessibilityText(action === MobileEdgeAction.BACK ? '返回' : '菜单')
    .onClick(() => this.handleEdgeAction(action))
  }

  @Builder
  private AddressBubble() {
    Row() {
      if (this.editingAddress) {
        TextInput({ text: this.addressDraft, placeholder: '搜索或输入网址' })
          .id('mobile_address_input')
          .layoutWeight(1)
          .height(52)
          .fontSize(16)
          .fontColor($r('app.color.text_primary'))
          .placeholderColor($r('app.color.text_secondary'))
          .backgroundColor(Color.Transparent)
          .padding({ left: 18, right: 12 })
          .enterKeyType(EnterKeyType.Go)
          .selectAll(true)
          .onChange((value: string) => {
            this.addressDraft = value;
          })
          .onSubmit(() => {
            this.submitAddress();
          })
        Button({ type: ButtonType.Circle }) {
          Image($r('app.media.chromium_close'))
            .width(20)
            .height(20)
            .fillColor($r('app.color.icon_secondary'))
        }
        .width(40)
        .height(40)
        .margin({ right: 6 })
        .backgroundColor(Color.Transparent)
        .accessibilityText('取消编辑')
        .onClick(() => {
          this.clearKeyboardRecoveryTimers();
          this.editingAddress = false;
        })
      } else {
        Button({ type: ButtonType.Normal, stateEffect: true }) {
          Row({ space: 8 }) {
            if (this.browserState.loading) {
              LoadingProgress()
                .width(18)
                .height(18)
                .color($r('app.color.accent_primary'))
            }
            Text(this.displayDomain())
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
              .fontSize(15)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('app.color.text_primary'))
          }
          .justifyContent(FlexAlign.Center)
          .width('100%')
        }
        .layoutWeight(1)
        .height(52)
        .padding({ left: 14, right: 14 })
        .backgroundColor(Color.Transparent)
        .accessibilityText('地址栏')
        .onClick(() => this.beginAddressEdit())
      }
    }
    .layoutWeight(1)
    .height(52)
    .borderRadius(26)
    .backgroundColor($r('app.color.surface_floating'))
    .shadow({ radius: 16, color: $r('app.color.shadow'), offsetY: 4 })
    .visualEffect(this.buildBubbleEffect())
  }

  @Builder
  private MenuSheet() {
    Column() {
      Row() {
        Text('标签页')
          .fontSize(20)
          .fontWeight(FontWeight.Bold)
          .fontColor($r('app.color.text_primary'))
        Blank()
        Button({ type: ButtonType.Circle }) {
          Image($r('app.media.chromium_add'))
            .width(22)
            .height(22)
            .fillColor($r('app.color.icon_primary'))
        }
        .width(44)
        .height(44)
        .backgroundColor($r('app.color.surface_secondary'))
        .accessibilityText('新建标签页')
        .onClick(() => {
          this.issueCommand({ command: 'newTab' });
        })
      }
      .width('100%')
      .height(52)

      List({ space: 2 }) {
        ForEach(this.browserState.tabs, (tab: BrowserTabState) => {
          ListItem() {
            Row({ space: 10 }) {
              Row()
                .width(4)
                .height(28)
                .borderRadius(2)
                .backgroundColor(tab.active ?
                  $r('app.color.accent_primary') : $r('app.color.transparent'))
              Column({ space: 2 }) {
                Text(tab.title && tab.title.length > 0 ? tab.title : '新标签页')
                  .maxLines(1)
                  .textOverflow({ overflow: TextOverflow.Ellipsis })
                  .fontSize(16)
                  .fontWeight(tab.active ? FontWeight.Medium : FontWeight.Regular)
                  .fontColor($r('app.color.text_primary'))
                Text(this.shortUrl(tab.url))
                  .maxLines(1)
                  .textOverflow({ overflow: TextOverflow.Ellipsis })
                  .fontSize(12)
                  .fontColor($r('app.color.text_secondary'))
              }
              .alignItems(HorizontalAlign.Start)
              .layoutWeight(1)
              Button({ type: ButtonType.Circle }) {
                Image($r('app.media.chromium_close'))
                  .width(18)
                  .height(18)
                  .fillColor($r('app.color.icon_secondary'))
              }
              .width(40)
              .height(40)
              .backgroundColor(Color.Transparent)
              .accessibilityText('关闭标签页')
              .onClick(() => {
                this.issueCommand({ command: 'closeTab', index: tab.index });
              })
            }
            .width('100%')
            .height(58)
            .padding({ left: 4, right: 2 })
            .onClick(() => {
              this.issueCommand({ command: 'activateTab', index: tab.index });
            })
          }
        }, (tab: BrowserTabState): string => tab.index.toString())
      }
      .width('100%')
      .layoutWeight(1)

      Divider()
        .color($r('app.color.divider'))
        .margin({ top: 6, bottom: 10 })

      Row({ space: 8 }) {
        this.CommandButton(
          this.browserState.loading ? '停止' : '刷新',
          $r('app.media.chromium_reload'),
          this.browserState.loading ? 'stop' : 'reload')
        this.ShareButton()
        this.CommandButton(
          '\u6253\u5370',
          $r('app.media.chromium_print'),
          'print')
        this.CastButton()
      }
      .width('100%')

      Row() {
        Text('主按钮')
          .fontSize(15)
          .fontColor($r('app.color.text_secondary'))
        Blank()
        Row() {
          Button('返回')
            .height(36)
            .fontSize(14)
            .fontColor(this.primaryAction === MobileEdgeAction.BACK ?
              $r('app.color.on_accent') : $r('app.color.text_secondary'))
            .backgroundColor(this.primaryAction === MobileEdgeAction.BACK ?
              $r('app.color.accent_primary') : $r('app.color.transparent'))
            .borderRadius(8)
            .onClick(() => this.setPrimaryAction(MobileEdgeAction.BACK))
          Button('菜单')
            .height(36)
            .fontSize(14)
            .fontColor(this.primaryAction === MobileEdgeAction.MENU ?
              $r('app.color.on_accent') : $r('app.color.text_secondary'))
            .backgroundColor(this.primaryAction === MobileEdgeAction.MENU ?
              $r('app.color.accent_primary') : $r('app.color.transparent'))
            .borderRadius(8)
            .onClick(() => this.setPrimaryAction(MobileEdgeAction.MENU))
        }
        .padding(2)
        .borderRadius(8)
        .backgroundColor($r('app.color.surface_secondary'))
      }
      .width('100%')
      .height(52)

      Row() {
        Text('跟随握持手')
          .fontSize(15)
          .fontColor($r('app.color.text_secondary'))
        Blank()
        Toggle({ type: ToggleType.Switch, isOn: this.followGrip })
          .onChange((value: boolean) => this.setFollowGrip(value))
      }
      .width('100%')
      .height(52)
    }
    .width('100%')
    .height('100%')
    .padding({ left: 20, right: 20, bottom: 16 })
    .backgroundColor($r('app.color.surface_primary'))
  }

  @Builder
  private CommandButton(label: string, icon: Resource, command: string) {
    Button({ type: ButtonType.Normal }) {
      Column({ space: 4 }) {
        Image(icon)
          .width(22)
          .height(22)
          .fillColor($r('app.color.icon_primary'))
        Text(label)
          .fontSize(12)
          .fontColor($r('app.color.text_secondary'))
      }
    }
    .layoutWeight(1)
    .height(58)
    .borderRadius(8)
    .backgroundColor($r('app.color.surface_secondary'))
    .onClick(() => this.issueCommand({ command }))
  }

  @Builder
  private ShareButton() {
    Button({ type: ButtonType.Normal }) {
      Column({ space: 4 }) {
        Image($r('app.media.chromium_share'))
          .width(22)
          .height(22)
          .fillColor($r('app.color.icon_primary'))
        Text('华为分享')
          .fontSize(12)
          .fontColor($r('app.color.text_secondary'))
      }
    }
    .layoutWeight(1)
    .height(58)
    .borderRadius(8)
    .backgroundColor($r('app.color.surface_secondary'))
    .onClick(() => this.shareCurrentPage())
  }

  @Builder
  private CastButton() {
    AVCastPicker({
      normalColor: this.colorScheme === 'dark' ? '#F5F5F5' : '#202124',
      activeColor: this.colorScheme === 'dark' ? '#5B8CFF' : '#0A59F7',
      pickerStyle: AVCastPickerStyle.STYLE_PANEL,
      colorMode: AVCastPickerColorMode.AUTO,
      sessionType: 'video',
      customPicker: this.CastPickerContent
    })
      .layoutWeight(1)
      .height(58)
  }

  @Builder
  private CastPickerContent() {
    Column({ space: 4 }) {
      Image($r('app.media.chromium_cast'))
        .width(22)
        .height(22)
        .fillColor($r('app.color.icon_primary'))
      Text('投放')
        .fontSize(12)
        .fontColor($r('app.color.text_secondary'))
    }
    .width('100%')
    .height(58)
    .justifyContent(FlexAlign.Center)
    .borderRadius(8)
    .backgroundColor($r('app.color.surface_secondary'))
  }

  private buildBubbleEffect(): VisualEffect {
    return new hdsEffect.HdsEffectBuilder()
      .pressShadow(hdsEffect.PressShadowType.BLEND_GRADIENT)
      .buildEffect();
  }

  private leftAction(): MobileEdgeAction {
    if (this.lastGripSide === GripSide.LEFT) {
      return this.primaryAction;
    }
    return this.secondaryAction();
  }

  private rightAction(): MobileEdgeAction {
    if (this.lastGripSide === GripSide.RIGHT) {
      return this.primaryAction;
    }
    return this.secondaryAction();
  }

  private secondaryAction(): MobileEdgeAction {
    return this.primaryAction === MobileEdgeAction.BACK ?
      MobileEdgeAction.MENU : MobileEdgeAction.BACK;
  }

  private handleEdgeAction(action: MobileEdgeAction): void {
    if (action === MobileEdgeAction.BACK) {
      this.issueCommand({ command: 'back' });
    } else {
      this.menuVisible = true;
    }
  }

  private displayDomain(): string {
    if (this.isNewTabUrl(this.browserState.url)) {
      return '新标签页';
    }
    if (this.browserState.domain && this.browserState.domain.length > 0) {
      return this.browserState.domain;
    }
    return this.browserState.title && this.browserState.title.length > 0 ?
      this.browserState.title : 'Chromium';
  }

  private shortUrl(url: string): string {
    if (!url || this.isNewTabUrl(url)) {
      return '新标签页';
    }
    return url.replace(/^https?:\/\//, '');
  }

  private isNewTabUrl(url: string): boolean {
    return url.startsWith('chrome://newtab') ||
      url.startsWith('chrome://new-tab-page') ||
      url.startsWith('chrome://new-tab-page-third-party');
  }

  private beginAddressEdit(): void {
    this.addressDraft = this.browserState.url.startsWith('chrome://') ? '' : this.browserState.url;
    this.editingAddress = true;
    this.clearKeyboardRecoveryTimers();
    [0, 80, 220, 450].forEach((delay: number): void => {
      this.keyboardRecoveryTimers.push(setTimeout((): void => {
        if (!this.editingAddress) {
          return;
        }
        try {
          this.getUIContext().getFocusController().requestFocus('mobile_address_input');
          inputMethod.getController().showTextInput().catch((error: Error): void => {
            console.error('Failed to show the address keyboard:', error);
          });
        } catch (error) {
        }
      }, delay));
    });
  }

  private submitAddress(): void {
    this.clearKeyboardRecoveryTimers();
    const target = this.normalizeAddress(this.addressDraft);
    if (target.length > 0) {
      this.issueCommand({ command: 'navigate', url: target });
    }
    this.editingAddress = false;
  }

  private clearKeyboardRecoveryTimers(): void {
    this.keyboardRecoveryTimers.forEach((timer: number): void => clearTimeout(timer));
    this.keyboardRecoveryTimers = [];
  }

  private normalizeAddress(value: string): string {
    const input = value.trim();
    if (input.length === 0) {
      return '';
    }
    if (/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(input)) {
      return input;
    }
    if (/\s/.test(input) || !input.includes('.')) {
      return `https://www.bing.com/search?q=${encodeURIComponent(input)}`;
    }
    return `https://${input}`;
  }

  private issueCommand(command: BrowserCommandPayload): void {
    this.onCommand(command);
  }

  private setPrimaryAction(action: MobileEdgeAction): void {
    this.primaryAction = action;
    AppStorage.setOrCreate(PRIMARY_ACTION_KEY, action);
  }

  private setFollowGrip(value: boolean): void {
    this.followGrip = value;
    AppStorage.setOrCreate(FOLLOW_GRIP_KEY, value);
    if (value) {
      this.startHoldingHandAwareness();
    } else {
      this.holdingHandRouter.stop();
    }
  }

  private startHoldingHandAwareness(): void {
    this.holdingHandRouter.start((side: GripSide): void => {
      this.lastGripSide = side;
      AppStorage.setOrCreate(LAST_GRIP_SIDE_KEY, side);
    });
  }

  private shareCurrentPage(): void {
    const context = this.getUIContext().getHostContext() as common.UIAbilityContext | undefined;
    if (!context) {
      return;
    }
    HuaweiShareAdapter.sharePage(context, this.browserState.url, this.browserState.title)
      .catch((error: Error) => {
        console.error('Huawei Share failed:', error);
      });
  }
}
