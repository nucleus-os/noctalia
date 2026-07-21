#pragma once

#include "include/cef_frame.h"

// Installs trusted, idempotent main-frame integrations for supported web-panel
// origins. Unknown origins receive no Noctalia CSS or JavaScript.
void installWebPanelSiteIntegration(CefRefPtr<CefFrame> frame);
