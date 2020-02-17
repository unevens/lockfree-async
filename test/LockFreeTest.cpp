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

#include "LockFreeAsync.hpp"
#include <chrono>
#include <iostream>
#include <sstream>

class Backend final
{
  int state;

public:
  int getState() { return state; };
  void incrementState(int amount = 1) { state += amount; };
  Backend(int state = 0)
    : state(state)
  {}
};

static_assert(std::is_move_constructible<Backend>::value,
              "Backend should be noexcept move constructible");

static_assert(std::is_move_assignable<Backend>::value,
              "Backend should be noexcept move assignable");

static_assert(std::is_copy_constructible<Backend>::value,
              "Backend should be move constructible");

static_assert(std::is_copy_assignable<Backend>::value,
              "Backend should be move assignable");

using Async = lockfree::Async<Backend>;
using Message = Async::Message;

int
main()
{
  int const serverUpdatePeriodMs = 50;
  int const stateChangePeriodMs = 32;
  int const getterPeriodMs = 20;
  int const blockingGetterPeriodMs = 100;

  auto const OnBackendStateChange = [](Backend& backend) {
    std::cout << "The backend was updated to " +
                   std::to_string(backend.getState()) + " by a message.\n";
  };

  auto asyncBackend =
    Async(Backend{}, OnBackendStateChange, serverUpdatePeriodMs);
  auto getter = asyncBackend.requestGetter();
  auto blockingGetter = asyncBackend.requestBlockingGetter();
  asyncBackend.startTimer();

  std::atomic<bool> runStateChangingThread = true;
  auto stateChangingThread = std::thread([&] {
    while (runStateChangingThread) {
      std::cout << "sending message from state changing thread\n";
      asyncBackend.submitMessage(
        Message([](Backend& backend) { backend.incrementState(); }));
      std::this_thread::sleep_for(
        std::chrono::milliseconds(stateChangePeriodMs));
    }
    std::cout << "state changing thread stopped.\n";
  });

  std::atomic<bool> runGetterThread = true;
  auto getterThread = std::thread([&] {
    while (runGetterThread) {
      getter->update();
      Backend& backend = getter->get();
      std::cout << "from access point thread: " << backend.getState() << "\n";
      std::this_thread::sleep_for(std::chrono::milliseconds(getterPeriodMs));
    }
    std::cout << "access point thread stopped.\n";
  });

  std::atomic<bool> runBlockingThread = true;
  auto blockingGetterThread = std::thread([&] {
    while (runBlockingThread) {
      std::cout << "accessing blocking access point\n";
      blockingGetter->update();
      Backend& backend = blockingGetter->get();
      std::cout << "from blocking access point thread: " << backend.getState()
                << "\n";
      std::this_thread::sleep_for(
        std::chrono::milliseconds(blockingGetterPeriodMs));
    }
    std::cout << "blocking access point thread stopped.\n";
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  std::cout << "main thread slept for 2 second, stopping other threads\n";

  runStateChangingThread = false;
  runGetterThread = false;
  runBlockingThread = false;

  std::cout << "joining threads\n";
  blockingGetterThread.join();
  getterThread.join();
  stateChangingThread.join();
  std::cout << "joined\n";
  std::cout << "stopping asyncBackend\n";
  asyncBackend.stopTimer();
  std::cout << "asyncBackend stopped\n";
  return 0;
}