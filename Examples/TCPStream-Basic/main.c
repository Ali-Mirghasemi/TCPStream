#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "SerialStream.h"

static void printHelp(char* root) {
    printf("Usage: %s <tty> <baudrate>\n\n", root);
}

void Serial_onReceive(StreamIn* stream, Stream_LenType len) {
    while (IStream_available(stream) > 0) {
        putchar(IStream_readChar(stream));
    }
}

int main(int argc, char* argv[]) {
    uint8_t rxBuf[1024];
    uint8_t txBuf[1024];
    SerialStream serial;

    if (argc < 3) {
        printHelp(argv[0]);
        return 1;
    }

    int32_t baudrate = atoi(argv[2]);

    if (!SerialStream_init(
        &serial, 
        argv[1], baudrate,
        rxBuf, sizeof(rxBuf),
        txBuf, sizeof(txBuf)
    )) {
        perror("Failed to initialize serial port");
        return -1;
    }

    IStream_onReceive(&serial.Input, Serial_onReceive);

    while (1) {
        char c = getchar();
        OStream_writeChar(&serial.Output, c);
        OStream_flush(&serial.Output);
    }
}

