/*
Copyright 2021 Dario Mambro

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#pragma once
#include "Messenger.hpp"
#include "inplace_function.h"
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>

namespace lockfree {

class AsyncInterface
{
  friend class AsyncThread;

public:
  virtual ~AsyncInterface() = default;

protected:
  virtual void timerCallback() = 0;
  class AsyncThread* asyncThread{ nullptr };
  void setAsyncThread(AsyncThread* asyncThread_) { asyncThread = asyncThread_; }
  std::mutex& getMutex();
};

class AsyncThread final
{
  friend AsyncInterface;

public:
  explicit AsyncThread(int timerPeriod = 250)
    : timerPeriod{ timerPeriod }
  {}

  void addObject(AsyncInterface& async)
  {
    auto const lock = std::lock_guard<std::mutex>(mutex);
    asyncObjects.insert(&async);
    async.setAsyncThread(this);
  }

  void removeObject(AsyncInterface& async)
  {
    auto const lock = std::lock_guard<std::mutex>(mutex);
    asyncObjects.erase(&async);
    async.setAsyncThread(nullptr);
  }

  void start()
  {
    auto const lock = std::lock_guard<std::mutex>(mutex);
    if (isRunningFlag) {
      return;
    }
    stopTimerFlag = false;
    timer = std::thread([this]() {
      while (true) {
        for (auto asyncObject : asyncObjects)
          asyncObject->timerCallback();
        if (stopTimerFlag) {
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(timerPeriod));
        if (stopTimerFlag) {
          return;
        }
      }
    });
  }

  void stop()
  {
    auto const lock = std::lock_guard<std::mutex>(mutex);
    stopTimerFlag = true;
    if (timer.joinable()) {
      timer.join();
    }
    isRunningFlag = false;
  }

  /**
   * Sets the period of the timer thread that handles the messages.
   */
  void setTimerPeriod(int period)
  {
    auto const lock = std::lock_guard<std::mutex>(mutex);
    timerPeriod.store(period, std::memory_order_release);
  }

  /**
   * @return true if the timer thread is running, false otherwise.
   */
  bool isRunning() const
  {
    return isRunningFlag.load(std::memory_order_acquire);
  }

  int getTimerPeriod() const
  {
    return timerPeriod.load(std::memory_order_acquire);
  }

  ~AsyncThread()
  {
    stop();
    for (auto asyncObject : asyncObjects)
      removeObject(*asyncObject);
  }

private:
  std::unordered_set<AsyncInterface*> asyncObjects;
  std::thread timer;
  std::atomic<bool> stopTimerFlag{ false };
  std::atomic<int> timerPeriod;
  std::atomic<bool> isRunningFlag{ false };
  std::mutex mutex;
};

inline std::mutex&
AsyncInterface::getMutex()
{
  return asyncThread->mutex;
}

template<class TObject,
         class TObjectSettings,
         size_t ChangeFunctorClosureCapacity = 32>
class Async final : public AsyncInterface
{
public:
  using ObjectSettings = TObjectSettings;
  using Object = TObject;
  using ChangeSettings = stdext::inplace_function<void(ObjectSettings&),
                                                  ChangeFunctorClosureCapacity>;

private:
  struct Instance final
  {
    std::unique_ptr<Object> object;
    Messenger<Object> toInstance;
    Messenger<Object> fromInstance;

    explicit Instance(ObjectSettings& objectSettings)
      : object{ objectSettings }
    {}
  };

public:
  class InstanceAccess final
  {
    template<class TObject_, class TObjectSettings_>
    friend class Async;

  public:
    bool update()
    {
      using std::swap;
      auto messageNode = instance->toInstance.receiveLastNode();
      if (messageNode) {
        auto& object = messageNode->get();
        swap(instance->object, object);
        instance->fromInstance.send(messageNode);
        return true;
      }
      return false;
    }

    Object& get() { return instance->object; }

    Object const& get() const { return instance->object; }

    ~InstanceAccess()
    {
      if (async.asyncThread) {
        auto const lock = std::lock_guard<std::mutex>(async.getMutex());
        async.removeInstance(instance);
      }
      else {
        async.removeInstance(instance);
      }
    }

  private:
    explicit InstanceAccess(Async& async, Instance* instance)
      : async{ async }
      , instance{ instance }
    {}

    Async& async;
    Instance* instance;
  };

  friend InstanceAccess;

  std::unique_ptr<InstanceAccess> requestInstance()
  {
    if (asyncThread) {
      auto const lock = std::lock_guard<std::mutex>(getMutex());
      return createInstance();
    }
    else {
      return createInstance();
    }
  }

  bool submitChange(ChangeSettings&& change) { return messenger.send(change); }

  bool submitChangeIfNodeAvailable(ChangeSettings&& change)
  {
    return messenger.sendIfNodeAvailable(change);
  }

  void preallocateNodes(int numNodesToPreallocate)
  {
    messenger.preallocateNodes(numNodesToPreallocate);
  }

  ~Async()
  {
    if (asyncThread) {
      asyncThread->removeObject(*this);
    }
  }

private:
  std::unique_ptr<InstanceAccess> createInstance()
  {
    instances.push_back(std::make_unique<Instance>(objectSettings));
    return std::make_unique<InstanceAccess>(*this, instances.back().get());
  }

  void removeInstance(Instance* instance)
  {
    auto it =
      std::find_if(instances.begin(), instances.end(), [instance](auto& instance_) {
        return instance_.get() == instance;
      });
    assert(it != instances.end());
    if (it == instances.end())
      return;
    instances.erase(it);
  }

  bool handleChanges()
  {
    int numChanges = receiveAndHandleMessageStack(
      messenger, [&](ChangeSettings& change) { change(objectSettings); });
    return numChanges > 0;
  }

  void timerCallback() override
  {
    for (auto& instance : instances) {
      freeMessageStack(instance->fromInstance.receiveAllNodes());
    }
    bool const anyChange = handleChanges();
    if (anyChange) {
      for (auto& instance : instances) {
        freeMessageStack(instance->toInstance.receiveAllNodes());
        instance->toInstance.send(Object{ objectSettings });
      }
    }
  }

  AsyncThread* asyncThread{ nullptr };
  std::vector<std::unique_ptr<Instance>> instances;
  Messenger<ChangeSettings> messenger;
  ObjectSettings objectSettings;
};

} // namespace lockfree
