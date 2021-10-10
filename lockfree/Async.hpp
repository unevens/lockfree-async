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
#include <vector>

namespace lockfree {

class AsyncThread;

namespace detail {

/**
 * An interface abstracting over the different template specialization of Async objects.
 */
class AsyncInterface
{
  friend class ::lockfree::AsyncThread;

public:
  virtual ~AsyncInterface() = default;

protected:
  virtual void timerCallback() = 0;
  class AsyncThread* asyncThread{ nullptr };
  void setAsyncThread(AsyncThread* asyncThread_)
  {
    asyncThread = asyncThread_;
  }
  std::mutex& getMutex();
};

} // namespace detail

/**
 * The AsyncThread class manages a thread that will perform asynchronously any change submitted to an Async object
 * attached to it. An Async object needs to be attached to an AsyncThread for it to receive any submitted change.
 */
class AsyncThread final
{
  using AsyncInterface = detail::AsyncInterface;
  friend AsyncInterface;

public:
  /**
   * Constructor
   * @period the period in milliseconds with which the thread that receives and handles any change submitted to the
   * objects attached to it.
   */
  explicit AsyncThread(int timerPeriod = 250)
    : timerPeriod{ timerPeriod }
  {}

  /**
   * Attaches an Async object from the thread
   * @asyncObject the object to attach to the thread
   */
  void attachObject(AsyncInterface& asyncObject)
  {
    auto const lock = std::lock_guard<std::mutex>(mutex);
    asyncObjects.insert(&asyncObject);
    asyncObject.setAsyncThread(this);
  }

  /**
   * Detaches an Async object from the thread
   * @asyncObject the object to detach to the thread
   */
  void detachObject(AsyncInterface& asyncObject)
  {
    auto const lock = std::lock_guard<std::mutex>(mutex);
    asyncObjects.erase(&asyncObject);
    asyncObject.setAsyncThread(nullptr);
  }

  /**
   * Starts the thread.
   */
  void start()
  {
    if (isRunningFlag.load(std::memory_order_acquire)) {
      return;
    }
    stopTimerFlag.store(false, std::memory_order_release);
    isRunningFlag.store(true, std::memory_order_release);
    timer = std::thread([this]() {
      while (true) {
        for (auto asyncObject : asyncObjects)
          asyncObject->timerCallback();
        if (stopTimerFlag.load(std::memory_order_acquire)) {
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(timerPeriod));
        if (stopTimerFlag.load(std::memory_order_acquire)) {
          return;
        }
      }
    });
  }

  /**
   * Stops the thread.
   */
  void stop()
  {
    stopTimerFlag.store(true, std::memory_order_release);
    if (timer.joinable()) {
      timer.join();
    }
    isRunningFlag.store(false, std::memory_order_release);
  }

  /**
   * Sets the period in milliseconds with which the thread that receives and handles any change submitted to the objects
   * attached to it.
   * @period the period to set
   */
  void setUpdatePeriod(int period)
  {
    timerPeriod.store(period, std::memory_order_release);
  }

  /**
   * @return the period in milliseconds with which the thread that receives and handles any change submitted to the
   * objects attached to it.
   */
  int getUpdatePeriod() const
  {
    return timerPeriod.load(std::memory_order_acquire);
  }

  /**
   * @return true if the thread is running, false otherwise.
   */
  bool isRunning() const
  {
    return isRunningFlag.load(std::memory_order_acquire);
  }

  /**
   * Destructor. It stops the thread if it is active and detach any Async object that was using it
   */
  ~AsyncThread()
  {
    stop();
    for (auto asyncObject : asyncObjects)
      asyncObject->setAsyncThread(nullptr);
  }

private:
  std::unordered_set<AsyncInterface*> asyncObjects;
  std::thread timer;
  std::atomic<bool> stopTimerFlag{ false };
  std::atomic<int> timerPeriod;
  std::atomic<bool> isRunningFlag{ false };
  std::mutex mutex;
};

/**
 * Let's say you have some realtime threads, an each of them wants an instance of an object; and sometimes you need to
 * perform some changes to that object that needs to be done asynchronously and propagated to all the instances. The
 * Async class handles this scenario. The key idea is that the Object is constructable from some ObjectSettings, and you
 * can submit a change to those object settings from any thread using a stdext::inplace_function<void(ObjectSettings&)>.
 * Any thread that wants an instance of the object can request an InstanceAccess which will hold a copy of the Object
 * constructed from the ObjectSettings, and can receive the result of any changes submitted. The changes and the
 * construction of the objects happen in an AsyncThread.
 */
template<class TObject, class TObjectSettings, size_t ChangeFunctorClosureCapacity = 32>
class Async final : public detail::AsyncInterface
{
public:
  using ObjectSettings = TObjectSettings;
  using Object = TObject;
  using ChangeSettings = stdext::inplace_function<void(ObjectSettings&), ChangeFunctorClosureCapacity>;

private:
  struct Instance final
  {
    std::unique_ptr<Object> object;
    Messenger<std::unique_ptr<Object>> toInstance;
    Messenger<std::unique_ptr<Object>> fromInstance;

    explicit Instance(ObjectSettings& objectSettings)
      : object{ std::make_unique<Object>(objectSettings) }
    {}
  };

public:
  /**
   * A class that gives access to an instance of the async object.
   */
  class InstanceAccess final
  {
    template<class TObject_, class TObjectSettings_, size_t ChangeFunctorClosureCapacity_>
    friend class Async;

  public:
    /**
     * Updates the instance to the last change submitted to the Async object. Lockfree.
     * @return true if any change has been received and the instance has been updated, otherwise false
     */
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

    /**
     * @return a reference to the object instance
     */
    Object& get()
    {
      return *(instance->object);
    }

    /**
     * @return a const reference to the object instance
     */
    Object const& get() const
    {
      return instance->object;
    }

    /**
     * Destructor.
     */
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
  /**
   * @return a unique_ptr holding an InstanceAccess object to access and update an instance of the async object
   */
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

  /**
   * Submit a change to Async object, which will be handled asynchronously by the AsyncThread and received by the
   * instances throught the IstanceAccess::update method.
   * It is not lock-free, as it may allocate an internal node of the lifo stack if there is no one available.
   * @return true if the node was available a no allocation has been made, false otherwise
   */
  bool submitChange(ChangeSettings change)
  {
    return messenger.send(std::move(change));
  }

  /**
   * Submit a change to Async object, which will be handled asynchronously by the AsyncThread and received by the
   * instances throught the IstanceAccess::update method.
   * It does not submit the change if the lifo stack is empty. It is lock-free.
   * @return true if the change was submitted, false if the lifo stack is empty.
   */
  bool submitChangeIfNodeAvailable(ChangeSettings change)
  {
    return messenger.sendIfNodeAvailable(std::move(change));
  }

  /**
   * Preallocates nodes for the lifo stack used to send changes.
   * @numNodesToPreallocate the number of nodes to allcoate
   */
  void preallocateNodes(int numNodesToPreallocate)
  {
    messenger.preallocateNodes(numNodesToPreallocate);
  }

  /**
   * Constructor.
   * @objectSettings the initial settings to build the Async object
   */
  explicit Async(ObjectSettings objectSettings)
    : objectSettings{ std::move(objectSettings) }
  {}

  /**
   * Destructor. If the object was attached to an AsyncThread, it detached it
   */
  ~Async() override
  {
    if (asyncThread) {
      asyncThread->detachObject(*this);
    }
  }

private:
  std::unique_ptr<InstanceAccess> createInstance()
  {
    instances.push_back(std::make_unique<Instance>(objectSettings));
    return std::unique_ptr<InstanceAccess>{ new InstanceAccess{ *this, instances.back().get() } };
  }

  void removeInstance(Instance* instance)
  {
    auto it = std::find_if(
      instances.begin(), instances.end(), [instance](auto& instance_) { return instance_.get() == instance; });
    assert(it != instances.end());
    if (it == instances.end())
      return;
    instances.erase(it);
  }

  bool handleChanges()
  {
    int numChanges = receiveAndHandleMessageStack(messenger, [&](ChangeSettings& change) { change(objectSettings); });
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
        instance->toInstance.send(std::make_unique<Object>(objectSettings));
      }
    }
  }

  std::vector<std::unique_ptr<Instance>> instances;
  Messenger<ChangeSettings> messenger;
  ObjectSettings objectSettings;
};

namespace detail {

inline std::mutex& AsyncInterface::getMutex()
{
  return asyncThread->mutex;
}

} // namespace detail

} // namespace lockfree
