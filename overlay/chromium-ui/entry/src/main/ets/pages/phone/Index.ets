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

/**
 * 手机UI主页面
 */
@Entry
@ComponentV2
struct PhoneIndex {
  @Local url: string = CHROMIUM_HOME_URL;
  @Local pageTitle: string = '';
  @Local isLoading: boolean = false;
  
  build() {
    Column() {
      // 手机使用原Chromium布局，直接加载WebWindow并填充全屏
      // 这里的WebWindow由Native (Skia) 渲染，包含原生的Omnibox和Menu
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
    }
    .width('100%')
    .height('100%')
  }
}

