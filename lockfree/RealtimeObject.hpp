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
#include <mutex>

namespace lockfree {

/**
 * A wrapper to manage an object that needs to be used by one real-time thread, and that it is created and modified by
 * one or more non real-time threads.
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
  Object* getOnRealtimeThread()
  {
    auto head = messengerForNewObjects.receiveAllNodes();
    if (head) {
      std::swap(realtimeInstance, head->get());
      messengerForOldObjects.sendMultiple(head);
    }
    return realtimeInstance.get();
  }

  /**
   * Changes the object from the non-realtime thread.
   * @oaram change the std::function that creates the new version of the object
   */
  void change(std::function<std::unique_ptr<Object>(Object const&)> const& changer)
  {
    auto const lock = std::lock_guard<std::mutex>(mutex);
    auto objectPtr = getOnNonRealtimeThread();
    if (!objectPtr)
      return;
    auto newObject = changer(*objectPtr);
    lastObject = newObject.get();
    send(std::move(newObject));
  }

  /**
   * Conditionally changes the object from a the non-realtime thread.
   * @oaram change the std::function that creates the new version of the object
   * @oaram predicate the predicate used to decide if the change is to be applied
   * @return true if the predicate returned true and the change was applied, false otherwise
   */
  bool changeIf(std::function<std::unique_ptr<Object>(Object const&)> const& changer,
                std::function<bool(Object const&)> const& predicate)
  {
    auto const lock = std::lock_guard<std::mutex>(mutex);
    auto objectPtr = getOnNonRealtimeThread();
    if (!objectPtr)
      return false;
    if (predicate(*objectPtr)) {
      auto newObject = changer(*objectPtr);
      lastObject = newObject.get();
      send(std::move(newObject));
      return true;
    }
    return false;
  }

  /**
   * Gets the last version of the object.
   * @return a pointer to the object
   */
  Object* getOnNonRealtimeThread()
  {
    return lastObject;
  }

  /**
   * Gets the last version of the object.
   * @return a pointer to the object
   */
  Object const* getOnNonRealtimeThread() const
  {
    return lastObject;
  }

  /**
   * Sets the object and it sends it to the real-time thread. Also frees any object that has been previously discarded
   * from the real-time thread.
   * @newObject the new version of the object
   */
  void set(std::unique_ptr<Object> newObject)
  {
    auto const lock = std::lock_guard<std::mutex>(mutex);
    lastObject = newObject.get();
    send(std::move(newObject));
  }

  /**
   * Constructor.
   * @param object the object to hold
   * @param numNodesToPreallocate the number of nodes to allocate.
   */
  explicit RealtimeObject(std::unique_ptr<Object> object)
    : realtimeInstance(std::move(object))
  {
    lastObject = realtimeInstance.get();
  }

private:
  void send(std::unique_ptr<Object> newObject)
  {
    messengerForOldObjects.discardAndFreeAllMessages();
    messengerForNewObjects.send(std::move(newObject));
  }

  lockfree::Messenger<std::unique_ptr<Object>> messengerForNewObjects;
  lockfree::Messenger<std::unique_ptr<Object>> messengerForOldObjects;
  std::unique_ptr<Object> realtimeInstance;
  Object* lastObject{ nullptr };
  std::mutex mutex;
};

} // namespace lockfree
