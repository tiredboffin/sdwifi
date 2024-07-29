#!/usr/bin/env sh
#Note: I believe there should be a more straightforward way to build and upload the filesystem 
#that is as a part of Arduino project. The idea below is to use arduino-cli to get the 
#build properties and then extact the partition parameters.
#

TMPDIR=./tmp
FQBN=esp32:esp32:pico32:PartitionScheme=no_ota,DebugLevel=info

if [ -z "$1" ]; then
	echo "Usage: fs build|upload"
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
    esptool --port /dev/ttyUSB0 write_flash $SPIFFS_START $OUTPUT_PATH/$BUILD_PROJECT_NAME.filesystem.bin
    shift
fi
