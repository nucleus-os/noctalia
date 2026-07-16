#include "render/backend/graphite_resource_observer.h"

namespace {
  class Observer final : public GraphiteTextureManagerObserver {
  public:
    void onGraphiteTextureManagerDestroying() noexcept override { ++notifications; }
    int notifications = 0;
  };
} // namespace

int main() {
  GraphiteResourceObserverRegistry registry;
  Observer retained;
  Observer removed;

  registry.add(retained);
  registry.add(retained);
  registry.add(removed);
  registry.remove(removed);
  registry.notifyDestroying();

  if (retained.notifications != 1 || removed.notifications != 0) {
    return 1;
  }
  registry.notifyDestroying();
  return retained.notifications == 1 ? 0 : 1;
}
