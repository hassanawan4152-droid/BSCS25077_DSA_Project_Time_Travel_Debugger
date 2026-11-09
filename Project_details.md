# Time-Travel Debugger (TTDB)

A client-server debugger for a small custom language (C--).
The client sends a program, the server runs it and returns a .tdbg file,
and the client can then step forward and backward through the execution.

## Supported Platforms
Windows and Ubuntu.

## Components
- server: runs the program and writes the .tdbg file
- load_balancer: queues clients and passes messages to the server
- client: the command line debugger

## Build

### Ubuntu
g++ -std=c++11 -pthread Server.cpp -o server
g++ -std=c++11 -pthread load_balancer.cpp -o load_balancer
g++ -std=c++11 -pthread client.cpp -o client

### Windows (Visual Studio)
1. Create one project for each program: Server, Load Balancer and Client.
2. Add the matching .cpp file and helper.h to each project.
3. Build all projects (Build > Rebuild Solution).
4. Put the three executables in one folder and name them server.exe,
   load_balancer.exe and client.exe. The load balancer starts server.exe
   from its own folder, so the name and the folder must be correct.

## Run
Ubuntu:
1. ./load_balancer 127.0.0.1 5001
2. ./client 127.0.0.1 5000

Windows:
1. load_balancer.exe 127.0.0.1 5001
2. client.exe 127.0.0.1 5000

The load balancer takes the server address and port. The client takes the
load balancer address and port (the load balancer listens on port 5000).
The load balancer starts the server automatically when it is needed.

Then type: start source.bin, next, prv, jmp next 3, jmp prv 2, quit

## Known limitations
- The checksum is a simple sum and cannot detect swapped bytes.

## Authors
Hassan, BSCS, ITU
