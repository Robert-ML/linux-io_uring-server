## Linux `io_uring` server

Asynchronous Linux web server using the `io_uring` async IO API.

All blocking IO operations (on sockets and on files) are replaced with asynchronous operations through the use of the `io_uring` API (the library `liburing` 2.1 was used for easy interfacing and testing). This ensures the server is not stuck at any time waiting for IO and is always serving clients and new connections. The data transfer and IO operations are deferred to the kernel.

The server delivers preexisting files to connecting users. There are two locations and types of files:

- `static/*` - Files that need no processing and are static in nature. They are mapped directly into the server's memory and transferred to the requesting clients through zero copy mechanism if the library version of the runner allows for it.

- `dynamic/*` - Files that mimic needing processing and are loaded on demand by the server. They are loaded into memory asynchronously and on completion are sent to the requesting user.


### Building

1. `cd ./build` - enter build directory

2.
- `cmake -DCMAKE_BUILD_TYPE=Debug ..` - for debug version

- `cmake -DCMAKE_BUILD_TYPE=Release ..` - for release version

3. `make`


### Running

1. `cd ./bin` - enter the directory with the executable

2. `./server` - run the executable


### Testing

- `wget http://127.0.0.1:8888/static/example1.txt` - to download a static file from the server that was maped in its memory

- `wget http://127.0.0.1:8888/dynamic/clasic_greeting.txt` - to download a dynamic file from the server

Additional files can be added in these folders to be downloaded by clients. To load the new files in the static folder, restart the server. The files in the static folder need to have a valid HTTP header and formatting.


### Improvements

1. Make the user_data passed to the `io_uring` submission queue not be reallocated for each submission, but reuse it through the stages of a client connection and file transmission.

2. Test the zero copy mechanism is doing what it is supposed to do.
