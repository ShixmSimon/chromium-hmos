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
  'pictureInPicture': '画中画',
  'copy': '复制',
  'cut': '剪切',
  'paste': '粘贴',
  'selectAll': '全选',
  'searchSelection': '搜索',
  'openLinkInIncognito': '在无痕窗口中打开',
  'copyLinkToHighlight': '复制指向所选文字的链接',
  'pasteAsPlainText': '粘贴为纯文本',
  'delete': '删除',
  'undo': '撤销',
  'redo': '重做',
  'suggestPassword': '建议强密码',
  'selectSavedPassword': '选择已保存的密码',
  'managePasswords': '管理密码',
  'spellSuggestion:0': '',
  'spellSuggestion:1': '',
  'spellSuggestion:2': '',
  'spellSuggestion:3': '',
  'spellSuggestion:4': '',
  'addToDictionary': '添加到词典',
  'shareLink': '分享链接',
  // Never shown: the shell's own "share image" and "look up image" run it.
  'imageToFile': ''
};

function labelFor(event: BrowserRuntimeEvent, action: ContextMenuAction): string {
  const flags = event.mediaFlags;
  // A spelling suggestion is labelled with the word it suggests, which only
  // the event knows.
  if (action.startsWith('spellSuggestion:')) {
    const index = Number.parseInt(action.substring('spellSuggestion:'.length), 10);
    const suggestions = event.spellSuggestions;
    return suggestions && index >= 0 && index < suggestions.length
      ? suggestions[index] : '';
  }
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
  // Spelling suggestions carry the word rather than a fixed label, and
  // imageToFile is something this shell runs itself rather than shows. An
  // entry with no label would be a blank row.
  const shown: ContextMenuAction[] = actions.filter(
    (action: ContextMenuAction): boolean => labelFor(event, action).length > 0);
  if (shown.length === 0) {
    send({ command: 'contextMenuDismissed', requestId: requestId });
    return;
  }
  const sheets: SheetInfo[] = shown.map((action: ContextMenuAction): SheetInfo => ({
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
