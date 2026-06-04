Async sockets wrapper in fibers
=============

### Introduction

Folly offers two main classes to handle async TCP communications: AsyncSocket to implement asynchronous reads/writes and AsyncServerSocket to listen, accept and distribute accepted sockets across multiple IO threads. All the logic uses inversion of control via callbacks, the usual event-driven style. AsyncServerSocket has sophisticated communicaiton logic (via NotificationQueue) to pass accepted connections into new threads.

Wangle offers concept of future/promise that helps in better handling the callback logic by attaching handlers in chains via the .then() primitive. It also has higher-level abstractions like Pipelines that allows for inserting re-usable input and output handlers (filters) for structured IO handling.

All of the above still does not offer the "synchronous" feel where you invoke the read()/write() and continue execution in the same logical thread instead of delegating (inverting) control to the IO loop. `folly::fibers` library offers such opportunity by allowing for in-thread scheduler that permits maininting multiple "green" threads (aka fibers or co-routines) each maintaining its own stack. Fibers may voluntarily "suspend" themselves, passing control to the scheduler and allowing other fibers to run. Every fiber is allocated a fixed amount of stack memory, which is the major caveat with fibers - if you create too many fibers you may run out of memory. Btw In golang this problem is avoided by implementing dynamic stack growing on heap and splitting it when needed to add more space. It's "easy" in Go since they created a new language, but it also has performance downsides.

Anyways, fibers allow running multiples "tasks" within same thread, yielding control to each other via the fiber manager that handles the scheduling logic. The key primitive used to accomplish this is `folly::fibers::Baton`, which supports wait() and post() operations, allowing for one-time handshake/signal between two fibers. All other communications constructs build on top of this simple logic. Batons are thread safe, so you can use them to communicate among fibers in different threads too.

NOTE: The "stack" mentioned above is not implcitly visible to gdb - you would need to load special gdb scripts (coming with fibers library) to inspect state of fibers running on the thread and dump their stack contexts.

Look up folly::fibers wiki for more information. Keep in mind that fibers allow you to integrate waiting on IO and signal event using the same logic. For example, there is a convenient "await" primitive that allows you to wrap asynchronous IO operation running in EventBase. You would need fiber manager driven by event-base loop to make that work. It is also possible to run fibers without IO interaction, using generic fiber manager without LoopController.

### Whats inside

The code here is rather simple. It wraps the callbacks usually triggered by AsyncSocket and AsyncServerSocket and offers "synchronous" calls experience. For example, you may call "read" on the FiberSocket, and if it would need to block it will register a callback with EventBase using AsyncSocket's API and then yield to the scheduler, allowing for other fibers to have their chance on execution. From the perspecitve of the yielding fiber, it would resume when the operation completes.

Notice that you can similarly await on AsyncTimeout, if you want to, or await on multiple async events using WhenN, CollectN and ForEach. The FiberSocket constructor takes existing AsyncSocket object, which is expected to be already connected. You can also produce new connected socket using the connect() factory method or by obtaining FiberSocket via calling accept() on FiberServerSocket. Since FiberSocket can wrap AsyncSocket, you are responsible for closing it - either by calling the helper close() method on FiberSocket, or closing the underlying AsyncSocket.

The FiberServerSocket's function is to accept connections and build new FiberSockets for each one. Unlike FiberSocket, it does not wrap an existing AsyncServerSocket, but creates and maintains one internally. You call the accept() method to suspend current fiber until new connection has arrived. Every accept() pulls the next accepted connections and creates a FiberSocket for it. Normally that pattern is that you run accept() in one thread and then spawn new fiber to handle the connection, while accepting sockets in another fiber.

This works really nice with TCP SO_REUSEPORT, i.e. you can have bunch of IO threads running accept() loop and handling the IO in the same thread. The original acceptor logic was to distribute load across different IO threads, but SO_REUSPORT delegates the load-balancing logic to the kernel. Actually I liked that so much that SO_REUSEPORT is always enabled, and we always accept in the same event loop where ServerSocket is running. Accepting in the same event loop also saves the intermediate transition via NotificationQueue.

### Inter-fiber communications

There are two simple higher-level primitives that build on concepts familiar to people who used Golang or gevent() in python. Channel offers an one-way synchronized communication facility: it has read and write endpoints, and only two fibers can use it. Every put() and get() are blocking.

Queue is more flexible with some added overhead of maintaining internal data structures. It allows multiple publishing fibers talk to multiple consuming fibers. Queue may be unbounded, in which case publishers can put data without constraints, or boudned, where publishers start to queue up and wait on consumers to read. Notice that consumer and publisher queues are unbounded, so in theory it's possible to have unbounded queue of publishers waiting for consumers :)

Queue is an implementation detail, which is present as shared_ptr inside RWQueue or WQueue/RQueue. The latter offer the actual interface, clearly telling the available API to the user. The front-end classes support move and copy construction, where copy construction creates another view of the same underlying queue (shared_ptr is copied). Copying the front-end classes does never create a new underlying Queue.

Queues are intended to be mostly used in 1:1 communication scenarios, though many-to-many and other variants are possible. The operation "putNull" results in putting empty folly::Optional object inside the data queue, and one of the readers getting a "null" value. This could be used as a sentinel value telling the consumer to stop consuming. In case of multiple consumers, multiple sentinel values need to be posted.

### Caveats

- Fiber sockets must be created inside fibers, for safety - they inherit the event-base that drives the fiber manager. This also reduces the number of arguments passed :)
- Listening FiberServerSocket always enables SO_REUSEPORT so you can run the server socket in multiple threads on the same port. This requires proper kernel support, you'd better have it!
- FiberServerSocket is set to accept SINGLE connection per event loop run. This may somewhat limit scalability, but honestly the current design is not intended to be used for very high rate connection handling.

### Combinators

There are "combinators" - higher oreder functions that could be used to transform queues in helpful ways. The below quickly documents them:

- mergeQueues: take first/last iterators and reference to an output queue. Reads data from all input queues in parallels, and pipes into single output queue. Notice that input and output queues need to have the same element type. You can use std::variant<> to merge queues with different payloads - e.g. queue1 would use "int" and queue2 "string" discriminated type in the algebraic data-type. There are two versions of the merge routine - one merges queues of same element type T (often a variant type) into queue of same element type. This is because we cannot infer the resulting type automatically. The "static" version merges fixed number of input queues of possibly varying types with single output type std::variant(T1, T2, T3,...).

All of the combinators need to be invoked inside a new fiber, since they run their own processing loops - the combinators do not create "wrapping" fibers themselves.
