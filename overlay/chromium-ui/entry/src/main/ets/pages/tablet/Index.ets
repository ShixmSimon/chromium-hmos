/*
 * Copyright (c) 2024 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
import { WebWindow } from 'engine';
import { CHROMIUM_HOME_URL } from 'engine';

import { KeyCode } from '@kit.InputKit';

/**
 * Tablet UI主页面（用于汽车和电视）
 */
@Entry
@ComponentV2
struct TabletIndex {
  @Local url: string = CHROMIUM_HOME_URL;
  @Local pageTitle: string = '';
  @Local isLoading: boolean = false;
  @Local focusedIndex: number = 0;  // 遥控器焦点索引
  
  build() {
    Column() {
      // 大屏使用平板布局，由Native (Skia) 渲染全功能界面
      WebWindow({
        url: this.url,
        onPageLoadStart: (url: string) => {
          this.isLoading = true;
        },
        onPageLoadFinish: (url: string) => {
          this.isLoading = false;
        },
        onTitleReceive: (title: string) => {
          this.pageTitle = title;
        }
      })
        .width('100%')
        .height('100%')
        .focusable(true)
        .onKeyEvent((event: KeyEvent) => {
          this.handleKeyEvent(event);
        })
    }
    .width('100%')
    .height('100%')
  }
  
  /**
   * 处理键盘事件（遥控器/方向盘）
   */
  handleKeyEvent(event: KeyEvent): void {
    if (event.type === KeyType.Down) {
      switch (event.keyCode) {
        case KeyCode.KEYCODE_DPAD_UP:
          // 向上移动焦点
          if (this.focusedIndex > 0) {
            this.focusedIndex--;
          }
          break;
        case KeyCode.KEYCODE_DPAD_DOWN:
          // 向下移动焦点
          if (this.focusedIndex < 1) {
            this.focusedIndex++;
          }
          break;
        case KeyCode.KEYCODE_DPAD_LEFT:
          // 向左
          break;
        case KeyCode.KEYCODE_DPAD_RIGHT:
          // 向右
          break;
        case KeyCode.KEYCODE_ENTER:
          // 确认
          break;
        case KeyCode.KEYCODE_BACK:
          // 返回
          break;
      }
    }
  }
  
  /**
   * 处理遥控器按键
   */
  handleRemoteKey(key: string): void {
    // 处理遥控器按键逻辑
    console.log('Remote key pressed:', key);
  }
}
