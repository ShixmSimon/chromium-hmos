import { ComponentContent } from '@kit.ArkUI';
import { pasteboard } from '@kit.BasicServicesKit';

export function hasSystemPlainText(): boolean {
  try {
    return pasteboard.getSystemPasteboard().hasDataType(pasteboard.MIMETYPE_TEXT_PLAIN);
  } catch {
    return true;
  }
}

class SystemPasteDialogParams {
  onPaste: (text: string) => void;
  onCancel: () => void;

  constructor(onPaste: (text: string) => void, onCancel: () => void) {
    this.onPaste = onPaste;
    this.onCancel = onCancel;
  }
}

@Component
struct SystemPasteDialog {
  params: SystemPasteDialogParams = new SystemPasteDialogParams(() => {}, () => {});
  @State private reading: boolean = false;
  @State private error: string = '';
  private alive: boolean = true;

  aboutToDisappear(): void {
    this.alive = false;
  }

  build() {
    Column({ space: 16 }) {
      Text('粘贴文字')
        .fontSize(20)
        .fontWeight(FontWeight.Medium)
      PasteButton()
        .padding({ top: 12, bottom: 12, left: 24, right: 24 })
        .enabled(!this.reading)
        .onClick((_event: ClickEvent, result: PasteButtonOnClickResult): void => {
          if (result !== PasteButtonOnClickResult.SUCCESS || this.reading) {
            this.error = '请点击系统粘贴按钮以允许粘贴';
            return;
          }
          this.reading = true;
          this.error = '';
          pasteboard.getSystemPasteboard().getData().then((data: pasteboard.PasteData): void => {
            if (!this.alive) {
              return;
            }
            this.reading = false;
            const text = data.hasType(pasteboard.MIMETYPE_TEXT_PLAIN) ? data.getPrimaryText() : '';
            if (text.length === 0) {
              this.error = '剪贴板中没有可粘贴的文字';
              return;
            }
            this.params.onPaste(text);
          }).catch((): void => {
            if (!this.alive) {
              return;
            }
            this.reading = false;
            this.error = '无法读取剪贴板，请重新点击粘贴';
          });
        })
      if (this.error.length > 0) {
        Text(this.error)
          .fontSize(14)
      }
      Button('取消')
        .onClick((): void => this.params.onCancel())
    }
    .padding(24)
    .width('100%')
  }
}

@Builder
function buildSystemPasteDialog(params: SystemPasteDialogParams) {
  SystemPasteDialog({ params: params })
}

export function showSystemPasteDialog(uiContext: UIContext,
  onPaste: (text: string) => void, onCancel: () => void): void {
  const prompt = uiContext.getPromptAction();
  let finished = false;
  let disposed = false;
  let content: ComponentContent<SystemPasteDialogParams> | undefined = undefined;
  const dispose = (): void => {
    if (!disposed) {
      disposed = true;
      content?.dispose();
    }
  };
  const finish = (text: string | undefined): void => {
    if (finished) {
      return;
    }
    finished = true;
    if (!content) {
      onCancel();
      return;
    }
    prompt.closeCustomDialog(content).then((): void => {
      if (text !== undefined) {
        onPaste(text);
      } else {
        onCancel();
      }
    }).catch((): void => onCancel());
  };
  const params = new SystemPasteDialogParams(
    (text: string): void => finish(text), (): void => finish(undefined));
  content = new ComponentContent(uiContext, wrapBuilder(buildSystemPasteDialog), params);
  prompt.openCustomDialog(content, {
    alignment: DialogAlignment.Center,
    autoCancel: true,
    onWillDisappear: (): void => {
      if (!finished) {
        finished = true;
        onCancel();
      }
    },
    onDidDisappear: (): void => dispose()
  }).catch((): void => {
    if (!finished) {
      finished = true;
      onCancel();
    }
    dispose();
  });
}
