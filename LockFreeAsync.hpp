/*
Copyright 2019 Dario Mambro

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
#include "LockFreeMessenger.hpp"
#include <chrono>
#include <thread>

namespace lockfree {

template<class TClassToStore>
class Async;

/**
 * Abstract class for getters. Used internally by the Async class.
 * @tparam TClassToStore the type of the object stored in the Async object.
 */
template<class TClassToStore>
class TGetterInterface
{
  using ClassToStore = TClassToStore;
  friend Async<ClassToStore>;
  virtual void onChange(ClassToStore const& storedObject) = 0;
  virtual void cleanup() = 0;

public:
  virtual ~TGetterInterface() {}
};

/**
 * Abstract class for BlockingGetters. Used internally by the Async class.
 * @tparam TClassToStore the type of the object stored in the Async object.
 */
template<class TClassToStore>
class TBlockingGetterInterface
{
  using ClassToStore = TClassToStore;
  friend Async<ClassToStore>;
  virtual void handleRequests(ClassToStore const& storedObject) = 0;

public:
  virtual ~TBlockingGetterInterface() {}
};

/**
 * A class to get a thread-local copy of the object stored in the Async class,
 * or of an object of an other type constructed from it. Non-blocking version.
 * @tparam TClassToStore the type of the object stored in the Async object.
 * @tparam TClassToGet the type of object to be returned by the method get of
 * the getter. It must be constructible from a constant reference to a
 * TClassToStore object.
 */
template<class TClassToGet, class TClassToStore>
class TGetter final : public TGetterInterface<TClassToStore>
{
  using ClassToStore = TClassToStore;
  using ClassToGet = TClassToGet;
  friend Async<ClassToStore>; // friended for constructor

  ClassToGet localCopy;
  Messenger<ClassToGet> toGetter;
  Messenger<ClassToGet> toAsync;

  void onChange(ClassToStore const& storedObject) override
  {
    freeMessageStack(toGetter.receiveAllNodes()); // free old messages
    toGetter.send(ClassToGet(storedObject));
  }

  void cleanup() override { freeMessageStack(toAsync.receiveAllNodes()); }

  TGetter(ClassToStore const& storedObject)
    : localCopy{ storedObject }
  {}

public:
  /**
   * Updates the local copy to the last version of the object stored by the
   * Async instance, or to a ClassToGet object constructed from it.
   * Non-blocking. Don't call this function while you are using the local copy!
   */
  void update()
  {
    using std::swap;
    auto messageNode = toGetter.receiveLastNode();
    if (messageNode) {
      auto& message = messageNode->get();
      swap(localCopy, message);
      // send the node back with the old ClassToGet to be freed
      toAsync.send(messageNode);
    }
  }
  /**
   * @return a reference to the local storage.
   */
  ClassToGet& get() { return localCopy; }
};

/**
 * A class to get a thread-local copy of the object stored in the Async class,
 * or of an object of an other type constructed from it. Blocking version.
 * @tparam TClassToStore the type of the object stored in the Async object.
 * @tparam TClassToGet the type of object to be returned by the method get of
 * the getter. It must be constructible from a constant reference to a
 * TClassToStore object.
 */
template<class TClassToGet, class TClassToStore>
class TBlockingGetter final : public TBlockingGetterInterface<TClassToStore>
{
  using ClassToStore = TClassToStore;
  using ClassToGet = TClassToGet;
  friend Async<ClassToStore>; // friended for constructor

  ClassToGet localCopy;
  Messenger<ClassToGet> toGetter;
  Messenger<ClassToGet> toAsync;
  Messenger<> requestMessenger;
  int timerPeriod;

  void handleRequests(ClassToStore const& storedObject) override
  {
    freeMessageStack(toAsync.receiveAllNodes());
    auto requests = requestMessenger.receiveAllNodes();
    if (requests) {
      toGetter.send(ClassToGet(storedObject));
      freeMessageStack(requests);
    }
  }

  TBlockingGetter(ClassToStore const& storedObject, int timerPeriod)
    : localCopy{ storedObject }
    , timerPeriod{ timerPeriod }
  {}

public:
  /**
   * Updates the local copy to a synchronized instance of the object stored by
   * the Async instance, or to a ClassToGet object constructed from it. This is
   * the blocking version, which will waits for the syncrhonized instance. Don't
   * call this function while you are using the local copy!
   * @param sleepWhileWaiting if true, the thread will sleep while waiting, if
   * flase, it will do a busy wait in a while(true) loop.
   */
  void update(bool sleepWhileWaiting = true)
  {
    using std::swap;
    requestMessenger.send('x');
    while (true) {
      auto messageNode = toGetter.receiveLastNode();
      if (messageNode) {
        auto& message = messageNode->get();
        swap(localCopy, message);
        toAsync.send(messageNode);
        return;
      }
      if (sleepWhileWaiting) {
        std::chrono::milliseconds interval{ timerPeriod + 1 };
        std::this_thread::sleep_for(interval);
      }
    }
  }

  /**
   * @return a reference to the local copy.
   */
  ClassToGet& get() { return localCopy; }
};

/**
 * A class to share and syncrhonize the state of an object among multiple
 * threads, in a lock-free and non-blocking manner.
 * @tparam TClassToStore the type of the object to share.
 */
template<class TClassToStore>
class Async final
{
public:
  using ClassToStore = TClassToStore;
  using Message = std::function<void(TClassToStore&)>;
  using GetterInterface = TGetterInterface<ClassToStore>;
  using BlockingGetterInterface = TBlockingGetterInterface<ClassToStore>;

  template<class TClassToGet = TClassToStore>
  using Getter = TGetter<TClassToGet, ClassToStore>;

  template<class TClassToGet = TClassToStore>
  using BlockingGetter = TBlockingGetter<TClassToGet, ClassToStore>;

private:
  struct AwaiterFakeStorage final
  {
    AwaiterFakeStorage(TClassToStore const&) {}
  };
  using AwaiterInternal = TBlockingGetter<AwaiterFakeStorage, ClassToStore>;

public:
  class Awaiter final
  {
    AwaiterInternal* awaiter;

  public:
    /**
     * Makes the current thread await for all already submitted messages to be
     * handled.
     * @param sleepWhileWaiting if true, the thread will sleep while waiting, if
     * flase, it will do a busy wait in a while(true) loop.
     */
    void await(bool sleepWhileWaiting = true)
    {
      awaiter->update(sleepWhileWaiting);
    }

    /**
     * Check if the awaiter is valid. Awaiters will always be valid if requested
     * while the Async's timer is not running, and will always be invalid if
     * requested while the Async's timer is running. Calling await on an invalid
     * Awaiter will try to dereference a nullptr.
     */
    operator bool() { return awaiter != nullptr; }

    /**
     * Awaiter constructor. Can only be used internally by the Async.
     */
    Awaiter(AwaiterInternal* awaiter)
      : awaiter{ awaiter }
    {}
  };

private:
  Messenger<Message> messenger;

  std::thread timer;
  std::atomic<bool> stopTimerFlag;
  std::atomic<int> timerPeriod;
  std::atomic<bool> isRunningFlag;
  std::atomic<bool> flipFlop;

  ClassToStore storedObject;
  std::function<void(ClassToStore&)> onChange;

  std::vector<std::unique_ptr<GetterInterface>> getters;
  std::vector<std::unique_ptr<BlockingGetterInterface>> blockingGetters;

  std::vector<std::unique_ptr<MessageBuffer<Message>>> buffers;

  bool handleMessages()
  {
    int numMessages = receiveAndHandleMessageStack(
      messenger, [&](MessageNode<Message>* messageNode) {
        auto message = std::move(messageNode->get());
        message(storedObject);
      });
    return numMessages > 0;
  }

  void timerCallback()
  {
    for (auto& getter : getters) {
      getter->cleanup();
    }
    bool anyChange = handleMessages();
    if (anyChange) {
      if (onChange) {
        onChange(storedObject);
      }
      for (auto& getter : getters) {
        getter->onChange(storedObject);
      }
    }
    for (auto& getter : blockingGetters) {
      getter->handleRequests(storedObject);
    }
    for (auto& buffer : buffers) {
      buffer->maintenance();
    }
  }

public:
  /**
   * Sends a message already wrapped in a MessageNode. non-blocking.
   * @param messageNode the message node to send.
   */
  void submitMessage(MessageNode<Message>* messageNode)
  {
    messenger.send(messageNode);
  }

  /**
   * Sends a message that is not already wrapped in a MessageNode. This should
   not be called from realtime threads, because a MessageNode may be allocated
   under the hood (it will be allocated when there are no available ones, which
   will happen if messages are sent more often than how often they are handled
   by the timerCallback).
   * @param message the message to send.
   */
  void submitMessage(Message&& message) { messenger.send(std::move(message)); }

  /**
   * Requests an Getter. This should not be called while the Async's timer
   * is running.
   * @tparam TClassToGet a type that can be constructed from the ClassToStore,
   * to store what the access point needs from the ClassToStore.
   * @return pointer to the requested Getter, or nullptr if the Async's timer is
   * running
   */
  template<class TClassToGet = TClassToStore>
  Getter<TClassToGet>* requestGetter()
  {
    if (isRunningFlag) {
      return nullptr;
    }

    getters.push_back(
      std::unique_ptr<GetterInterface>(new Getter<TClassToGet>(storedObject)));
    return static_cast<Getter<TClassToGet>*>(getters.back().get());
  }

  /**
   * Requests a BlockingGetter. This should not be called while the Async's
   * timer is running.
   * @tparam TClassToGet a type that can be constructed from the ClassToStore,
   * to store what the access point needs from the ClassToStore.
   * @return pointer to the requested BlockingGetter, or nullptr if the
   * Async's timer is running
   */
  template<class TClassToGet = TClassToStore>
  BlockingGetter<TClassToGet>* requestBlockingGetter()
  {
    if (isRunningFlag) {
      return nullptr;
    }
    blockingGetters.push_back(std::unique_ptr<BlockingGetterInterface>(
      new BlockingGetter<TClassToGet>(storedObject, timerPeriod.load())));
    return static_cast<BlockingGetter<TClassToGet>*>(
      blockingGetters.back().get());
  }

  /**
   * An awaiter is like a blocking access points, but it does not own a copy of
   * the stored object. You can use it to wait for the next iteration of the
   * timerCallback.
   * @return an Awaiter object
   */
  Awaiter requestAwaiter()
  {
    if (isRunningFlag) {
      return { nullptr };
    }
    blockingGetters.push_back(std::unique_ptr<BlockingGetterInterface>(
      new AwaiterInternal(storedObject, timerPeriod.load())));
    return { static_cast<AwaiterInternal*>(blockingGetters.back().get()) };
  }

  /**
   * Requests a MessageBuffer. This should not be called while the Async's timer
   * is running.
   * @see MessageBuffer
   * @param initializer functor used by the message buffer to initialize its
   * nodes
   * @param numPreallocatedNodes number of nodes preallcoated by the message
   * buffer
   * @param minNumNodes number of nodes at which the message buffer will
   * replenish its nodes.
   * @return a pointer to the MessageBuffer, or nullptr if the Async's timer is
   * running
   */
  MessageBuffer<Message>* requestMessageBuffer(
    std::function<Message(void)> initializer,
    int numPreallocatedNodes,
    int minNumNodes)
  {
    if (isRunningFlag) {
      return nullptr;
    }
    buffers.push_back(std::make_unique<MessageBuffer<Message>>(
      numPreallocatedNodes, minNumNodes, initializer));
    return buffers.back().get();
  }

  /**
   * Returns a preallocated node.
   */
  MessageNode<Message>* getMessageNodeFromStorage()
  {
    auto head = messenger.PopStorage();
    if (!head) {
      return nullptr;
    }
    messenger.Recycle(head->next());
    head->next() = nullptr;
    return head;
  }

  /**
   * Starts the Async's timer, spawning its timer thread.
   */
  void startTimer()
  {
    if (isRunningFlag) {
      return;
    }
    stopTimerFlag = false;
    timer = std::thread([this]() {
      while (true) {
        timerCallback();
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

  /**
   * Stops the Async's timer, joining its timer thread.
   */
  void stopTimer()
  {
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
    timerPeriod = period;
    for (auto& getter : blockingGetters) {
      getter.timerPeriod = period;
    }
  }

  /**
   * @return true if the timer thread is running, false otherwise.
   */
  bool isRunning() { return isRunningFlag; }

  /**
   * Constructor.
   * @param storedObject the ClassToStore object to share
   * @param onChange functor called on the ClassToStore whenever it
   * is updated by messages
   * @param timerPeriod period of the timer thread that handles the messages.
   * @param numNodesToPreallocate number of message nodes to preallocate.
   * @param messageInitialzier functor to initialize the preallocated nodes.
   */
  explicit Async(
    ClassToStore storedObject,
    std::function<void(ClassToStore&)> onChange = nullptr,
    int timerPeriod = 50,
    int numNodesToPreallocate = 32,
    std::function<Message(void)> messageInitialzier =
      [] { return Async<TClassToStore>::Message(); })
    : storedObject{ std::move(storedObject) }
    , onChange{ onChange }
    , timerPeriod{ timerPeriod }
    , isRunningFlag{ false }
  {
    messenger.preallocateNodes(numNodesToPreallocate, messageInitialzier);
  }

  ~Async() { stopTimer(); }
};

} // namespace lockfree
