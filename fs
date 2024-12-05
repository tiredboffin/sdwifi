#!/usr/bin/env sh
#Note: I believe there should be a more straightforward way to build and upload the filesystem 
#that is as a part of Arduino project. The idea below is to use arduino-cli to get the 
#build properties and then extact the partition parameters.
#

TMPDIR=./tmp
FQBN=esp32:esp32:pico32:PartitionScheme=no_ota,DebugLevel=info

if [ -f set-tty-ports.sh ]; then
    . ./set-tty-ports.sh
fi
PORT=${SDWIFI_DEV:-/dev/ttyUSB0}
PORTM=${USBTTL_DEV:-${PORT}}


if [ -z "$1" ]; then
	echo "Usage: fs build|upload|erase"
	exit
fi

PROPS=$(TMPDIR=$TMPDIR arduino-cli compile --fqbn $FQBN --show-properties --json)
#PROPS=$(TMPDIR=$TMPDIR arduino-cli compile --profile pico32 --show-properties --json)

MKSPIFFS_PATH=`echo $PROPS | jq -r -c ".builder_result.build_properties|map(select((contains(\"runtime.tools.mkspiffs.path\"))))[0]|split(\"=\")[1]"`
BUILD_PATH=`echo $PROPS | jq -r -c ".builder_result.build_path"`
BUILD_PROJECT_NAME=`echo $PROPS | jq -rc ".builder_result.build_properties|map(select(contains(\"build.project_name\")))[0]|split(\"=\")[1]"`
FQBN=`echo $PROPS | jq -rc ".builder_result.build_properties|map(select(contains(\"build.fqbn\")))[0]|split(\"=\")[1]"`
PARTITION_TABLE=$BUILD_PATH/partitions.csv
cat $PARTITION_TABLE
tmp=(${FQBN//:/ })
OUTPUT_PATH=build/$(IFS=.; printf '%s' "${tmp[*]:0:3}" )

tmp=`cat ${PARTITION_TABLE} | grep "^spiffs"|cut -d"," -f4-5 | xargs`
tmp=(${tmp//,/ })
SPIFFS_START=${tmp[0]}
SPIFFS_SIZE=${tmp[1]}

while read -r line
do
    name=$(echo "$line" | awk -F',' '{printf "%s%s", $1,$3}')
    if [[ $name == "nvs nvs" ]]; then
        NVS_OFFSET_SIZE=$(echo "$line" | awk -F',' '{printf "%s %s", $4, $5}')
        break
    fi
done < "$PARTITION_TABLE"

if [[ "$1" == b* ]]; then
    $MKSPIFFS_PATH/mkspiffs -c data --page 256 --block 4096 --size $((SPIFFS_SIZE)) $OUTPUT_PATH/$BUILD_PROJECT_NAME.filesystem.bin
    retVal=$?
    if [ $retVal -ne 0 ]; then
        exit
    fi
    echo "Filesystem saved to $OUTPUT_PATH/$BUILD_PROJECT_NAME.filesystem.bin"
    shift
fi

if [[ "$1" == up* ]]; then
    esptool --port ${PORT} write_flash $SPIFFS_START $OUTPUT_PATH/$BUILD_PROJECT_NAME.filesystem.bin
    shift
fi

if [[ "$1" == "erase" ]]; then
    esptool --port ${PORT} --chip esp32 erase_flash
    shift
fi

if [[ "$1" == "read_part" ]]; then
    esptool --port ${PORT} read_flash 0x8000 0x1000 part_table.bin
    shift
fi

if [[ "$1" == "read_nvs" ]]; then
    if [[ -z $NVS_OFFSET_SIZE ]]; then
	echo "ERROR: set NVS offset and size"
	exit
    fi
    esptool --port ${PORT} read_flash ${NVS_OFFSET_SIZE} nvs_readout.bin
    shift
fi

if [[ "$1" == "erase_nvs" ]]; then
    if [[ -z $NVS_OFFSET_SIZE ]]; then
	echo "ERROR: set NVS offset and size"
	exit
    fi
    esptool --port ${PORT} erase_region ${NVS_OFFSET_SIZE}
    shift
fi
