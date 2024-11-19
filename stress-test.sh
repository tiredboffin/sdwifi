#!/bin/bash
CURL_OPTIONS="-s -o /dev/null -w %{http_code}"
echo "Press [CTRL+C] to stop.."
i=0
elapsed=0
printf -v start '%(%s)T'
TEST_TIME=3600
while [ $elapsed -lt $TEST_TIME ];
do
    rc=`curl $CURL_OPTIONS http://sdwifi.local/sysinfo`
#   rc=`curl $CURL_OPTIONS http://sdwifi.local/list?path=/`
#   rc=`curl $CURL_OPTIONS http://sdwifi.local/ping`
    if [[ ! "$rc" == "200" ]];
    then
	echo "curl got $rc"
	break
    fi
    i=$((i+1))
    if [ $((i % 10)) -eq 0 ];
    then
	printf -v now '%(%s)T'
	elapsed=$((now-start))
	echo "$i reqs in $elapsed sec"
    fi
done