#!/usr/bin/env bash

set -euo pipefail

replace() {
    printf "\x00\x00\x00\x00\x00\x00\x00\x00" | cat - "$1"
}

append() {
    printf "\x00\x00\x00\x00\x00\x00\x00\x01" | cat - "$1"
}

write_var() {
    var=$1
    input=$2
    replace $input > /tmp/$var.in
    cat /tmp/$var.in > $VARS_DIR/$var/update
}

read_var() {
    var=$1
    tail -c +5 $VARS_DIR/$var/data > /tmp/$var.out
}

echo 9 > /proc/sysrq-trigger

BASE_DIR=/sys/firmware/secvar
CONFIG_DIR=$BASE_DIR/config
VARS_DIR=$BASE_DIR/vars

printf "Reading config vars:\n\n"
grep . $CONFIG_DIR/*

#printf "\nReading KEK:\n\n"
#tail -c +5 $VARS_DIR/KEK/data >/tmp/KEK.out

printf "\nWriting KEK:\n"
printf "test data woopedy doooooo" > /tmp/test
write_var KEK /tmp/test

printf "\nReading KEK back:\n"
read_var KEK
diff /tmp/test /tmp/KEK.out

printf "\nWriting db with 8k size object:\n"
write_var db 8k.txt

printf "\nReading db back:\n"
read_var db
diff /tmp/db.out 8k.txt

printf "\nWriting KEK with 8k size object:\n"
write_var KEK 8k.txt

printf "\nReading KEK back:\n"
read_var KEK
diff /tmp/KEK.out 8k.txt
