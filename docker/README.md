
### Steps to Build sdwifi Arduino Sketch in a Container:

There are many ways to build Arduino sketches, but to ensure consistency and avoid dependencies on installed libraries and tools, it’s occasionally beneficial to use a container.

### 1. **Build the Container Image**

First, build the base container image (once):

```bash
./build-image-base
```

### 2. **Install Arduino-CLI and ESP32 Tools to container**

Run the script to install Arduino-CLI and the necessary ESP32 tools:

```bash
./run-script install-arduino-cli.sh
```

This script installs the tools in the default "ubuntu" user home directory inside the container. The container’s `/home/ubuntu` directory is mapped to the `./user` directory on the host.
The `./user` directory serves as a shared volume between the container and host, where all necessary files and build outputs are saved.


### 3. **Build the sdwifi Project**

Now, build the sdifi project using the following script that simply pulls the latest master branch and runs ./sketch build:

```bash
./run-script build-sdwifi.sh
```

The final built output will be located at:

```
./user/sdwifi/build/esp32.esp32.pico32
```

