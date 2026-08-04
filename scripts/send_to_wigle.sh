#!/bin/bash

for file in /path/to/files/*.csv
do
if [ -f "$file" ]
then
curl -X POST "https://api.wigle.net/api/v2/file/upload"\
 -H "accept: application/json"\
 -H "Content-Type: multipart/form-data"\
 -H "Authorization: Basic <you authorization token>
 -F "file=@$file;type=application/vnd.ms-excel"\
 -F "donate=on"
    if [ $? -eq 0 ]
    then
    mv $file /path/to/saved/files/
    fi
fi
done
