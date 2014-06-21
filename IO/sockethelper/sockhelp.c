#include "sockhelp.h"
#define _POSIX_C_SOURCE 200812L
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include "order32.h"

#define DEBUG_BYTES 0

/** if endianness needs to be fixed up between client/server */
static bool shouldReencode = true;

void checkEndianness(int socket) {
    //Get own endianness
    uint8_t ownEndianness;
    if(O32_HOST_ORDER == O32_LITTLE_ENDIAN) {
        ownEndianness = 0;
    }
    else if (O32_HOST_ORDER == O32_BIG_ENDIAN) {
        ownEndianness = 1;
    }
    else if (O32_HOST_ORDER == O32_PDP_ENDIAN) {
        ownEndianness = 2;
    }
    else {
        printf("What kind of system is this? cant determine byte order");
        ownEndianness = 3;
    }

    wru8(socket, ownEndianness);

    uint8_t otherEndianness;
    ru8(socket, &otherEndianness);

    if(ownEndianness != otherEndianness
            || ownEndianness == 3
            || otherEndianness == 3) {
        printf("Different byte order between systems, have to reencode data!\n");
        shouldReencode = true;
    }
    else {
        printf("Both systems have the same endianness (%d), "
               "don't need to reencode data before transfer.\n", ownEndianness);
        shouldReencode = false;
    }
}

enum NetDataType netTypeForPlainT(struct PlainTypeInfo info) {
    return netTypeForBitWidth(info.bitwidth, info.is_signed, info.is_float);
}

enum NetDataType netTypeForBitWidth(size_t width, bool is_signed, bool is_float) {
    if(is_float && width == 32)
        return N_FL32;
    else if(!is_float && !is_signed) {
        if(width == 8)
            return N_UINT8;
        else if(width == 16)
            return N_UINT16;
        else if(width == 32)
            return N_UINT32;
    }

    return N_NOT_SUPPORTED;
}

struct PlainTypeInfo bitWidthFromNType(enum NetDataType type) {
    struct PlainTypeInfo retVal;
    if(type == N_FL32) {
        retVal.bitwidth = 32;
        retVal.is_float = true;
        retVal.is_signed = true;
        return retVal;
    }

    retVal.is_float     = false;
    retVal.is_signed    = false;

    if(type == N_UINT8)
        retVal.bitwidth = 8;
    else if(type == N_UINT16)
        retVal.bitwidth = 16;
    else if(type == N_UINT32)
        retVal.bitwidth = 32;
    else
        retVal.bitwidth = 0; // Not supported

    return retVal;
}

/*#################################*/
/*#######       Write       #######*/
/*#################################*/

/* a msgsend for arbitrary data. */
bool wrmsg(int fd, const void* buf, const size_t len) {
  if(fd == -1) { return false; }
  assert(len > 0);
  struct iovec vec[1];
  vec[0].iov_base = (void*)buf;
  vec[0].iov_len = len;
  struct msghdr msg;
  memset(&msg, 0, sizeof(struct msghdr));
  msg.msg_iov = vec;
  msg.msg_iovlen = 1;

#ifdef __APPLE__
  if(sendmsg(fd, &msg, 0) != (ssize_t)len) {
#else //Only available on linux systems
  if(sendmsg(fd, &msg, MSG_NOSIGNAL) != (ssize_t)len) {
#endif

    fprintf(stderr, "error writing %zu-byte buffer at %p: %d\n", len, buf,
            errno);
    return false;
  }
  return true;
}
/* same as write(2), but never reports partial writes. */
bool write2(int fd, const void* buffer_, const size_t len) {
  if(fd == -1) { return false; }
  assert(len > 0);
  const char* buf = buffer_;
  struct pollfd pll;

  pll.fd = fd;
  pll.events = POLLOUT;
  size_t n = 0;
  while(len > n) {
    const ssize_t bytes = write(fd, buf+n, len-n);
    switch(bytes) {
      case -1:
        if(errno == EAGAIN) {
          continue;
        } else if(errno == EWOULDBLOCK) {
          poll(&pll, 1, -1); /* wait until we can send data. */
          continue;
        }
        return false;
      case 0:
        return false;
      default:
        n += bytes;
    }
  }
  return true;
}

bool wru8(int fd, const uint8_t buf) {
  return wr(fd, &buf, sizeof(uint8_t));
}

bool wru16(int fd, const uint16_t buf) {
    if(!shouldReencode)
        return wr(fd, &buf, sizeof(uint16_t));

    const uint16_t data = htons(buf);
    return wr(fd, &data, sizeof(uint16_t));
}

bool wru32(int fd, const uint32_t buf) {
    if(!shouldReencode)
        return wr(fd, &buf, sizeof(uint32_t));

    const uint32_t data = htonl(buf);
    return wr(fd, &data, sizeof(uint32_t));
}

bool wrsizet(int fd, const size_t buf) {
    return wru32(fd, (uint32_t) buf);
}

bool wru8v(int fd, const uint8_t* buf, size_t count) {
    wrsizet(fd, count);//Telling the other side how many elements there are
    return wru8v_d(fd, buf, count);
}

bool wru16v(int fd, const uint16_t* buf, size_t count) {
    wrsizet(fd, count);//Telling the other side how many elements there are
    return wru16v_d(fd, buf, count);
}

bool wru32v(int fd, const uint32_t* buf, size_t count) {
    wrsizet(fd, count);//Telling the other side how many elements there are
    return wru32v_d(fd, buf, count);
}

bool wrf32v(int fd, const float* buf, size_t count) {
    assert(count <= 4294967296);
    wrsizet(fd, count);//Telling the other side how many elements there are
    return wrf32v_d(fd, buf, count);
}

bool wrsizetv(int fd, const size_t *buf, size_t count) {
    wrsizet(fd, count);
    return wrsizetv_d(fd, buf, count);
}

bool wru8v_d(int fd, const uint8_t* buf, size_t count) {
    return wr(fd, buf, sizeof(uint8_t)*count);
}

bool wru16v_d(int fd, const uint16_t* buf, size_t count) {
    if(!shouldReencode) {
        return wr(fd, buf, sizeof(uint16_t)*count);
    }
    else {
        uint16_t* netData = malloc(sizeof(uint16_t)*count);
        for(size_t i = 0; i < count; i++) {
            netData[i] = htons(buf[i]);
        }
        bool retValue = wr(fd, netData, sizeof(uint16_t)*count);
        free(netData);
        return retValue;
    }
}

bool wru32v_d(int fd, const uint32_t* buf, size_t count) {
    if(!shouldReencode) {
        return wr(fd, buf, sizeof(uint32_t)*count);
    }
    else {
        uint32_t* netData = malloc(sizeof(uint32_t)*count);
        for(size_t i = 0; i < count; i++) {
            netData[i] = htonl(buf[i]);
        }
        bool retValue = wr(fd, netData, sizeof(uint32_t)*count);
        free(netData);
        return retValue;
    }
}
bool wrf32v_d(int fd, const float* buf, size_t count) {
    if(!shouldReencode) {
        return wr(fd, buf, sizeof(float)*count);
    }

    float* swapped = malloc(sizeof(float)*count);
    for(float* b = swapped; b < swapped+count; ++b) {
        *b = htonl(*b);
    }
    const bool rv = wr(fd, swapped, sizeof(float)*count);
    free(swapped);
    return rv;
}

bool wrsizetv_d(int fd, const size_t* buf, size_t count) {
    uint32_t newBuffer[count];
    for(size_t i = 0; i < count; i++) {
        newBuffer[i] = (uint32_t)buf[i];
    }
    return wru32v_d(fd, &newBuffer[0], count);
}

bool wrCStr(int fd, const char *cstr) {
    size_t len = strlen(cstr)+1;

    if(!wru16(fd, len) || !wr(fd, cstr, len)) {
        return false;
    }
    return true;
}

/*#################################*/
/*#######       Read        #######*/
/*#################################*/
bool newDataOnSocket(int socket) {
    fd_set rfds;
    struct timeval tv;
    int retval;

    /* Watch socket to see when it has input. */
    FD_ZERO(&rfds);
    FD_SET(socket, &rfds);

    /* Don't wait... */
    tv.tv_sec   = 0;
    tv.tv_usec  = 0;

    retval = select(socket+1, &rfds, NULL, NULL, &tv);
    /* Don't rely on the value of tv now! */

    if (retval == -1) {
        perror("select()");
        return false;
    }
    else if (retval)
        return true;
    else
        return false;
}

int readFromSocket(int socket, void *buffer, size_t len) {
#if DEBUG_BYTES
    printf("Waiting on data from client...(len: %d)\n", len);
#endif

    int byteCount = (int)recv(socket, buffer, len, MSG_WAITALL);
    if (byteCount < 0) {
        fprintf(stderr, "Failed to receive message! (errno %d)\n", errno);
        close(socket);
        exit(EXIT_FAILURE);
    }

#if DEBUG_BYTES
    printf("Received %d bytes from client.\n", byteCount);
#endif

    return byteCount;
}

bool ru8(int socket, uint8_t* value) {
    return 0 < (readFromSocket(socket, value, 1));
}

bool ru16(int socket, uint16_t* value) {
    bool success = 0 < (readFromSocket(socket, value, sizeof(uint16_t)));
    
    if (shouldReencode)
        *value = ntohs(*value);
    
    return success;
}

bool ru32(int socket, uint32_t* value) {
    bool success = 0 < (readFromSocket(socket, value, sizeof(uint32_t)));
    
    if (shouldReencode)
        *value = ntohl(*value);
    
    return success;
}

bool rf32(int socket, float* value) {
    bool success = 0 < (readFromSocket(socket, value, sizeof(float)));

    if (shouldReencode)
        *value = ntohl(*value);

    return success;
}

bool rsizet(int socket, size_t *value) {
    uint32_t tmp_val;
    bool success = ru32(socket, &tmp_val);
    *value = (size_t)tmp_val;
    return success;
}

bool ru8v(int socket, uint8_t** buffer, size_t* count) {
    rsizet(socket, count);
    *buffer = malloc(sizeof(uint8_t)*(*count));
    return ru8v_d(socket, *buffer, *count);
}

bool ru16v(int socket, uint16_t** buffer, size_t* count) {
    rsizet(socket, count);
    *buffer = malloc(sizeof(uint16_t)*(*count));
    return ru16v_d(socket, *buffer, *count);
}

bool ru32v(int socket, uint32_t** buffer, size_t* count) {
    rsizet(socket, count);
    *buffer = malloc(sizeof(uint32_t)*(*count));
    return ru32v_d(socket, *buffer, *count);
}

bool rf32v(int socket, float **buffer, size_t *count) {
    rsizet(socket, count);
    *buffer = malloc(sizeof(float)*(*count));
    return rf32v_d(socket, *buffer, *count);
}

bool rsizetv(int socket, size_t **buffer, size_t *count) {
    rsizet(socket, count);
    *buffer = malloc(sizeof(size_t)*(*count));
    return rsizetv_d(socket, *buffer, *count);
}

bool ru8v_d(int socket, uint8_t* buffer, size_t count) {
    return 0 < (readFromSocket(socket, buffer, sizeof(uint8_t)*count));
}

bool ru16v_d(int socket, uint16_t* buffer, size_t count) {
    bool success = 0 < (readFromSocket(socket, buffer, sizeof(uint16_t)*count));
    if (shouldReencode) {
        for(size_t i = 0; i < count; i++) {
            buffer[i] = ntohs(buffer[i]);
        }
    }
    return success;
}

bool ru32v_d(int socket, uint32_t* buffer, size_t count) {
    bool success = 0 < (readFromSocket(socket, buffer, sizeof(uint32_t)*count));
    if (shouldReencode) {
        for(size_t i = 0; i < count; i++) {
            buffer[i] = ntohl(buffer[i]);
        }
    }
    return success;
}

bool rf32v_d(int socket, float* buffer, size_t count) {
    bool success = 0 < (readFromSocket(socket, buffer, sizeof(float)*count));
    if (shouldReencode) {
        for(size_t i = 0; i < count; i++) {
            buffer[i] = ntohl(buffer[i]);
        }
    }
    return success;
}

//Careful... slow!
bool rsizetv_d(int socket, size_t* buffer, size_t count) {
    uint32_t newBuffer[count];
    bool success = ru32v_d(socket, &newBuffer[0], count);

    if(success) {
        for(size_t i = 0; i < count; i++) {
            buffer[i] = (size_t)newBuffer[i];
        }
    }

    return success;
}

bool rCStr(int socket, char **buffer, size_t *countOrNULL) {
    uint16_t len;
    ru16(socket, &len);
    
    if (countOrNULL != NULL)
        *countOrNULL = len;

    *buffer = malloc(sizeof(char)*len);
    return readFromSocket(socket, *buffer, len);
}
