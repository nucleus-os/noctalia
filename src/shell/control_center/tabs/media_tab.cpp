#include "shell/control_center/tabs/media_tab.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "net/http_client.h"
#include "pipewire/pipewire_spectrum.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/control_center/tab.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/image.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/slider.h"
#include "ui/visuals/audio_visualizer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace control_center;
using namespace mpris;

namespace {

  // Layout-grid unit for the media tab. Decoupled from control-height tokens so a
  // roomier control row does not inflate artwork/card widths and overflow the
  // panel content area.
  constexpr float kMediaUnit = 36.0f;

  // Compact per-player card: fixed square art on the left, text + waveform +
  // scrubber + transport on the right. Sized so 2-3 sources are visible before
  // scrolling.
  constexpr float kCardArtSize = kMediaUnit * 3.6f;
  constexpr float kCardControlBtn = kMediaUnit * 0.86f;
  constexpr float kCardPlayBtn = kMediaUnit;
  constexpr float kCardVizHeight = kMediaUnit * 0.9f;

  constexpr std::int64_t kSeekArrivedToleranceUs = 1'500'000;
  constexpr auto kProgressSettleHold = std::chrono::milliseconds(2500);
  constexpr auto kPendingSeekTimeout = std::chrono::milliseconds(5000);
  constexpr int kVisualizerBandCount = 32;

  std::string playPauseGlyph(const std::string& playbackStatus) {
    return playbackStatus == "Playing" ? "media-pause" : "media-play";
  }

  std::string repeatGlyph(const std::string& loopStatus) { return loopStatus == "Track" ? "repeat-once" : "repeat"; }

  ButtonVariant toggleVariant(bool active) { return active ? ButtonVariant::Primary : ButtonVariant::Ghost; }

  [[nodiscard]] int cardArtDecodeSize(float scale) { return static_cast<int>(std::round(kCardArtSize * scale)); }

} // namespace

MediaTab::MediaTab(
    MprisService* mpris, HttpClient* httpClient, PipeWireSpectrum* spectrum, ConfigService* config,
    WaylandConnection* wayland, RenderContext* renderContext
)
    : m_mpris(mpris), m_httpClient(httpClient), m_spectrum(spectrum), m_config(config), m_wayland(wayland),
      m_renderContext(renderContext) {}

MediaTab::~MediaTab() { m_aliveGuard.reset(); }

MediaTab::PlayerCard* MediaTab::cardForBus(const std::string& busName) {
  for (auto& card : m_cards) {
    if (card->busName == busName) {
      return card.get();
    }
  }
  return nullptr;
}

std::unique_ptr<Flex> MediaTab::create() {
  const float scale = contentScale();

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });

  auto scroll = ui::scrollView({
      .out = &m_mediaScroll,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0f,
      .viewportPaddingV = 0.0f,
      .flexGrow = 1.0f,
      .configure =
          [](ScrollView& view) {
            view.clearFill();
            view.clearBorder();
          },
  });
  m_cardList = scroll->content();
  m_cardList->setDirection(FlexDirection::Vertical);
  m_cardList->setAlign(FlexAlign::Stretch);
  m_cardList->setGap(Style::spaceMd * scale);
  tab->addChild(std::move(scroll));
  return tab;
}

std::unique_ptr<Flex> MediaTab::buildCard(PlayerCard& card, float scale) {
  auto root = ui::column({
      .out = &card.card,
      .gap = Style::spaceSm * scale,
      .flexGrow = 0.0f,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& node) {
        applySectionCardStyle(node, scale, opacity, borders);
      },
  });

  root->addChild(
      ui::label({
          .out = &card.source,
          .text = "",
          .fontSize = Style::fontSizeCaption * scale,
          .fontWeight = FontWeight::Medium,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );

  auto body = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceMd * scale,
  });

  body->addChild(
      ui::row(
          {.out = &card.artworkRow, .align = FlexAlign::Center, .justify = FlexJustify::Center, .gap = 0.0f},
          ui::image({
              .out = &card.artwork,
              .fit = ImageFit::Cover,
              .radius = Style::scaledRadiusLg(scale),
              .width = kCardArtSize * scale,
              .height = kCardArtSize * scale,
          })
      )
  );

  auto right = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * scale,
      .flexGrow = 1.0f,
  });

  right->addChild(
      ui::label({
          .out = &card.title,
          .text = i18n::tr("control-center.media.nothing-playing"),
          .fontSize = Style::fontSizeTitle * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::Primary),
      })
  );
  right->addChild(
      ui::label({
          .out = &card.artist,
          .text = "",
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );

  auto vizRow = ui::row({
      .out = &card.vizRow,
      .align = FlexAlign::Stretch,
      .justify = FlexJustify::Start,
      .minHeight = kCardVizHeight * scale,
      .fillWidth = true,
      .clipChildren = true,
  });
  auto visualizer = std::make_unique<AudioVisualizer>();
  visualizer->setGradient(colorForRole(ColorRole::Primary), colorForRole(ColorRole::Secondary));
  visualizer->setOrientation(AudioSpectrumOrientation::Horizontal);
  visualizer->setMirrored(true);
  visualizer->setCentered(true);
  visualizer->setValues(std::vector<float>(kVisualizerBandCount, 0.0f));
  visualizer->tick(0.0f);
  visualizer->setFlexGrow(1.0f);
  card.visualizer = visualizer.get();
  vizRow->addChild(std::move(visualizer));
  right->addChild(std::move(vizRow));

  const std::string busName = card.busName;
  right->addChild(
      ui::slider({
          .out = &card.progress,
          .minValue = 0.0f,
          .maxValue = 100.0f,
          .step = 1.0f,
          .trackHeight = 6.0f * scale,
          .thumbSize = 14.0f * scale,
          .controlHeight = (Style::controlHeight * 0.7f) * scale,
          .onValueChanged =
              [this, busName](double value) {
                auto* c = cardForBus(busName);
                if (c == nullptr || c->syncingProgress) {
                  return;
                }
                const auto now = std::chrono::steady_clock::now();
                c->pendingSeekUs = static_cast<std::int64_t>(std::llround(value * 1000000.0));
                c->pendingSeekUntil = now + kPendingSeekTimeout;
                c->progressSettleUntil = now + kProgressSettleHold;
              },
          .onDragEnd =
              [this, busName]() {
                auto* c = cardForBus(busName);
                if (c == nullptr || c->syncingProgress || c->progress == nullptr) {
                  return;
                }
                commitSeek(*c, c->progress->value());
              },
      })
  );

  auto controls = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
  });

  auto makeControl = [&](Button** out, const char* glyph, ButtonVariant variant, float side, auto onClick) {
    controls->addChild(
        ui::button({
            .out = out,
            .glyph = glyph,
            .variant = variant,
            .minWidth = side * scale,
            .minHeight = side * scale,
            .padding = Style::spaceXs * scale,
            .radius = Style::scaledRadiusMd(scale),
            .onClick = std::move(onClick),
        })
    );
  };

  auto deferred = [this, busName](auto fn) {
    const std::weak_ptr<void> guard = m_aliveGuard;
    DeferredCall::callLater([this, guard, busName, fn]() {
      if (guard.expired() || m_mpris == nullptr) {
        return;
      }
      fn(busName);
      PanelManager::instance().refresh();
    });
  };

  makeControl(&card.repeatButton, "repeat", ButtonVariant::Ghost, kCardControlBtn, [this, deferred]() {
    deferred([this](const std::string& bus) {
      const auto current = m_mpris->loopStatus(bus).value_or("None");
      const std::string next = current == "None" ? "Playlist" : (current == "Playlist" ? "Track" : "None");
      (void)m_mpris->setLoopStatus(bus, next);
    });
  });
  makeControl(&card.prevButton, "media-prev", ButtonVariant::Ghost, kCardControlBtn, [this, deferred]() {
    deferred([this](const std::string& bus) { (void)m_mpris->previous(bus); });
  });
  makeControl(&card.playPauseButton, "media-play", ButtonVariant::Primary, kCardPlayBtn, [this, deferred]() {
    deferred([this](const std::string& bus) { (void)m_mpris->playPause(bus); });
  });
  makeControl(&card.nextButton, "media-next", ButtonVariant::Ghost, kCardControlBtn, [this, deferred]() {
    deferred([this](const std::string& bus) { (void)m_mpris->next(bus); });
  });
  makeControl(&card.shuffleButton, "shuffle", ButtonVariant::Ghost, kCardControlBtn, [this, deferred]() {
    deferred([this](const std::string& bus) {
      const bool enabled = m_mpris->shuffle(bus).value_or(false);
      (void)m_mpris->setShuffle(bus, !enabled);
    });
  });

  right->addChild(std::move(controls));
  body->addChild(std::move(right));
  root->addChild(std::move(body));
  return root;
}

void MediaTab::rebuildCards(const std::vector<MprisPlayerInfo>& players) {
  if (m_cardList == nullptr) {
    return;
  }
  std::string nextKey;
  for (const auto& player : players) {
    nextKey += player.busName;
    nextKey += '\n';
  }
  if (nextKey == m_structureKey && (players.empty() == (m_emptyLabel != nullptr))) {
    return;
  }
  m_structureKey = nextKey;
  m_lastListWidth = -1.0f;

  m_cards.clear();
  m_emptyLabel = nullptr;
  while (!m_cardList->children().empty()) {
    m_cardList->removeChild(m_cardList->children().front().get());
  }

  const float scale = contentScale();

  if (players.empty()) {
    auto emptyCard = ui::column({
        .align = FlexAlign::Center,
        .justify = FlexJustify::Center,
        .gap = Style::spaceXs * scale,
        .flexGrow = 1.0f,
        .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& node) {
          applySectionCardStyle(node, scale, opacity, borders);
        },
    });
    emptyCard->addChild(
        ui::label({
            .out = &m_emptyLabel,
            .text = i18n::tr("control-center.media.nothing-playing"),
            .fontSize = Style::fontSizeTitle * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::Primary),
        })
    );
    emptyCard->addChild(
        ui::label({
            .text = i18n::tr("control-center.media.start-playback"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    m_cardList->addChild(std::move(emptyCard));
    return;
  }

  for (const auto& player : players) {
    auto card = std::make_unique<PlayerCard>();
    card->busName = player.busName;
    auto node = buildCard(*card, scale);
    m_cardList->addChild(std::move(node));
    m_cards.push_back(std::move(card));
  }
}

void MediaTab::applyCardMetrics() {
  if (m_mediaScroll == nullptr) {
    return;
  }
  const float scale = contentScale();
  const float viewport = m_mediaScroll->contentViewportWidth();
  if (viewport <= 0.0f || viewport == m_lastListWidth) {
    return;
  }
  m_lastListWidth = viewport;

  for (auto& card : m_cards) {
    if (card->card == nullptr) {
      continue;
    }
    const float cardInner = std::max(1.0f, viewport - (card->card->paddingLeft() + card->card->paddingRight()));
    const float textWidth = std::max(1.0f, cardInner - kCardArtSize * scale - Style::spaceMd * scale);
    if (card->source != nullptr) {
      card->source->setMaxWidth(textWidth);
    }
    if (card->title != nullptr) {
      card->title->setMaxWidth(textWidth);
    }
    if (card->artist != nullptr) {
      card->artist->setMaxWidth(textWidth);
    }
    if (card->progress != nullptr) {
      card->progress->setSize(textWidth, 0.0f);
    }
    if (card->visualizer != nullptr) {
      card->visualizer->setSize(textWidth, kCardVizHeight * scale);
    }
  }
}

void MediaTab::syncCard(Renderer& renderer, PlayerCard& card, const MprisPlayerInfo& player) {
  if (card.source == nullptr || card.title == nullptr || card.artist == nullptr || card.progress == nullptr) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();

  card.source->setText(player.identity.empty() ? player.busName : player.identity);
  card.title->setText(player.title.empty() ? player.identity : player.title);
  const std::string artists = joinArtists(player.artists);
  card.artist->setText(artists);
  card.artist->setVisible(!artists.empty());

  // Artwork.
  const std::string resolvedArtUrl = effectiveArtUrl(player);
  const std::string artPath = resolveArtworkSource(
      m_httpClient, m_pendingArtDownloads, resolvedArtUrl,
      [this] {
        for (auto& c : m_cards) {
          c->lastArtUrl.clear();
        }
        PanelManager::instance().refresh();
      },
      m_aliveGuard
  );
  if (card.artwork != nullptr) {
    if (!resolvedArtUrl.empty() && (resolvedArtUrl != card.lastArtUrl || !card.artwork->hasImage())) {
      bool loaded = false;
      if (artPath.empty()) {
        card.artwork->clear(renderer);
      } else if (!card.artwork->setSourceFile(renderer, artPath, cardArtDecodeSize(contentScale()), true, true)) {
        card.artwork->clear(renderer);
      } else {
        loaded = true;
      }
      card.lastArtUrl = loaded ? resolvedArtUrl : std::string{};
      if (loaded) {
        PanelManager::instance().requestLayout();
      }
    } else if (resolvedArtUrl.empty()) {
      card.artwork->clear(renderer);
      card.lastArtUrl.clear();
    }
  }

  // Progress / scrubber. Positions are projected live by MprisService, so we only
  // hold the displayed value briefly after a seek to avoid a snap-back.
  std::int64_t lengthUs = player.lengthUs > 0 ? player.lengthUs : card.lastTrackLengthUs;
  if (player.lengthUs > 0) {
    card.lastTrackLengthUs = player.lengthUs;
  }
  std::int64_t liveUs = lengthUs > 0 ? std::clamp<std::int64_t>(player.positionUs, 0, lengthUs)
                                     : std::max<std::int64_t>(0, player.positionUs);

  const bool seekArrived =
      card.pendingSeekUs >= 0 && std::llabs(liveUs - card.pendingSeekUs) <= kSeekArrivedToleranceUs;
  const bool seekExpired = card.pendingSeekUs >= 0 && now >= card.pendingSeekUntil;
  const bool seekPending = card.pendingSeekUs >= 0 && !seekArrived && !seekExpired;
  const bool withinSettle = now < card.progressSettleUntil;

  std::int64_t displayUs = liveUs;
  if (seekPending || (withinSettle && card.pendingSeekUs >= 0)) {
    displayUs = card.pendingSeekUs;
  }
  if (seekArrived || seekExpired) {
    card.pendingSeekUs = -1;
  }

  const bool progressInteracting = card.progress->dragging() || seekPending || withinSettle;
  const bool progressEnabled = player.canSeek && (lengthUs > 0 || progressInteracting);

  card.syncingProgress = true;
  card.progress->setEnabled(progressEnabled);
  if (lengthUs > 0) {
    card.progress->setRange(0.0, static_cast<double>(lengthUs) / 1000000.0);
  }
  if (!card.progress->dragging()) {
    const double sliderMax = card.progress->maxValue();
    const double nextValue =
        sliderMax > 0.0 ? std::clamp(static_cast<double>(displayUs) / 1000000.0, 0.0, sliderMax) : 0.0;
    card.progress->setValue(nextValue);
  }
  card.syncingProgress = false;

  card.playing = player.playbackStatus == "Playing";

  // Transport.
  if (card.playPauseButton != nullptr) {
    card.playPauseButton->setGlyph(playPauseGlyph(player.playbackStatus));
  }
  if (card.prevButton != nullptr) {
    card.prevButton->setEnabled(player.canGoPrevious);
  }
  if (card.nextButton != nullptr) {
    card.nextButton->setEnabled(player.canGoNext);
  }
  if (card.repeatButton != nullptr) {
    card.repeatButton->setGlyph(repeatGlyph(player.loopStatus));
    card.repeatButton->setVariant(toggleVariant(player.loopStatus != "None"));
  }
  if (card.shuffleButton != nullptr) {
    card.shuffleButton->setVariant(toggleVariant(player.shuffle));
  }
}

void MediaTab::commitSeek(PlayerCard& card, double valueSeconds) {
  if (m_mpris == nullptr) {
    return;
  }
  const auto targetUs = static_cast<std::int64_t>(std::llround(valueSeconds * 1000000.0));
  const auto now = std::chrono::steady_clock::now();
  card.pendingSeekUs = targetUs;
  card.pendingSeekUntil = now + kPendingSeekTimeout;
  card.progressSettleUntil = now + kProgressSettleHold;

  const std::weak_ptr<void> guard = m_aliveGuard;
  const std::string bus = card.busName;
  DeferredCall::callLater([this, guard, bus, targetUs]() {
    if (guard.expired() || m_mpris == nullptr) {
      return;
    }
    (void)m_mpris->setPosition(bus, targetUs);
    PanelManager::instance().refresh();
  });
}

void MediaTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_rootLayout == nullptr) {
    return;
  }
  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
  refresh(renderer);
  applyCardMetrics();
  m_rootLayout->layout(renderer);
}

void MediaTab::doUpdate(Renderer& renderer) {
  if (!m_active) {
    m_progressTimer.stop();
    return;
  }

  refresh(renderer);
  applyCardMetrics();
  feedVisualizers();

  bool anyPlaying = false;
  for (const auto& card : m_cards) {
    if (card->playing) {
      anyPlaying = true;
      break;
    }
  }
  if (anyPlaying) {
    if (!m_progressTimer.active()) {
      m_progressTimer.startRepeating(std::chrono::milliseconds(1000), [this]() {
        if (!m_active) {
          return;
        }
        PanelManager::instance().requestUpdateOnly();
        PanelManager::instance().requestRedraw();
      });
    }
  } else {
    m_progressTimer.stop();
  }
}

void MediaTab::feedVisualizers() {
  const bool haveSpectrum = m_spectrum != nullptr && m_spectrumListenerId != 0 && !m_spectrum->idle();
  const std::vector<float> silence(kVisualizerBandCount, 0.0f);
  for (auto& card : m_cards) {
    if (card->visualizer == nullptr) {
      continue;
    }
    // The spectrum is the mixed system output rather than a per-player capture,
    // so only the playing cards animate; the rest flatten.
    if (card->playing && haveSpectrum) {
      card->visualizer->setValues(m_spectrum->values(m_spectrumListenerId));
    } else {
      card->visualizer->setValues(silence);
    }
  }
}

void MediaTab::refresh(Renderer& renderer) {
  const auto players = m_mpris != nullptr ? m_mpris->listPlayers() : std::vector<MprisPlayerInfo>{};
  rebuildCards(players);
  for (const auto& player : players) {
    if (auto* card = cardForBus(player.busName)) {
      syncCard(renderer, *card, player);
    }
  }
}

void MediaTab::onFrameTick(float deltaMs) {
  if (!m_active) {
    return;
  }
  feedVisualizers();
  for (auto& card : m_cards) {
    if (card->visualizer != nullptr) {
      card->visualizer->tick(deltaMs);
    }
  }
}

void MediaTab::setActive(bool active) {
  m_active = active;
  if (m_spectrum != nullptr) {
    if (active && m_spectrumListenerId == 0) {
      m_spectrumListenerId = m_spectrum->addChangeListener(kVisualizerBandCount, [this]() {
        if (!m_active || m_spectrum->idle()) {
          return;
        }
        PanelManager::instance().requestFrameTick();
      });
    } else if (!active && m_spectrumListenerId != 0) {
      m_spectrum->removeChangeListener(m_spectrumListenerId);
      m_spectrumListenerId = 0;
    }
  }
  if (!active) {
    m_progressTimer.stop();
  }
}

void MediaTab::onClose() {
  m_progressTimer.stop();
  if (m_spectrum != nullptr && m_spectrumListenerId != 0) {
    m_spectrum->removeChangeListener(m_spectrumListenerId);
    m_spectrumListenerId = 0;
  }
  m_active = false;
  m_rootLayout = nullptr;
  m_mediaScroll = nullptr;
  m_cardList = nullptr;
  m_emptyLabel = nullptr;
  m_cards.clear();
  m_structureKey.clear();
  m_lastListWidth = -1.0f;
  m_pendingArtDownloads.clear();
}
