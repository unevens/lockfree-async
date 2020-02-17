# lockfree-async

[lockfree-async](https://github.com/unevens/lockfree-async) is a C++ header-only simple template library for lock-free inter-thread communication.

The fundamental building block of this library is the "IMB Freelist" multiple-producer multiple-consumer LIFO stack, as implemented by Ross Bencina in [Queue World](https://github.com/RossBencina/QueueWorld).

## Design

This library is designed to solve this problem:

```
We have an object, whose state can be changed from many threads, which is also used by one (or more) realtime threads.
In the realtime threads no blocking action should be executed, so we can't prevent data races by using a mutex.
```

Here's the general idea of how this library solves the problem:

- Let's call `State` the class of the object we want to share.
- The object is not directly visible, but is managed by an instance of the class `Async<State>`.
- The `Async<State>` instance keeps a private copy of the `State` object
- For each thread that needs to acces the `State` object, a `Getter` object can be requested to the `Async<State>` instance. 
- Each getter keeps its local copy of the `State` object - or, optionally, of an object of an other type that can be constructed from the `State` object. The `Getter` object have two methods: `get`, which will return a reference to the local copy of the `State` object, and `update`, which will synchronize the local copy with the last available version of the `State` object kept by the `Async<State>` instace. 
- `Async<State>`  has a method called `SubmitMessage`, which can be called from any thread, that takes a `std::function<void(State&)>` to asynchronously change the state of the object. Let's call a `std::function<void(State&)>` a _message_.
- The messages are received and executed using a timer thread. They are executed on the private copy of the `State` object owned by the `Asyn<State>` istance. 
- Then, the timer thread will update the local copies of the `State` object kept by the getters using a lockfree FIFO stack to avoid data races. All memory allocations and deallocations are executed by the timer thread. 

Read on for a more detailed explanation.

### Messages

Messages are used to asynchronously change the state of the State.

But what is a message?
A message is a `std::function<void(State&)>`.

Just put the code to change you want to execute on the `State` object in a `std::function<void(State&)>` and pass it to the `Async<State>` instance using one of its `SubmitMessage` methods.

If you need to send such messages from realtime threads, you can preallocate them using the function `Async<State>::RequestMessageBuffer`.

The Messages are handled in a timer thread. You can set its period in the constructor of `Async<State>`, or by calling `SetTimerPeriod`.

The constructor of `Async<State>` also has an optional argument of type `std::function<void(State&)>` called `onChange`. If not null, it will be executed on the `State` object by timer callback if any message has been received.

### Getters

`Getter` objects can be reqeusted using the method `RequestGetter<ClassToGet>` of `Async<State>`. The template parameter `ClassToGet` is the type that the `get` method of the getter will return. By default it is `State`, but it can be anything that can be constructed from a `State const&`. 

When the timer thread of the `Async<State>` handles the messages and updates its private `State` object, it will also update the copies of the `State` owned by the Getters - more precisely, the new versions will be sent to the `Getter`s using a the lock-free lifo-stack, to avoid data races.

The method `update` of the `Getter` swaps its local copy of the `State` with the last version that the server has sent to it using the the lifo-stack.

The local copy will have been updated by all the messages sent **before** the last iteration of the timer.

However, the messages sent *after the last iteration of the `Async<State>` timer*, even if they were sent *before the call to `update`*, will not have been handled yet. 

Messages are, after all, asynchronous.

In non realtime threads however, you might want to wait for all the messages already sent to the `Async<State>` to be handled before `update` returns. 

In such case, instead of a `Getter`, you have to request a `BlockingGetter` using the method `RequestBlockingGetter`.

The method `update` of a `BlockingGetter` will wait for the next timer iteration, so that all messages already submitted to the `Async<State>` will have been handled before it returns.

### Awaiter

An `Awaiter` is an object with only one method: `await`, which will make the current thread wait for all the already submitted `Messages` to be handled by the `Async<State>`, by waiting for the completion of the next timer iteration. Each `Awaiter` object should only be used by one thread at the same time, as it is for the `Getter`s. 

## LockFreeMessenger.h

The template class `LockFreeMessenger<T>` is a wrapper around the Queue World's multiple-producer multiple-consumer LIFO stack. It implements functionality to send and receive data of type `T` between threads in a lock-free way, and to preallocate the resources to do so. 

## Documentation

The documentation, available at https://unevens.github.io/lockfree-async/, can be generated with [Doxygen](http://doxygen.nl/) running

```bash
$ doxygen doxyfile.txt
```
