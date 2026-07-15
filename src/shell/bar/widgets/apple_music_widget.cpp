#include "shell/bar/widgets/apple_music_widget.h"

#include "dbus/mpris/mpris_service.h"

AppleMusicWidget::AppleMusicWidget(
    MprisService* mpris, HttpClient* httpClient, wl_output* output, float maxWidth, float minWidth, float artSize,
    MediaTitleScrollMode titleScrollMode, bool hideWhenNoMedia, bool albumArtOnly, bool hideAlbumArt, bool hideArtist
)
    : MediaWidget(
          mpris, httpClient, output, maxWidth, minWidth, artSize, titleScrollMode, hideWhenNoMedia, albumArtOnly,
          hideAlbumArt, hideArtist, currentProcessChromiumMprisBusName(), "apple-music", ""
      ) {}
