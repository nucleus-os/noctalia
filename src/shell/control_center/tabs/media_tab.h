#pragma once

#include "core/timer_manager.h"
#include "dbus/mpris/mpris_service.h"
#include "shell/control_center/tab.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

class Button;
class HttpClient;
class Image;
class Label;
class MprisService;
class PipeWireSpectrum;
class RenderContext;
class ScrollView;
class Slider;
class AudioVisualizer;
class ConfigService;
class WaylandConnection;

class MediaTab : public Tab {
public:
  MediaTab(
      MprisService* mpris, HttpClient* httpClient, PipeWireSpectrum* spectrum, ConfigService* config,
      WaylandConnection* wayland, RenderContext* renderContext
  );
  ~MediaTab() override;

  std::unique_ptr<Flex> create() override;
  void onFrameTick(float deltaMs) override;
  void setActive(bool active) override;
  void onClose() override;

private:
  // One card per MPRIS player. Holds its scene nodes plus a little runtime state
  // to keep the scrubber from snapping back while a seek settles. Positions are
  // projected live by MprisService::listPlayers(), so no anti-jitter bookkeeping
  // is needed here.
  struct PlayerCard {
    std::string busName;
    Flex* card = nullptr;
    Flex* artworkRow = nullptr;
    Image* artwork = nullptr;
    Label* source = nullptr;
    Label* title = nullptr;
    Label* artist = nullptr;
    Label* album = nullptr;
    Slider* progress = nullptr;
    Button* prevButton = nullptr;
    Button* playPauseButton = nullptr;
    Button* nextButton = nullptr;
    Button* repeatButton = nullptr;
    Button* shuffleButton = nullptr;
    Flex* vizRow = nullptr;
    AudioVisualizer* visualizer = nullptr;

    bool playing = false;
    std::string lastArtUrl;
    std::int64_t lastTrackLengthUs = 0;
    std::int64_t pendingSeekUs = -1;
    std::chrono::steady_clock::time_point pendingSeekUntil;
    std::chrono::steady_clock::time_point progressSettleUntil;
    bool syncingProgress = false;
  };

  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void refresh(Renderer& renderer);
  void rebuildCards(const std::vector<MprisPlayerInfo>& players);
  [[nodiscard]] std::unique_ptr<Flex> buildCard(PlayerCard& card, float scale);
  void applyCardMetrics();
  void feedVisualizers();
  void syncCard(Renderer& renderer, PlayerCard& card, const MprisPlayerInfo& player);
  [[nodiscard]] PlayerCard* cardForBus(const std::string& busName);
  void commitSeek(PlayerCard& card, double valueSeconds);

  // Guard token for deferred callbacks that run on the next main-loop tick.
  std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);

  MprisService* m_mpris = nullptr;
  HttpClient* m_httpClient = nullptr;
  PipeWireSpectrum* m_spectrum = nullptr;
  ConfigService* m_config = nullptr;
  WaylandConnection* m_wayland = nullptr;
  RenderContext* m_renderContext = nullptr;
  std::uint64_t m_spectrumListenerId = 0;
  bool m_active = false;

  Flex* m_rootLayout = nullptr;
  ScrollView* m_mediaScroll = nullptr;
  Flex* m_cardList = nullptr;
  Label* m_emptyLabel = nullptr;

  std::vector<std::unique_ptr<PlayerCard>> m_cards;
  std::string m_structureKey;
  float m_lastListWidth = -1.0f;
  std::unordered_set<std::string> m_pendingArtDownloads;
  Timer m_progressTimer;
};
