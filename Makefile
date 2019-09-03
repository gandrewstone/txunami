
#BU_DIR=/fast/bitcoin/buclean/release
BU_DIR:=BitcoinUnlimited/src

INC_PATHS:=-I$(BU_DIR)/../../src/univalue/include -I$(BU_DIR)/../../src/cashlib -I$(BU_DIR)/../../src -I$(BU_DIR) -I$(BU_DIR)/config
LIB_PATHS:=-L. -L$(BU_DIR)/.libs -L$(BU_DIR)/univalue/.libs

STD_LIBS:=-lboost_system -lpthread

GCC:=g++ -g -Wall -c -std=c++14
LINK:=g++ -g -std=c++14

all: txunami 

txunami: main.o libbitcoincash.so.0
	$(LINK) -o txunami $< -L$(BU_DIR)/.libs  -lbitcoincash $(BU_DIR)/univalue/.libs/libunivalue.a $(STD_LIBS)

libbitcoincash.so.0: $(BU_DIR)/.libs/libbitcoincash.so.0
	cp $(BU_DIR)/.libs/libbitcoincash.so.0 .

# Static link missing some symbols: $(LINK) -o txunami $< -L$(BU_DIR)/.libs  $(BU_DIR)/.libs/libbitcoincash.a $(BU_DIR)/univalue/.libs/libunivalue.a $(BU_DIR)/secp256k1/.libs/libsecp256k1.a $(STD_LIBS)

%.o: %.cpp
	$(GCC) -o $@ $(INC_PATHS) $< 
