#ifndef CHROME_BROWSER_UI_OHOS_SHELL_ACCESSIBILITY_OHOS_H_
#define CHROME_BROWSER_UI_OHOS_SHELL_ACCESSIBILITY_OHOS_H_

#include "ui/gfx/native_ui_types.h"

namespace content {
class WebContents;
}

namespace chrome::ohos {

void UpdateShellAccessibility(gfx::AcceleratedWidget widget,
                              content::WebContents* contents);

}

#endif
