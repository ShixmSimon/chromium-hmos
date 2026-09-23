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
 * PC UI主页面（用于平板和电脑）
 */
@Entry
@ComponentV2
struct PCIndex {
  build() {
    Column() {
      // PC使用全功能浏览器布局，由Native (Skia) 渲染标签页、工具栏等
      WebWindow({
        url: CHROMIUM_HOME_URL
      })
        .width('100%')
        .height('100%')
    }
    .width('100%')
    .height('100%')
  }
}
