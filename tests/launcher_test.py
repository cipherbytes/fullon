#!/usr/bin/env python3

import decimal
import re
import os

from TestHarness import Cluster, Node, TestHelper, Utils, WalletMgr, CORE_SYMBOL
from pathlib import Path

###############################################################
# launcher-test
#
# Specifically tests using the bios bootstrap script that is created by gax-launcher
#
###############################################################

Print=Utils.Print
errorExit=Utils.errorExit
cmdError=Utils.cmdError

args = TestHelper.parse_args({"--defproducera_prvt_key","--dump-error-details","--dont-launch","--keep-logs",
                              "-v","--leave-running","--clean-run","--unshared"})
debug=args.v
defproduceraPrvtKey=args.defproducera_prvt_key
dumpErrorDetails=args.dump_error_details
keepLogs=args.keep_logs
dontLaunch=args.dont_launch
dontKill=args.leave_running
killAll=args.clean_run

Utils.Debug=debug
cluster=Cluster(walletd=True, defproduceraPrvtKey=defproduceraPrvtKey,unshared=args.unshared)
walletMgr=WalletMgr(True)
testSuccessful=False
killEosInstances=not dontKill
killWallet=not dontKill

WalletdName=Utils.EosWalletName
ClientName="gaxcli"
timeout = .5 * 12 * 2 + 60 # time for finalization with 1 producer + 60 seconds padding
Utils.setIrreversibleTimeout(timeout)

try:
    TestHelper.printSystemInfo("BEGIN")

    cluster.setWalletMgr(walletMgr)

    if not dontLaunch:
        cluster.killall(allInstances=killAll)
        cluster.cleanup()
        Print("Stand up cluster")
        pnodes=4
        abs_path = os.path.abspath(os.getcwd() + '/unittests/contracts/eosio.token/eosio.token.abi')
        traceNodeosArgs=" --trace-rpc-abi gax.token=" + abs_path
        if cluster.launch(pnodes=pnodes, totalNodes=pnodes, extraNodeosArgs=traceNodeosArgs) is False:
            cmdError("launcher")
            errorExit("Failed to stand up eos cluster.")
    else:
        walletMgr.killall(allInstances=killAll)
        walletMgr.cleanup()
        cluster.initializeNodes(defproduceraPrvtKey=defproduceraPrvtKey)
        killEosInstances=False

        print("Stand up walletd")
        if walletMgr.launch() is False:
            cmdError("%s" % (WalletdName))
            errorExit("Failed to stand up eos walletd.")

    Print("Validating system accounts after bootstrap")
    cluster.validateAccounts(None)

    accounts=Cluster.createAccountKeys(3)
    if accounts is None:
        errorExit("FAILURE - create keys")
    testeraAccount=accounts[0]
    testeraAccount.name="testera11111"
    currencyAccount=accounts[1]
    currencyAccount.name="currency1111"
    exchangeAccount=accounts[2]
    exchangeAccount.name="exchange1111"

    PRV_KEY1=testeraAccount.ownerPrivateKey
    PUB_KEY1=testeraAccount.ownerPublicKey
    PRV_KEY2=currencyAccount.ownerPrivateKey
    PUB_KEY2=currencyAccount.ownerPublicKey
    PRV_KEY3=exchangeAccount.activePrivateKey
    PUB_KEY3=exchangeAccount.activePublicKey

    testeraAccount.activePrivateKey=currencyAccount.activePrivateKey=PRV_KEY3
    testeraAccount.activePublicKey=currencyAccount.activePublicKey=PUB_KEY3

    exchangeAccount.ownerPrivateKey=PRV_KEY2
    exchangeAccount.ownerPublicKey=PUB_KEY2

    testWalletName="test"
    Print("Creating wallet \"%s\"." % (testWalletName))
    testWallet=walletMgr.create(testWalletName, [cluster.eosioAccount,cluster.defproduceraAccount])

    Print("Wallet \"%s\" password=%s." % (testWalletName, testWallet.password.encode("utf-8")))

    for account in accounts:
        Print("Importing keys for account %s into wallet %s." % (account.name, testWallet.name))
        if not walletMgr.importKey(account, testWallet):
            cmdError("%s wallet import" % (ClientName))
            errorExit("Failed to import key for account %s" % (account.name))

    defproduceraWalletName="defproducera"
    Print("Creating wallet \"%s\"." % (defproduceraWalletName))
    defproduceraWallet=walletMgr.create(defproduceraWalletName)

    Print("Wallet \"%s\" password=%s." % (defproduceraWalletName, defproduceraWallet.password.encode("utf-8")))

    defproduceraAccount=cluster.defproduceraAccount

    Print("Importing keys for account %s into wallet %s." % (defproduceraAccount.name, defproduceraWallet.name))
    if not walletMgr.importKey(defproduceraAccount, defproduceraWallet):
        cmdError("%s wallet import" % (ClientName))
        errorExit("Failed to import key for account %s" % (defproduceraAccount.name))

    node=cluster.getNode(0)

    Print("Validating accounts before user accounts creation")
    cluster.validateAccounts(None)

    # create accounts via gax as otherwise a bid is needed
    Print("Create new account %s via %s" % (testeraAccount.name, cluster.eosioAccount.name))
    transId=node.createInitializeAccount(testeraAccount, cluster.eosioAccount, stakedDeposit=0, waitForTransBlock=False, exitOnError=True)

    Print("Create new account %s via %s" % (currencyAccount.name, cluster.eosioAccount.name))
    transId=node.createInitializeAccount(currencyAccount, cluster.eosioAccount, buyRAM=1000000, stakedDeposit=5000, exitOnError=True)

    Print("Create new account %s via %s" % (exchangeAccount.name, cluster.eosioAccount.name))
    transId=node.createInitializeAccount(exchangeAccount, cluster.eosioAccount, buyRAM=1000000, waitForTransBlock=True, exitOnError=True)

    Print("Validating accounts after user accounts creation")
    accounts=[testeraAccount, currencyAccount, exchangeAccount]
    cluster.validateAccounts(accounts)

    Print("Verify account %s" % (testeraAccount))
    if not node.verifyAccount(testeraAccount):
        errorExit("FAILURE - account creation failed.", raw=True)

    transferAmount="97.5321 {0}".format(CORE_SYMBOL)
    Print("Transfer funds %s from account %s to %s" % (transferAmount, defproduceraAccount.name, testeraAccount.name))
    node.transferFunds(defproduceraAccount, testeraAccount, transferAmount, "test transfer", waitForTransBlock=True)

    expectedAmount=transferAmount
    Print("Verify transfer, Expected: %s" % (expectedAmount))
    actualAmount=node.getAccountEosBalanceStr(testeraAccount.name)
    if expectedAmount != actualAmount:
        cmdError("FAILURE - transfer failed")
        errorExit("Transfer verification failed. Excepted %s, actual: %s" % (expectedAmount, actualAmount))

    transferAmount="0.0100 {0}".format(CORE_SYMBOL)
    Print("Force transfer funds %s from account %s to %s" % (
        transferAmount, defproduceraAccount.name, testeraAccount.name))
    node.transferFunds(defproduceraAccount, testeraAccount, transferAmount, "test transfer", force=True, waitForTransBlock=True)

    expectedAmount="97.5421 {0}".format(CORE_SYMBOL)
    Print("Verify transfer, Expected: %s" % (expectedAmount))
    actualAmount=node.getAccountEosBalanceStr(testeraAccount.name)
    if expectedAmount != actualAmount:
        cmdError("FAILURE - transfer failed")
        errorExit("Transfer verification failed. Excepted %s, actual: %s" % (expectedAmount, actualAmount))

    Print("Validating accounts after some user transactions")
    accounts=[testeraAccount, currencyAccount, exchangeAccount]
    cluster.validateAccounts(accounts)

    transferAmount="97.5311 {0}".format(CORE_SYMBOL)
    Print("Transfer funds %s from account %s to %s" % (
        transferAmount, testeraAccount.name, currencyAccount.name))
    trans=node.transferFunds(testeraAccount, currencyAccount, transferAmount, "test transfer a->b", waitForTransBlock=True)
    transId=Node.getTransId(trans)

    expectedAmount="98.0311 {0}".format(CORE_SYMBOL) # 5000 initial deposit
    Print("Verify transfer, Expected: %s" % (expectedAmount))
    actualAmount=node.getAccountEosBalanceStr(currencyAccount.name)
    if expectedAmount != actualAmount:
        cmdError("FAILURE - transfer failed")
        errorExit("Transfer verification failed. Excepted %s, actual: %s" % (expectedAmount, actualAmount))

    node.waitForTransactionInBlock(transId)

    transaction=node.getTransaction(transId, exitOnError=True, delayedRetry=False)

    typeVal=None
    amountVal=None
    key=""
    try:
        key = "[actions][0][action]"
        typeVal = transaction["actions"][0]["action"]
        key = "[actions][0][params][quantity]"
        amountVal = transaction["actions"][0]["params"]["quantity"]
        amountVal = int(decimal.Decimal(amountVal.split()[0]) * 10000)
    except (TypeError, KeyError) as e:
        Print("transaction%s not found. Transaction: %s" % (key, transaction))
        raise

    if typeVal != "transfer" or amountVal != 975311:
        errorExit("FAILURE - get transaction trans_id failed: %s %s %s" % (transId, typeVal, amountVal), raw=True)

    Print("Bouncing nodes #00 and #01")
    if cluster.bounce("00,01") is False:
        cmdError("launcher bounce")
        errorExit("Failed to bounce eos node.")

    Print("Taking down node #02")
    if cluster.down("02") is False:
        cmdError("launcher down command")
        errorExit("Failed to take down eos node.")

    Print("Using bounce option to re-launch node #02")
    if cluster.bounce("02") is False:
        cmdError("launcher bounce")
        errorExit("Failed to bounce eos node.")

    p = re.compile('Assert')
    errFileName=f"{cluster.nodeosLogPath}/node_00/stderr.txt"
    assertionsFound = False
    with open(errFileName) as errFile:
        for line in errFile:
            if p.search(line):
                assertionsFound=True

    if assertionsFound:
        # Too many assertion logs, hard to validate how many are genuine. Make this a warning
        #  for now, hopefully the logs will get cleaned up in future.
        Print(f"WARNING: Asserts in {cluster.nodeosLogPath}/node_00/stderr.txt")
        #errorExit("FAILURE - Assert in launcher_test.py/node_00/stderr.txt")

    Print("Validating accounts at end of test")
    accounts=[testeraAccount, currencyAccount, exchangeAccount]
    cluster.validateAccounts(accounts)

    testSuccessful=True
finally:
    TestHelper.shutdown(cluster, walletMgr, testSuccessful, killEosInstances, killWallet, keepLogs, killAll, dumpErrorDetails)

exitCode = 0 if testSuccessful else 1
exit(exitCode)