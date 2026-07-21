#include "cef/site_integrations/discord_integration.h"

#include <string>
#include <string_view>

namespace {
constexpr std::string_view kDiscordGlassThemeScript = R"JS(
(() => {
  const id = 'noctalia-discord-glass-theme-v8';
  const install = () => {
    const root = document.documentElement;
    if (!root) return false;
    let style = document.getElementById(id);
    if (!style) {
      style = document.createElement('style');
      style.id = id;
      root.appendChild(style);
    }
    style.textContent = `
      :root, html, body, #app-mount, #app-mount > div {
        background-color: transparent !important;
        background-image: none !important;
      }
      :root,
      #app-mount .theme-dark,
      #app-mount .theme-darker {
        --background-primary: rgba(30, 31, 34, 0.11) !important;
        --background-secondary: rgba(43, 45, 49, 0.18) !important;
        --background-secondary-alt: rgba(35, 36, 40, 0.24) !important;
        --background-tertiary: rgba(30, 31, 34, 0.29) !important;
        --background-accent: rgba(78, 80, 88, 0.42) !important;
        --background-floating: rgba(17, 18, 20, 0.80) !important;
        --background-mobile-primary: rgba(30, 31, 34, 0.20) !important;
        --background-mobile-secondary: rgba(43, 45, 49, 0.24) !important;
        --background-base-lowest: rgba(17, 18, 20, 0.29) !important;
        --background-base-lower: rgba(25, 26, 29, 0.20) !important;
        --background-base-low: rgba(30, 31, 34, 0.13) !important;
        --channel-background-default: rgba(25, 26, 29, 0.20) !important;
        --panel-bg: rgba(25, 26, 29, 0.30) !important;
        --__header-bar-background: rgba(25, 26, 29, 0.18) !important;
        --custom-channel-members-bg: rgba(25, 26, 29, 0.18) !important;
        --background-surface-high: rgba(43, 45, 49, 0.31) !important;
        --background-surface-higher: rgba(49, 51, 56, 0.41) !important;
        --background-surface-highest: rgba(56, 58, 64, 0.51) !important;
        --bg-base-primary: rgba(30, 31, 34, 0.11) !important;
        --bg-base-secondary: rgba(43, 45, 49, 0.18) !important;
        --bg-base-tertiary: rgba(30, 31, 34, 0.29) !important;
        --chat-background: rgba(30, 31, 34, 0.11) !important;
        --chat-background-default: rgba(30, 31, 34, 0.11) !important;
        --channeltextarea-background: rgb(56, 58, 64) !important;
        --modal-background: rgb(30, 31, 34) !important;
        --modal-footer-background: rgb(35, 36, 40) !important;
        --noctalia-user-panel-background: rgb(25, 26, 29) !important;
        --noctalia-tooltip-background: rgba(17, 18, 20, 0.58) !important;
        --noctalia-profile-background: rgba(25, 26, 29, 0.64) !important;
        --noctalia-profile-overlay-background: rgba(17, 18, 20, 0.48) !important;
        --noctalia-editor-background: rgb(56, 58, 64) !important;
        --home-background: transparent !important;
      }
      #app-mount .theme-light {
        --background-primary: rgba(245, 246, 248, 0.13) !important;
        --background-secondary: rgba(232, 234, 238, 0.20) !important;
        --background-secondary-alt: rgba(220, 223, 228, 0.26) !important;
        --background-tertiary: rgba(213, 216, 222, 0.33) !important;
        --background-base-lowest: rgba(225, 228, 233, 0.31) !important;
        --background-base-lower: rgba(235, 237, 241, 0.22) !important;
        --background-base-low: rgba(242, 243, 246, 0.15) !important;
        --channel-background-default: rgba(232, 234, 238, 0.22) !important;
        --panel-bg: rgba(225, 228, 233, 0.33) !important;
        --__header-bar-background: rgba(235, 237, 241, 0.20) !important;
        --custom-channel-members-bg: rgba(232, 234, 238, 0.20) !important;
        --chat-background: rgba(245, 246, 248, 0.13) !important;
        --chat-background-default: rgba(245, 246, 248, 0.13) !important;
        --channeltextarea-background: rgb(225, 228, 233) !important;
        --background-floating: rgba(245, 246, 248, 0.82) !important;
        --modal-background: rgb(245, 246, 248) !important;
        --modal-footer-background: rgb(232, 234, 238) !important;
        --noctalia-user-panel-background: rgb(232, 234, 238) !important;
        --noctalia-tooltip-background: rgba(245, 246, 248, 0.68) !important;
        --noctalia-profile-background: rgba(232, 234, 238, 0.72) !important;
        --noctalia-profile-overlay-background: rgba(245, 246, 248, 0.58) !important;
        --noctalia-editor-background: rgb(225, 228, 233) !important;
      }
      #app-mount nav[aria-label="Servers sidebar"],
      #app-mount section[aria-label="Channel header"],
      #app-mount section[aria-label="User status and settings"] {
        background-image: none !important;
      }
      #app-mount section[aria-label="User status and settings"] {
        background-color: var(--noctalia-user-panel-background) !important;
      }
      #app-mount [role="tooltip"] {
        --background-floating: var(--noctalia-tooltip-background) !important;
        background-color: var(--noctalia-tooltip-background) !important;
        background-image: none !important;
        -webkit-backdrop-filter: blur(18px) saturate(1.18) !important;
        backdrop-filter: blur(18px) saturate(1.18) !important;
        animation: none !important;
        transition: none !important;
      }
      #app-mount div:has(> [role="tooltip"]) {
        opacity: 1 !important;
        transform: none !important;
        animation: none !important;
        transition: none !important;
      }
      #app-mount [class*="user-profile-"] {
        background-color: var(--noctalia-profile-background) !important;
        background-image: none !important;
        -webkit-backdrop-filter: blur(24px) saturate(1.15) !important;
        backdrop-filter: blur(24px) saturate(1.15) !important;
      }
      #app-mount [class*="user-profile-"] [class*="overlay_"] {
        background-color: var(--noctalia-profile-overlay-background) !important;
        background-image: none !important;
      }
      #app-mount [class*="channelTextArea_"],
      #app-mount [class*="scrollableContainer_"] {
        background-color: var(--noctalia-editor-background) !important;
        background-image: none !important;
      }
    `;
    return true;
  };
  if (!install()) {
    new MutationObserver((_, observer) => {
      if (install()) observer.disconnect();
    }).observe(document, {childList: true, subtree: true});
  }
})();
)JS";
}

void installDiscordSiteIntegration(CefRefPtr<CefFrame> frame) {
  if (frame == nullptr || !frame->IsMain()) {
    return;
  }
  frame->ExecuteJavaScript(std::string(kDiscordGlassThemeScript), frame->GetURL(), 0);
}
