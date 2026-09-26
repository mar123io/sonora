#include <sonora/platform/single_instance.h>

#include <utility>

// Single instance, where there is no mechanism to enforce it.
//
// Every process is told it is the primary one, which is the safe answer rather
// than the convenient one: a program wrongly told it is a *second* instance
// would hand its arguments to nobody and exit, and the user would see nothing
// happen at all. Told it is the first, the worst case is two copies running --
// visible, and fixable by closing one.

namespace sonora::platform {
namespace {

class NoSingleInstance final : public SingleInstance {
 public:
  explicit NoSingleInstance(ActivationFn on_activation)
      : on_activation_(std::move(on_activation)) {
    // Kept so the callback is not silently dropped by a later refactor that
    // gives this class a real implementation.
    (void)on_activation_;
  }

  [[nodiscard]] bool IsPrimary() const noexcept override { return true; }

  [[nodiscard]] bool ForwardToPrimary(const std::vector<std::string>& arguments) override {
    (void)arguments;
    return false;
  }

 private:
  ActivationFn on_activation_;
};

}  // namespace

std::unique_ptr<SingleInstance> ClaimSingleInstance(const std::string& id,
                                                    ActivationFn on_activation) {
  (void)id;
  return std::make_unique<NoSingleInstance>(std::move(on_activation));
}

}  // namespace sonora::platform
