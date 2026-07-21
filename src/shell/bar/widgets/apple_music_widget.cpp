#include "shell/bar/widgets/apple_music_widget.h"

#include "cef/cef_browser_session.h"
#include "dbus/mpris/mpris_service.h"

AppleMusicWidget::AppleMusicWidget(
    MprisService* mpris, std::shared_ptr<CefBrowserSession> session, HttpClient* httpClient, wl_output* output,
    float maxWidth, float minWidth, float artSize,
    MediaTitleScrollMode titleScrollMode, bool hideWhenNoMedia, bool albumArtOnly, bool hideAlbumArt, bool hideArtist
)
    : MediaWidget(
          mpris, httpClient, output, maxWidth, minWidth, artSize, titleScrollMode, hideWhenNoMedia, albumArtOnly,
          hideAlbumArt, hideArtist, currentProcessChromiumMprisBusName(), "apple-music", "", true,
          [weak = std::weak_ptr<CefBrowserSession>(session)]() -> std::optional<MediaWidgetState> {
            const auto locked = weak.lock();
            if (locked == nullptr || !locked->mediaState().available) {
              return std::nullopt;
            }
            const CefBrowserMediaState& state = locked->mediaState();
            return MediaWidgetState{
                .playbackStatus = state.playing ? "Playing" : "Paused",
                .title = state.title,
                .artist = state.artist,
                .artUrl = state.artworkUrl,
            };
          }
      ) {}
