#pragma once

#include "shell/bar/widgets/media_widget.h"

class HttpClient;
class MprisService;
struct wl_output;

class AppleMusicWidget final : public MediaWidget {
public:
  AppleMusicWidget(
      MprisService* mpris, HttpClient* httpClient, wl_output* output, float maxWidth, float minWidth, float artSize,
      MediaTitleScrollMode titleScrollMode, bool hideWhenNoMedia, bool albumArtOnly, bool hideAlbumArt, bool hideArtist
  );
};
