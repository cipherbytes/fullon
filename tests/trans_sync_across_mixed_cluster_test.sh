#!/bin/bash

###############################################################
# Test for validating transaction synchronization across cluster nodes.
# Nodes can be producing or non-producing.
# -p <producing nodes count>
# -n <total nodes>
# -s <topology>
# -d <delay between nodes startup>
###############################################################

pnodes=1
topo=mesh
delay=1

args=`getopt p:n:s:d: $*`
if [ $? == 0 ]; then

    set -- $args
    for i; do
        case "$i"
        in
            -p) pnodes=$2;
                shift; shift;;
            -n) total_nodes=$2;
                shift; shift;;
            -d) delay=$2;
                shift; shift;;
            -s) topo="$2";
                shift; shift;;
            --) shift;
                break;;
        esac
    done
else
    echo "huh we got err $?"
    if [ -n "$1" ]; then
        pnodes=$1
        if [ -n "$2" ]; then
            topo=$2
            if [ -n "$3" ]; then
		total_nodes=$3
            fi
        fi
    fi
fi

total_nodes="${total_nodes:-`echo $pnodes`}"

# $1 - error message
error()
{
  (>&2 echo $1)
  killAll
  echo =================================================================
  exit 1
}

verifyErrorCode()
{
  rc=$?
  if [[ $rc != 0 ]]; then
    error "FAILURE - $1 returned error code $rc"
  fi
}

killAll()
{
  programs/gax-launcher/gax-launcher -k 15
}

cleanup()
{
    rm -rf etc/gax/node_*
    rm -rf var/lib/node_*
}


# result stored in HEAD_BLOCK_NUM
getHeadBlockNum()
{
  INFO="$(programs/gaxcli/gaxcli get info)"
  verifyErrorCode "gaxcli get info"
  HEAD_BLOCK_NUM="$(echo "$INFO" | awk '/head_block_num/ {print $2}')"
  # remove trailing coma
  HEAD_BLOCK_NUM=${HEAD_BLOCK_NUM%,}
}

waitForNextBlock()
{
  getHeadBlockNum
  NEXT_BLOCK_NUM=$((HEAD_BLOCK_NUM+1))
  while [ "$HEAD_BLOCK_NUM" -lt "$NEXT_BLOCK_NUM" ]; do
    sleep 0.25
    getHeadBlockNum
  done
}

# $1 - string that contains "transaction_id": "<trans id>", in it
# result <trans id> stored in TRANS_ID
getTransactionId()
{
  TRANS_ID="$(echo "$1" | awk '/transaction_id/ {print $2}')"
  # remove leading/trailing quotes
  TRANS_ID=${TRANS_ID#\"}
  TRANS_ID=${TRANS_ID%\",}
}

INITA_PRV_KEY="5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3"

# cleanup from last run
cleanup

# stand up gaxnod cluster
launcherOpts="-p $pnodes -n $total_nodes -s $topo -d $delay"
echo Launcher options: --gaxnod \"--plugin eosio::wallet_api_plugin\" $launcherOpts
programs/gax-launcher/gax-launcher --gaxnod "--plugin eosio::wallet_api_plugin" $launcherOpts
sleep 7

startPort=8888
endport=`expr $startPort + $total_nodes`
echo startPort: $startPort
echo endPort: $endPort

# basic cluster validation
port2=$startPort
while [ $port2  -ne $endport ]; do
    echo Request block 1 from node on port $port2
    TRANS_INFO="$(programs/gaxcli/gaxcli --port $port2 get block 1)"
    verifyErrorCode "gaxcli get block"
    port2=`expr $port2 + 1`
done

# create 3 keys
KEYS="$(programs/gaxcli/gaxcli create key)"
verifyErrorCode "gaxcli create key"
PRV_KEY1="$(echo "$KEYS" | awk '/Private/ {print $3}')"
PUB_KEY1="$(echo "$KEYS" | awk '/Public/ {print $3}')"
KEYS="$(programs/gaxcli/gaxcli create key)"
verifyErrorCode "gaxcli create key"
PRV_KEY2="$(echo "$KEYS" | awk '/Private/ {print $3}')"
PUB_KEY2="$(echo "$KEYS" | awk '/Public/ {print $3}')"
KEYS="$(programs/gaxcli/gaxcli create key)"
verifyErrorCode "gaxcli create key"
PRV_KEY3="$(echo "$KEYS" | awk '/Private/ {print $3}')"
PUB_KEY3="$(echo "$KEYS" | awk '/Public/ {print $3}')"
if [ -z "$PRV_KEY1" ] || [ -z "$PRV_KEY2" ] || [ -z "$PRV_KEY3" ] || [ -z "$PUB_KEY1" ] || [ -z "$PUB_KEY2" ] || [ -z "$PUB_KEY3" ]; then
  error "FAILURE - create keys"
fi


# create wallet for inita
PASSWORD_INITA="$(programs/gaxcli/gaxcli wallet create --name inita)"
verifyErrorCode "gaxcli wallet create"
# strip out password from output
PASSWORD_INITA="$(echo "$PASSWORD_INITA" | awk '/PW/ {print $1}')"
# remove leading/trailing quotes
PASSWORD_INITA=${PASSWORD_INITA#\"}
PASSWORD_INITA=${PASSWORD_INITA%\"}
programs/gaxcli/gaxcli wallet import --name inita --private-key $INITA_PRV_KEY
verifyErrorCode "gaxcli wallet import"
programs/gaxcli/gaxcli wallet import --name inita --private-key $PRV_KEY1
verifyErrorCode "gaxcli wallet import"
programs/gaxcli/gaxcli wallet import --name inita --private-key $PRV_KEY2
verifyErrorCode "gaxcli wallet import"
programs/gaxcli/gaxcli wallet import --name inita --private-key $PRV_KEY3
verifyErrorCode "gaxcli wallet import"

#
# Account and Transfer Tests
#

# create new account
echo Creating account testera
ACCOUNT_INFO="$(programs/gaxcli/gaxcli create account inita testera $PUB_KEY1 $PUB_KEY3)"
verifyErrorCode "gaxcli create account"
waitForNextBlock
# verify account created
ACCOUNT_INFO="$(programs/gaxcli/gaxcli get account testera)"
verifyErrorCode "gaxcli get account"
count=`echo $ACCOUNT_INFO | grep -c "staked_balance"`
if [ $count == 0 ]; then
  error "FAILURE - account creation failed: $ACCOUNT_INFO"
fi

pPort=$startPort
port=$startPort
echo Producing node port: $pPort
while [ $port  -ne $endport ]; do

    echo Sending transfer request to node on port $port.
    TRANSFER_INFO="$(programs/gaxcli/gaxcli transfer inita testera 975321 "test transfer")"
    verifyErrorCode "gaxcli transfer"
    getTransactionId "$TRANSFER_INFO"
    echo Transaction id: $TRANS_ID

    echo Wait for next block
    waitForNextBlock

    port2=$startPort
    while [ $port2  -ne $endport ]; do
	echo Verifying transaction exists on node on port $port2
   TRANS_INFO="$(programs/gaxcli/gaxcli --port $port2 get transaction $TRANS_ID)"
   verifyErrorCode "gaxcli get transaction trans_id of <$TRANS_INFO> from node on port $port2"
	port2=`expr $port2 + 1`
    done

    port=`expr $port + 1`
done

killAll
cleanup
echo SUCCESS!
