# sdwifi

simple http server to upload files to [FYSETC SD WiFi Pro card](https://github.com/FYSETC/SD-WIFI-PRO)

## description
 
The primary use case is my research project on Fujifilm camera custom boot code. Fro the project I was looking for a convenient wireless way to transfer experimental files to and from SD card that is inserted into the camera slot when the camera's native WiFi cannot be used for that purpose. Otherwise to update/collect the files I had to physically transfser the SD card itself.

While the use case is very specific to my needs the server may still prove to be useful in other similar simple scenarios when it could be reasonably guaranteed by some means that the SD card is not used actively by the host (camera, printer etc) at the time this server is accessing the card. Or if the host is capable to recover gracefully from sporadic SD card access errors.

The code is based on standard Arduino/ESP32 sketches and the orginal FYSETC [firmware](https://github.com/FYSETC/SdWiFiBrowser). 

## usage

By default the server starts as unprotected Wifi Access Point with defaul name "sdwifi" and ip address 192.168.4.1 and then it 
can be configured to use Wifi STA mode.

The server accepts the following commands:


#### file operations

     upload   - upload a file to SD card
     download - download a file
     remove   - remove a file
     rename   - rename a file
     sha1     - get sha1 sum of a file
     list     - get dir/file info
 
 #### config   - change wifi settings
 
    param=value
 
 #### info     - get general info about the server
 
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

   Upload file xe2-ffboot.bin as ffboot.bin to the root of SD card
    
     curl -T xe2-ffboot.bin http://192.168.4.1/upload/ffboot.bin
 
   or when in STA mode
   
    curl -T xe2-ffboot.bin http://sdwifi.local/upload/ffboot.bin
   
   Remove a file
    
    curl http://sdwifi.local/remove/ffboot.bin
 
   Get sha1 sum of a file
    
    curl http://sdwifi.local/sha1/ffboot.bin
 
   Rename a file
    
    curl http://sdwifi.local/rename?from=ffboot_new.bin&to=ffboot.bin
 
   Give permanent control over SD to ESP32, blocks access to NAND SD card from the host side
     
    curl http://sdwifi.local/exp?io26=esp32
 
   Relinquish SD control
    
    curl http://sdwifi.local/exp?io26=host
 
   Reboot ESP32
    
    curl http://sdwifi.local/exp?power=reset
 
 
