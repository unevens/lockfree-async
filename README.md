# lockfree-async

[lockfree-async](https://github.com/unevens/lockfree-async) is a C++ header-only simple template library for lock-free
inter-thread communication and sharing of resources.

The fundamental building block of this library is the "IMB Freelist" multiple-producer multiple-consumer LIFO stack, as
implemented by Ross Bencina in [Queue World](https://github.com/RossBencina/QueueWorld).

## Messenger.hpp

The template class `Messenger<T>` is a wrapper around the Queue World's multiple-producer multiple-consumer LIFO stack.
It implements functionality to send and receive data of type `T` between threads in a lock-free way, and to preallocate
the resources to do so.

## RealtimeObject.hpp

The template class `RealtimeObject<T>` owns an object of class `T` which can be shared between a non realtime thread and a
realtime thread, so that the non realtime thread can perform any blocking operation, such as creating the object, and
the realtime thread can use the object.

## AsyncObject.hpp

The template class `AsyncObject` manages asynchronous creation, modification, and destruction of several instances of an object, that can be
used from realtime threads, while being created, modified and destroyed by a timer thread.