import { common } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { BrowserCommandPayload, BrowserStateSnapshot } from 'engine';

@Component
export struct PwaCapsuleOverlay {
  @Prop browserState: BrowserStateSnapshot;
  @Prop windowStatus: number = 0;
  onCommand: (command: BrowserCommandPayload) => void = (_command: BrowserCommandPayload): void => {};

  private menuAnchorX: number = 0;
  private menuAnchorY: number = 0;
  private menuAnchorWidth: number = 36;
  private menuAnchorHeight: number = 36;

  build() {
    Row() {
      Row() {
        this.CapsuleSymbolButton($r('sys.symbol.chevron_backward'), '\u8fd4\u56de',
          (): void => this.onCommand({ command: 'back' }), this.browserState.canGoBack)
        this.CapsuleSymbolButton($r('sys.symbol.house'), '\u56de\u5230\u5c0f\u7a0b\u5e8f\u4e3b\u9875',
          (): void => this.onCommand({
            command: 'pwaHome',
            url: this.browserState.pwaStartUrl || this.browserState.url
          }))
      }
      .height(44)
      .padding({ left: 4, right: 4 })
      .borderRadius(22)
      .backgroundColor($r('app.color.surface_floating'))
      .shadow({ radius: 12, color: $r('app.color.shadow'), offsetY: 3 })
      .hitTestBehavior(HitTestMode.Block)

      Blank()

      Row() {
        this.MoreButton()
        if (this.showsWindowControls()) {
          this.CapsuleSymbolButton(this.maximizeSymbol(), this.maximizeLabel(),
            (): void => this.runWindowAction(
              this.windowStatus === window.WindowStatusType.MAXIMIZE ? 'recover' : 'maximize'))
          this.CapsuleSymbolButton($r('sys.symbol.minus'), '\u6700\u5c0f\u5316',
            (): void => this.runWindowAction('minimize'))
        }
        this.CapsuleSymbolButton($r('sys.symbol.multiply'), '\u5173\u95ed\u5c0f\u7a0b\u5e8f',
          (): void => this.onCommand({ command: 'pwaClose' }))
      }
      .height(44)
      .padding({ left: 4, right: 4 })
      .borderRadius(22)
      .backgroundColor($r('app.color.surface_floating'))
      .shadow({ radius: 12, color: $r('app.color.shadow'), offsetY: 3 })
      .hitTestBehavior(HitTestMode.Block)
    }
    .width('100%')
    .height(60)
    .padding({ left: 12, right: 12, top: 8, bottom: 8 })
    .hitTestBehavior(HitTestMode.Transparent)
  }

  @Builder
  private MoreButton() {
    Button({ type: ButtonType.Circle, stateEffect: true }) {
      SymbolGlyph($r('sys.symbol.more'))
        .fontSize(20)
        .fontColor([ $r('app.color.icon_primary') ])
    }
    .width(36)
    .height(36)
    .backgroundColor(Color.Transparent)
    .accessibilityText('\u66f4\u591a')
    .hitTestBehavior(HitTestMode.Block)
    .onAreaChange((_oldArea: Area, newArea: Area): void => {
      this.menuAnchorX = Number(newArea.globalPosition.x);
      this.menuAnchorY = Number(newArea.globalPosition.y);
      this.menuAnchorWidth = Number(newArea.width);
      this.menuAnchorHeight = Number(newArea.height);
    })
    .onClick((): void => this.openMenu())
  }

  @Builder
  private CapsuleSymbolButton(
    symbol: Resource,
    label: string,
    action: () => void,
    enabled: boolean = true
  ) {
    Button({ type: ButtonType.Circle, stateEffect: true }) {
      SymbolGlyph(symbol)
        .fontSize(20)
        .fontColor([ $r('app.color.icon_primary') ])
    }
    .width(36)
    .height(36)
    .backgroundColor(Color.Transparent)
    .enabled(enabled)
    .opacity(enabled ? 1 : 0.42)
    .accessibilityText(label)
    .hitTestBehavior(HitTestMode.Block)
    .onClick(action)
  }

  private openMenu(): void {
    if (this.windowStatus === window.WindowStatusType.MINIMIZE) {
      return;
    }
    const presentation: 'popup' | 'sheet' =
      this.windowStatus === window.WindowStatusType.FULL_SCREEN ? 'sheet' : 'popup';
    this.onCommand({
      command: 'pwaMenu',
      presentation,
      anchorX: this.menuAnchorX,
      anchorY: this.menuAnchorY,
      anchorWidth: this.menuAnchorWidth,
      anchorHeight: this.menuAnchorHeight
    });
  }

  private showsWindowControls(): boolean {
    return this.windowStatus === window.WindowStatusType.FLOATING ||
      this.windowStatus === window.WindowStatusType.MAXIMIZE ||
      this.windowStatus === window.WindowStatusType.SPLIT_SCREEN;
  }

  private maximizeSymbol(): Resource {
    return this.windowStatus === window.WindowStatusType.MAXIMIZE ?
      $r('sys.symbol.arrow_down_right_and_arrow_up_left') :
      $r('sys.symbol.arrow_up_left_and_arrow_down_right');
  }

  private maximizeLabel(): string {
    return this.windowStatus === window.WindowStatusType.MAXIMIZE ? '\u8fd8\u539f' : '\u6700\u5927\u5316';
  }

  private runWindowAction(action: 'maximize' | 'minimize' | 'recover'): void {
    const context = this.getUIContext().getHostContext() as common.Context | undefined;
    if (!context) {
      return;
    }
    window.getLastWindow(context).then((win: window.Window): void => {
      let operation: Promise<void>;
      if (action === 'maximize') {
        operation = win.maximize();
      } else if (action === 'minimize') {
        operation = win.minimize();
      } else {
        operation = win.recover();
      }
      operation.catch((error: Error): void => {
        console.error(`PWA window ${action} failed:`, error);
      });
    }).catch((error: Error): void => {
      console.error('Failed to get the PWA host window:', error);
    });
  }
}
