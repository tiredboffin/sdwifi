# sdwifi

simple http server to upload files to [FYSETC SD WiFi Pro card](https://github.com/FYSETC/SD-WIFI-PRO)

## description
 
The main use case for this project is my research on custom boot code for Fujifilm cameras. I needed an easy way to wirelessly transfer experimental firmware files to and from an SD card inserted in the camera. Without this setup, I would have to physically remove and transfer the SD card every time I needed to update or collect files.

While my use case is pretty specific, this server could still be useful in other simple scenarios—provided you can reasonably ensure the SD card isn't actively being used by the host (like a camera, printer, etc.) while the server is accessing it. Alternatively, it could work if the host can handle occasional SD card access errors gracefully.

The code is based on standard Arduino/ESP32 sketches and the orginal FYSETC [firmware](https://github.com/FYSETC/SdWiFiBrowser) and tested on FYSETC SD Wifi Pro card rev 1.1

![image](https://github.com/user-attachments/assets/bac7d2be-150f-4b85-b5eb-42dadb482aca)


## build instructions

- clone the repository 

- compile and upload with arduino-cli (assuming the port is /dev/ttyUSB)

      cd sdwifi/
      ./sketch build 
      ./sketch upload

- run monitor

      ./sketch monitor

- if successful you should see "HTTP server started" message:

      [   118][I][sdwifi.ino:167] setupAP(): Soft AP created: sdwifi
      [   131][I][sdwifi.ino:140] setup(): HTTP server started

## configuration

By default, the server starts as an unprotected WiFi Access Point with the default name "sdwifi" and IP address 192.168.4.1. Once connected to the access point, the WiFi mode ("access point" or "station"), SSID, password, and IP settings can be changed using the "config" command, for e.g.:

    curl "http://192.168.4.1/confiig?ap_ssid=myssid&ap_password=mypassword"

(see more example in the usage section). 

Alternatively, sdwifi_config.ini file can eb prepared on a PC and placed in the root folder of the SD card. 

Sample sdwifi_config.ini:
```
[WiFi]
sta_ssid=<SSID>
sta_password=<password>
```
Once the SD card is safely unmounted and reinserted, the server will automatically pick up the settings from the .ini file.

## usage

The server accepts the following commands:

 #### file operations

     upload   - upload a file to SD card
     download - download a file
     remove   - remove a file
     rename   - rename a file
     sha1     - get sha1 sum of a file
     list     - get file info

 #### directory operations

     mkdir    - create a directory on the SD card
     rmdir    - remove a directory (has to be empty)
     list     - get all file info in a directory
 
 #### config   - change wifi settings or local time
 
    param=value
 
 #### sysinfo  - get general info about the server

 #### ping     - returns 0200 empty response

 #### exp      - experimental commands
 
    power=off|reset
    sleep=sense|ms (sense puts ESP32 to sleep until there is an activity on D3/CS SD finger, ms -- sleep for N ms)

 ## examples
   
   Change AP name and AP password
   
    curl http://192.168.4.1/config?ap_ssid=sdwifi4&ap_password=012345678
 
   Change wifi mode to STA
    
    curl http://192.168.4.1/config?sta_ssid=mywifinetwork&sta_password=mywifinetworkpassword
 
   Reset wifi settings to default (AP mode, no password)
    
    curl http://sdwifi.local/config?reset=all
 
   or reset a parameter selectively 
   
    curl http://sdwifi.local/config?reset=sta_ssid
 
   or set time with
   
    curl http://sdwifi.local/config?time=$(date -Iseconds)
    curl http://sdwifi.local/config?time="20204-12-14T22:10:12"
    

   Download a file uilog.bin 

    curl http://sdwifi.local/download?path=uilog.bin -O -J

   Upload file xe2-ffboot.bin as ffboot.bin to the root of SD card
    
     curl -T xe2-ffboot.bin http://192.168.4.1/upload/ffboot.bin
 
   or when in STA mode
   
    curl -T xe2-ffboot.bin http://sdwifi.local/upload/ffboot.bin

   or with POST (from a Form)
   
    curl -F "data=@xe2-ffboot.bin" "http://sdwifi.local/upload?path=ffboot.bin
   
   Remove a file
    
    curl http://sdwifi.local/remove?path=ffboot.bin
 
   Get sha1 sum of a file
    
    curl http://sdwifi.local/sha1?path=ffboot.bin | jq
 
   Rename a file
    
    curl http://sdwifi.local/rename?from=ffboot_new.bin&to=ffboot.bin
 
   Create a directory
    
    curl http://sdwifi.local/mkdir?path=boot
  
   Remove a directory
    
    curl http://sdwifi.local/rmdir?path=boot

   Give permanent control over SD to ESP32, blocks access to NAND SD card from the host side
     
    curl http://sdwifi.local/exp?io26=esp32
 
   Relinquish SD control
    
    curl http://sdwifi.local/exp?io26=host
 
   Reboot ESP32
    
    curl http://sdwifi.local/exp?power=reset

   Get some info
    
    curl http://sdwifi.local/sysinfo | jq

   Get a list of files in / directory
    
    curl http://sdwifi.local/list?path=/ | jq

## web application

The server can also be used in a browser with the original [firmware](https://github.com/FYSETC/SdWiFiBrowser) web application.

To build the filesystem with the web app:
  - download the SdWifiBroser firmware source code
  - copy SDWifiBrowser/data directory into sdwifi/data
  - execute

        ./fs build upload


