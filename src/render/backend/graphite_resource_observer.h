#pragma once

#include <algorithm>
#include <vector>

class GraphiteTextureManagerObserver {
public:
  virtual ~GraphiteTextureManagerObserver() = default;
  virtual void onGraphiteTextureManagerDestroying() noexcept = 0;
};

class GraphiteResourceObserverRegistry {
public:
  void add(GraphiteTextureManagerObserver& observer) {
    if (std::ranges::find(m_observers, &observer) == m_observers.end()) {
      m_observers.push_back(&observer);
    }
  }

  void remove(GraphiteTextureManagerObserver& observer) noexcept { std::erase(m_observers, &observer); }

  void notifyDestroying() noexcept {
    const auto observers = m_observers;
    m_observers.clear();
    for (auto* observer : observers) {
      if (observer != nullptr) {
        observer->onGraphiteTextureManagerDestroying();
      }
    }
  }

private:
  std::vector<GraphiteTextureManagerObserver*> m_observers;
};
