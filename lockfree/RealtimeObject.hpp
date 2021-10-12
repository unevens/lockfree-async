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
#include <cassert>

namespace lockfree {

/**
 * A wrapper to manage an object that has to be used by 1 real-time thread, and that it is created and modified by 1 non
 * real-time thread.
 * */
template<class Object>
class RealtimeObject final
{
public:
  /**
   * Gets the object on the realtime thread. It updates it if a new version has been submitted, and send the old version
   * back to the non real-time thread to be freed. Lock-free.
   * @return a pointer to the object
   */
  Object* getFromRealtimeThread()
  {
    auto newObject = messengerForNewObjects.receiveLastMessage();
    if (newObject) {
      messengerForOldObjects.send(std::move(currentObjectStorage));
      currentObjectStorage = std::move(newObject.value());
      currentObjectPtr.store(currentObjectStorage.get(), std::memory_order_release);
    }
    return currentObjectStorage.get();
  }

  /**
   * Changes the object from a the non-realtime thread, making a copy, applying the change to the copy and sending it to
   * the realtime thread. The change is a std::function<void(Object&)>.
   * @oaram change an std::function that will apply the change
   */
  void changeFromNonRealtimeThread(std::function<void(Object&)> const& change)
  {
    auto objectPtr = getFromNonRealtimeThread();
    if (!objectPtr)
      return;
    auto objectCopy = std::make_unique<Object>(*objectPtr);
    change(*objectCopy);
    set(std::move(objectCopy));
  }

  /**
   * Conditionally changes the object from a the non-realtime thread, making a copy, applying the change to the copy and
   * sending it to the realtime thread, if a predicate returns true.
   * @oaram change an std::function that will apply the change
   * @oaram predicate the predicate used to decide if the change is to be applied
   * @return true if the predicate returned true and the change was applied, false otherwise
   */
  bool changeFromNonRealtimeThreadIf(std::function<void(Object&)> const& change,
                                     std::function<bool(Object const&)> const& predicate)
  {
    auto objectPtr = getFromNonRealtimeThread();
    if (!objectPtr)
      return false;
    if (predicate(*objectPtr)) {
      auto objectCopy = std::make_unique<Object>(*objectPtr);
      change(*objectCopy);
      set(std::move(objectCopy));
      return true;
    }
    return false;
  }

  /**
   * Gets the object in use on the real-time thread form a non realtime thread.
   * @return a pointer to the object
   */
  Object* getFromNonRealtimeThread()
  {
    return currentObjectPtr.load(std::memory_order_acquire);
  }

  /**
   * Sets the object and it sends it to the real-time thread. Also frees any object that has been previously discarded
   * from the real-time thread.
   * @newObject the new version of the object
   */
  void set(std::unique_ptr<Object> newObject)
  {
    lockfree::receiveAndHandleMessageStack(messengerForOldObjects, [](auto& object) { object.reset(); });
    messengerForNewObjects.send(std::move(newObject));
  }

  /**
   * Allocates message nodes.
   * @param numNodesToPreallocate the number of nodes to allocate.
   */
  void allocateMessageNodes(int numNodesToPreallocate)
  {
    messengerForNewObjects.allocateNodes(numNodesToPreallocate);
    messengerForOldObjects.allocateNodes(numNodesToPreallocate);
  }

  /**
   * Constructor.
   * @param object the object to hold
   * @param numNodesToPreallocate the number of nodes to allocate.
   */
  explicit RealtimeObject(std::unique_ptr<Object> object, int numNodesToPreallocate = 128)
    : currentObjectStorage(std::move(object))
  {
    currentObjectPtr.store(currentObjectStorage.get(), std::memory_order_release);
    allocateMessageNodes(numNodesToPreallocate);
  }

private:
  lockfree::Messenger<std::unique_ptr<Object>> messengerForNewObjects;
  lockfree::Messenger<std::unique_ptr<Object>> messengerForOldObjects;
  std::unique_ptr<Object> currentObjectStorage;
  std::atomic<Object*> currentObjectPtr{ nullptr };
};

} // namespace lockfree
