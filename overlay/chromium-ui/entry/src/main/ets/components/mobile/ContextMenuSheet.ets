import { BrowserCommandPayload, BrowserRuntimeEvent, ContextMenuAction } from 'engine';

// The reference shell's long-press menu: a system action sheet listing what
// the engine offered in contextMenuRequested. A shell with its own design
// draws its own; the protocol is the same (docs/SHELL_CONTRACT.md).

const ACTION_LABELS: Record<ContextMenuAction, string> = {
  'openLinkInNewTab': '在新标签页中打开',
  'copyLinkAddress': '复制链接地址',
  'copyLinkText': '复制链接文字',
  'saveLinkAs': '下载链接',
  'openImageInNewTab': '在新标签页中打开图片',
  'saveImageAs': '保存图片',
  'copyImage': '复制图片',
  'copyImageAddress': '复制图片地址',
  'toggleLoop': '循环播放',
  'toggleControls': '显示控件',
  'openMediaInNewTab': '在新标签页中打开',
  'saveMediaAs': '保存',
  'copyMediaAddress': '复制地址',
  'copyVideoFrame': '复制视频帧',
  'copy': '复制',
  'cut': '剪切',
  'paste': '粘贴',
  'selectAll': '全选',
  'searchSelection': '搜索'
};

function labelFor(event: BrowserRuntimeEvent, action: ContextMenuAction): string {
  const flags = event.mediaFlags;
  if (action === 'toggleLoop' && flags?.loop) {
    return '取消循环播放';
  }
  if (action === 'toggleControls' && flags?.controls) {
    return '隐藏控件';
  }
  return ACTION_LABELS[action] ?? action;
}

function titleFor(event: BrowserRuntimeEvent): string {
  if (event.linkUrl) {
    return event.linkUrl;
  }
  if (event.mediaType === 'image') {
    return '图片';
  }
  if (event.mediaType === 'video') {
    return '视频';
  }
  if (event.mediaType === 'audio') {
    return '音频';
  }
  if (event.selectionText) {
    return event.selectionText.length > 40 ?
      `${event.selectionText.slice(0, 40)}…` : event.selectionText;
  }
  return event.isEditable ? '编辑' : '网页';
}

export function showContextMenuSheet(uiContext: UIContext, event: BrowserRuntimeEvent,
  send: (command: BrowserCommandPayload) => void): void {
  const requestId = event.requestId ?? 0;
  const actions: ContextMenuAction[] = event.supportedActions ?? [];
  if (actions.length === 0) {
    send({ command: 'contextMenuDismissed', requestId: requestId });
    return;
  }
  const sheets: SheetInfo[] = actions.map((action: ContextMenuAction): SheetInfo => ({
    title: labelFor(event, action),
    action: (): void => {
      send({ command: 'contextMenuAction', requestId: requestId, action: action });
    }
  }));
  uiContext.showActionSheet({
    title: titleFor(event),
    message: event.linkText ?? '',
    autoCancel: true,
    alignment: DialogAlignment.Bottom,
    sheets: sheets,
    cancel: (): void => {
      send({ command: 'contextMenuDismissed', requestId: requestId });
    }
  });
}
