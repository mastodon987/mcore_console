# mcore_console
simple Linux client console for Meshcore Linux variant.
It is designed for connecting into meshcore native console where user can read/write configurations and states.

## Usage

Filename is compiled as 'mcore' where if used make install it's loaded into /usr/local/bin directory
for connecting to meshcored instance is by default used Linux sockets /run/meshcored/meshcored.sock.
If multiple instances are running you can use -s param for connecting into another instances.
Maybe not all commands are working as expected.

## Screenshot
<img width="654" height="633" alt="Screenshot_15" src="https://github.com/user-attachments/assets/c722e718-7d0d-4eb1-88a7-ac550c2af85c" />
