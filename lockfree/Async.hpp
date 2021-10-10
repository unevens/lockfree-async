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

/**
 * The functionality in this file is still under development.
 * */

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
        auto const lock = std::lock_guard<std::mutex>(mutex);
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
 * can submit a change to those object settings from any thread using a stdext::inplace_function<void(ObjectSettings&)>
 * through an Async::Producer. Any thread that wants an instance of the object can request an Async::Instance which will
 * hold a copy of the Object constructed from the ObjectSettings, and can receive the result of any changes submitted.
 * The changes and the construction of the objects happen in an AsyncThread.
 */
template<class TObject, class TObjectSettings, size_t ChangeFunctorClosureCapacity = 32>
class Async final : public detail::AsyncInterface
{
public:
  using ObjectSettings = TObjectSettings;
  using Object = TObject;
  using ChangeSettings = stdext::inplace_function<void(ObjectSettings&), ChangeFunctorClosureCapacity>;

  template<class T>
  class AccessToken
  {
    template<class TObject_, class TObjectSettings_, size_t ChangeFunctorClosureCapacity_>
    friend class Async;

  public:
    AccessToken(AccessToken const&) = delete;
    AccessToken& operator=(AccessToken const&) = delete;
    virtual ~AccessToken() = default;

  protected:
    explicit AccessToken(Async& async, T* address)
      : address{ address }
      , async{ async }
    {}

    T* address;
    Async& async;
  };

public:
  /**
   * A class that gives access to an instance of the async object.
   */
  class Instance final
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
      auto messageNode = toInstance.receiveLastNode();
      if (messageNode) {
        auto& newObject = messageNode->get();
        swap(object, newObject);
        fromInstance.send(messageNode);
        return true;
      }
      return false;
    }

    /**
     * @return a reference to the actual object instance
     */
    Object& get()
    {
      return *object;
    }

    /**
     * @return a const reference to the actual object instance
     */
    Object const& get() const
    {
      return *object;
    }

  private:
    explicit Instance(ObjectSettings& objectSettings)
      : object{ std::make_unique<Object>(objectSettings) }
    {}

    std::unique_ptr<Object> object;
    Messenger<std::unique_ptr<Object>> toInstance;
    Messenger<std::unique_ptr<Object>> fromInstance;
  };

  class Producer final
  {
    template<class TObject_, class TObjectSettings_, size_t ChangeFunctorClosureCapacity_>
    friend class Async;

  public:
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

  private:
    bool handleChanges(ObjectSettings& objectSettings)
    {
      int numChanges = receiveAndHandleMessageStack(messenger, [&](ChangeSettings& change) { change(objectSettings); });
      return numChanges > 0;
    }

    Producer() = default;

    Messenger<ChangeSettings> messenger;
  };

  class InstanceAccessToken final : public AccessToken<Instance>
  {
    using AccessToken<Instance>::AccessToken;

  public:
    /**
     * Destructor. Use it by resetting the std::unique_ptr<InstanceAccessToken> to remove the instance form the
     * AsyncObject.
     */
    ~InstanceAccessToken() override
    {
      this->async.removeInstance(*this);
    }
  };

  class ProducerAccessToken final : public AccessToken<Producer>
  {
    using AccessToken<Producer>::AccessToken;

  public:
    /**
     * Destructor. Use it by resetting the std::unique_ptr<ProducerAccessToken> to remove the producer form the
     * AsyncObject.
     */
    ~ProducerAccessToken() override
    {
      this->async.removeProducer(*this);
    }
  };

  /**
   * Creates a new instance of the object and return an access token for it
   * @return an AccessToken to the Instance
   */
  std::unique_ptr<InstanceAccessToken> createInstance()
  {
    auto const lock = std::lock_guard<std::mutex>(mutex);
    instances.push_back(std::unique_ptr<Instance>(new Instance(objectSettings)));
    return std::unique_ptr<InstanceAccessToken>(new InstanceAccessToken{ *this, instances.back().get() });
  }

  /**
   * Gets an instance of the object from an access token
   * @accessToken access token to the instance
   * @return a reference to the instance
   */
  Instance& getInstance(std::unique_ptr<InstanceAccessToken>& accessToken)
  {
    return *(accessToken->address);
  }

  /**
   * Creates a new producer of object changes and return an access token for it
   * @return an AccessToken to the Producer
   */
  std::unique_ptr<ProducerAccessToken> createProducer()
  {
    auto const lock = std::lock_guard<std::mutex>(mutex);
    producers.push_back(std::unique_ptr<Producer>(new Producer));
    return std::unique_ptr<ProducerAccessToken>(new ProducerAccessToken{ *this, producers.back().get() });
  }

  /**
   * Gets a producer of object changes from an access token
   * @accessToken access token to the producer
   * @return a reference to the producer
   */
  Producer& getProducer(std::unique_ptr<ProducerAccessToken>& accessToken)
  {
    return *(accessToken->address);
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
    assert(instances.empty() && producers.empty());
    if (asyncThread) {
      asyncThread->detachObject(*this);
    }
  }

private:
  template<class T>
  void removeFromToken(AccessToken<T>& token, std::vector<std::unique_ptr<T>>& storage)
  {
    auto it =
      std::find_if(storage.begin(), storage.end(), [&](auto& element) { return element.get() == token.address; });
    assert(it != storage.end());
    if (it == storage.end())
      return;
    storage.erase(it);
  }

  void removeInstance(InstanceAccessToken& token)
  {
    auto const lock = std::lock_guard<std::mutex>(mutex);
    removeFromToken(token, instances);
  }

  void removeProducer(ProducerAccessToken& token)
  {
    auto const lock = std::lock_guard<std::mutex>(mutex);
    removeFromToken(token, producers);
  }

  void timerCallback() override
  {
    for (auto& instance : instances) {
      instance->fromInstance.discardAndFreeAllMessages();
    }
    bool anyChange = false;
    for (auto& producer : producers) {
      bool const anyChangeFromProducer = producer->handleChanges(objectSettings);
      if (anyChangeFromProducer) {
        anyChange = true;
      }
    }
    if (anyChange) {
      for (auto& instance : instances) {
        instance->toInstance.discardAndFreeAllMessages();
        instance->toInstance.send(std::make_unique<Object>(objectSettings));
      }
    }
  }

  std::vector<std::unique_ptr<Producer>> producers;
  std::vector<std::unique_ptr<Instance>> instances;
  ObjectSettings objectSettings;
  std::mutex mutex;
};

} // namespace lockfree
