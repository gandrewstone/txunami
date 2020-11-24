# txunami
An extremely high throughput Bitcoin Cash transaction generator.

Txunami is capable of delivering over 50000 transactions per second to a locally running bitcoind using 8 threads of a modern processor.  Txunami delivers the transactions through the P2P network, which is a more accurate simulation of a large network of users than injecting transactions via bitcoind's RPC interface.

Txunami allows the user to specify a multi-step transaction injection scenario, specifying the transaction rate to multiple hosts, and allowing either simultaneous or sequential operation.

Txunami stores all wallet state in RAM and just quits when done -- **all of the coins used in a run will be lost**.  It is expected that this tool is run on test blockchains with ample worthless coins.  However, future versions could store final UTXO state to disk or consolidate UTXOs into a single final output to conserve coins an clean up the UTXO.

## Building


### Bitcoin Unlimited

Txunami uses Bitcoin Unlimited's libbitcoincash.so and libunivalue.a shared libraries.  Therefore it is necessary to first build Bitcoin Unlimited.  Bitcoin Unlimited has been added as a submodule so to check it out use:
```git submodule update --init --recursive```

It is then necessary to build BitcoinUnlimited with the "--enable-shared" flag set in configure.  Please read https://github.com/BitcoinUnlimited/BitcoinUnlimited/blob/release/doc/build-unix.md for more details. For most purposes, the build process will look like this: 

```
cd BitcoinUnlimited
git checkout dev  # Optional - checkout the latest enchancements; if that doesn't work, swap "dev" for "release"
sudo apt-get install build-essential libtool autotools-dev automake pkg-config libssl-dev libevent-dev bsdmainutils libboost-system-dev libboost-filesystem-dev libboost-chrono-dev libboost-program-options-dev libboost-test-dev libboost-thread-dev
./autogen.sh
./configure --disable-wallet --with-gui=no --enable-shared
make
```

To speed up make, you can run parallel compilation with the `-j N` flag, where N is the number of threads you want (put in your number of cores-1 for a safe default)

If you have a separate Bitcoin Unlimited build directory, you can choose to not use the submodule.  Instead edit txunami/Makefile and change the BU_DIR variable to point to your Bitcoin Unlimited build source directory, or override BU_DIR on the make command line.  For example, If you are doing an "in-source-tree" build, this directory is "BitcoinUnlimited/src", but if you ran an "out-of-source-tree" build, say into the "BitcoinUnlimited/release" subdirectory, this directory is "BitcoinUnlimited/release/src".  

### Txunami

Once libunivalue.a, libbitcoincash.so and the Bitcoin Unlimited headers are available, run "make" to build the txunami executable.

Since out-of-tree builds can be confusing here's a concrete example:  If you checked out txunami as a sibling of your bitcoinUnlimited code which is checked out in the "bu" directory, and you have completed an out-of-source build in the bu/release directory, to build txunami use:
```make BU_DIR=../bu/release/src```

Libbitcoincash.so is also copied from the BitcoinUnlimited build tree into this directory.


## Configuration

Txunami uses a JSON based configuration file to define a test scenario that must be named "txunami.json".  The txunami_example_config.json file documents the configuration fields, and is itself a valid JSON configuration file (all fields named "_" are documentation, and can be removed).

Txunami is not good at indicating where your JSON has a syntax error.  To discover JSON formatting errors use:
```jsonlint-py --allow=duplicate-keys txunami.json```


On Ubuntu distros, ```apt-get install python-demjson``` will get you this tool.


## Running

To execute, run:
```LD_LIBRARY_PATH=<path to libbitcoincash.so> ./txunami```



## Operation


Txunami operates in 2 phases; preparation and testing.

### Preparation
During the preparation phase, Txunami reads UTXOs and private keys from its configuration file, and spends these in nested 1-to-many transactions to generate a specified quantity of UTXOs to be used during the test proper.  Please look at your debug.log file to ensure that these transactions are actually accepted.

One common problem is that Txunami exceeds your configured unconfirmed transaction chain length, especially when generating millions of UTXOs.  To resolve this, change your bitcoin.conf file with the following fields:
limitdescendantcount=5000000
limitdescendantsize=250100

Another problem is paying too much fee -- since Txunami generates a LOT of UTXOs and a LOT of child transactions, much of the balance can be bled away as fees.  You should configure your bitcoind to accept free transactions.  In Bitcoin Unlimited, this can be accomplished by:
minlimitertxfee=0
maxlimitertxfee=0

### Testing

In the test phase, Txunami reads the "schedule" field from the JSON configuration file.  This field specifies what transaction rate to send to what nodes at what times.  Txunami operates by first finding the total number of schedule entities (time interval, host pairs), and splitting the pool of UTXOs evenly between them.
It then spawns a thread for every schedule entity.  The thread sleeps until the entity is meant to go "live".  It then opens a P2P connection to the targeted node and starts sending 1 input, 1 output transactions to the targeted host, spending the UTXOs given to it to new TXOs.  Once all UTXOs are consumed, it creates unconfirmed chains of transactions by swapping the TXOs with the UTXOs and continuing.

It uses a leaky bucket algorithm to precisely control the rate of transaction generation.  When the end time specified in the schedule entity is reached, the connection is disconnected and the thread quits.  When all threads quit, the run is complete and Txunami ends.

At this point, there is no way to recover any funds left in UTXOs.  If needed, it would not be too hard to either write all these UTXOs to disk, or implement a UTXO sweep phase that combines all this dust back into a few UTXOs sent to addresses in the configuration file.

