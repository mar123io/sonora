#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sonora::platform {

// What a second launch hands to the first one: its command-line arguments,
// UTF-8, exactly as CommandLineArguments() would have given them.
using ActivationFn = std::function<void(const std::vector<std::string>& arguments)>;

// Makes this process the only one.
//
// The protocol has two halves and one rule. The first process to ask holds the
// claim and receives an ActivationFn call for every later launch. A later
// process is told it did not get the claim, hands its arguments over, and
// exits -- which is what makes clicking a sonora:// link twice raise the
// window that is already open rather than start a second player fighting the
// first one for the sound card.
//
// The rule: **the activation arrives on whatever thread the platform delivers
// it on.** On Windows that is the thread running the message loop, which is
// also the UI thread, so the shell could get away with assuming it. It does not
// assume it -- the shell posts the work to the UI thread anyway -- because that
// assumption is exactly the kind that is true on the platform it was written on
// and false on the next one.
class SingleInstance {
 public:
  virtual ~SingleInstance() = default;

  SingleInstance(const SingleInstance&) = delete;
  SingleInstance& operator=(const SingleInstance&) = delete;

  // True when this process holds the claim.
  [[nodiscard]] virtual bool IsPrimary() const noexcept = 0;

  // Hands these arguments to the process that does hold it. Only meaningful
  // when IsPrimary() is false; returns false when nobody was listening -- which
  // happens when the first instance exited between the claim failing and this
  // call, and which the caller answers by carrying on as a normal launch.
  [[nodiscard]] virtual bool ForwardToPrimary(const std::vector<std::string>& arguments) = 0;

 protected:
  SingleInstance() = default;
};

// `id` must be stable across releases and unique to the application: it is the
// name of an operating-system object other processes look up. Never null.
[[nodiscard]] std::unique_ptr<SingleInstance> ClaimSingleInstance(const std::string& id,
                                                                  ActivationFn on_activation);

}  // namespace sonora::platform
